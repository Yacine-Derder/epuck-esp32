#!/usr/bin/env python3
"""Interactive e-puck client. Never automatically ARM/reconnect/replay movement."""
import argparse
from collections import deque
from dataclasses import dataclass
import os
import queue
import re
import select
import socket
import sys
import threading
import time


def display_record(text, *, logs=False, verbose=False):
    return (logs or not text.startswith("LOG ")) and (verbose or text != "[MOTOR] OK HB")


@dataclass(frozen=True)
class SensorSample:
    sequence: int
    reported_age_ms: int
    epuck_time_ms: int
    proximity: tuple
    ambient_light: tuple
    accelerometer: tuple
    commanded_steps: tuple
    received_at: float
    raw: str


def parse_data(text, now=None):
    words = text.split()
    compact = words[0:1] == ["DATA2"]
    if len(words) != (17 if compact else 25) or words[0] not in ("DATA", "DATA2"):
        raise ValueError("DATA requires 24 fields; DATA2 requires 16 (no light sensors)")
    if not all(re.fullmatch(r"[+-]?[0-9]{1,11}", word) for word in words[1:]):
        raise ValueError("Invalid DATA integer")
    values = tuple(map(int, words[1:]))
    limits = [(0, 2**32-1)] * 2 + [(0, 65535)] + [(-32768, 32767)] * (11 if compact else 19) + [(-2**31, 2**31-1)] * 2
    if any(not low <= value <= high for value, (low, high) in zip(values, limits)):
        raise ValueError("DATA integer outside its type range")
    return SensorSample(values[0], values[1], values[2], values[3:11], () if compact else values[11:19],
                        values[11:14] if compact else values[19:22], values[14:16] if compact else values[22:24],
                        time.monotonic() if now is None else now, text)


class ClientState:
    """Bounded thread-safe mailbox: telemetry replaces a sample, never queues redraws."""
    def __init__(self, *, logs=False, verbose=False):
        self.lock = threading.Lock()
        self.logs, self.verbose = logs, verbose
        self.events = deque(maxlen=200)
        self.event_number = 0
        self.sample = None
        self.status = "Connected; waiting for SESSION READY"
        self.disconnect_reason = None
        self.heartbeat_acks = 0
        self.bad_data = 0

    def event(self, text):
        # Never interpret remote text as terminal escapes or formatted markup.
        text = "".join(c if c.isprintable() else "?" for c in text[:1024])
        with self.lock:
            self.event_number += 1
            self.events.append((self.event_number, text))

    def receive(self, text):
        if text.split(" ", 1)[0] in ("DATA", "DATA2"):
            try:
                sample = parse_data(text)
            except ValueError as error:
                with self.lock:
                    self.bad_data += 1
                self.event(f"Rejected sensor record: {error}")
            else:
                with self.lock:
                    self.sample = sample
            return
        with self.lock:
            if re.fullmatch(r"SESSION [0-9a-f]{16} READY", text):
                self.status = f"Ready: {text.split()[1]} (not a motor acknowledgement)"
            if text == "[MOTOR] OK HB":
                self.heartbeat_acks += 1  # Process even when display is suppressed.
        if display_record(text, logs=self.logs, verbose=self.verbose):
            self.event(text)

    def disconnected(self, reason):
        with self.lock:
            self.status = "Disconnected; explicit reconnect and ARM required"
            self.disconnect_reason = reason
            duplicate = bool(self.events and self.events[-1][1] == reason)
        if not duplicate:
            self.event(reason)

    def snapshot(self):
        with self.lock:
            return self.status, self.sample, tuple(self.events), self.bad_data


def sensor_text(sample, now=None):
    if sample is None:
        return "Waiting for a valid DATA record. Readings are raw."
    elapsed = max(0, int(((time.monotonic() if now is None else now) - sample.received_at) * 1000))
    return (f"Sequence {sample.sequence} | e-puck timestamp {sample.epuck_time_ms} ms\n"
            f"Reported age {sample.reported_age_ms} ms | Since reception {elapsed} ms\n"
            f"Proximity[0..7]       {' '.join(map(str, sample.proximity))}\n"
            f"Accelerometer[x y z]  {' '.join(map(str, sample.accelerometer))}\n"
            f"Commanded steps L/R   {' '.join(map(str, sample.commanded_steps))} (not measured motion)")


def validate(command):
    words = command.strip().split()
    if not words:
        return None
    if len(command) > 63:
        raise ValueError("Command too long (maximum 63 characters)")
    words[0] = words[0].upper()
    if words in [["HELP"], ["MOTOR_STATUS"], ["ARM"], ["STOP"], ["STREAM_STATUS"], ["LINK_STATUS"]]:
        return words[0]
    if len(words) == 2 and words[0] == "STREAM" and re.fullmatch(r"[+]?[0-9]+", words[1]):
        period = int(words[1])
        if period == 0 or 50 <= period <= 10000:
            return f"STREAM {period}"
    if len(words) == 3 and words[0] == "M":
        if all(re.fullmatch(r"[+-]?[0-9]+", word) for word in words[1:]):
            speeds = [int(word) for word in words[1:]]
            if all(-1000 <= speed <= 1000 for speed in speeds):
                return f"M {speeds[0]} {speeds[1]}"
    raise ValueError("HELP, MOTOR_STATUS, ARM, M <left> <right>, STOP, STREAM <0 or 50..10000 ms>, STREAM_STATUS, LINK_STATUS; speeds -1000..1000 commanded steps/s")


def run_network(sock, commands, stop, done, logs=False, *, handshake_timeout=1.0, ready_event=None,
                verbose=False, on_record=None, on_disconnect=None):
    """Sole socket writer/closer; stdin only submits bounded queue entries."""
    def emit(text):
        # The production sink only updates bounded memory, never renders or writes IO.
        if on_record is not None:
            on_record(text)
        elif display_record(text, logs=logs, verbose=verbose):
            print(text, flush=True)

    reason = "Stopped; connection closed."
    pending = b""
    received = bytearray()
    connected_at = time.monotonic()
    heartbeat = connected_at
    session_id = None
    ready_event = ready_event if ready_event is not None else threading.Event()
    ready_event.clear()
    closing_at = None
    stop_started = False
    write_at = heartbeat
    try:
        sock.setblocking(False)
        while True:
            now = time.monotonic()
            if session_id is None and now - connected_at >= handshake_timeout:
                raise TimeoutError(f"SESSION READY handshake timed out after {handshake_timeout:g}s; no commands sent")
            if stop.is_set() and closing_at is None:
                closing_at = now
                while True:
                    try:
                        commands.get_nowait()
                    except queue.Empty:
                        break
                if session_id is None:
                    break  # No commands, including STOP, before a valid READY.
            if closing_at is not None and now - closing_at >= 1.0:
                break
            if not pending and session_id is not None:
                if closing_at is not None:
                    if not stop_started:
                        pending = b"STOP\n"
                        stop_started = True
                elif now >= heartbeat:
                    pending = b"CONTROL_HB\n"
                    heartbeat = now + 0.1
                else:
                    try:
                        pending = (commands.get_nowait() + "\n").encode("ascii")
                    except queue.Empty:
                        pass
                write_at = now
            readable, writable, _ = select.select([sock], [sock] if pending else [], [], 0.02)
            if readable:
                chunk = sock.recv(1024)
                if not chunk:
                    reason = "Disconnected; explicit reconnect and ARM required."
                    emit(reason)
                    break
                received.extend(chunk)
                while b"\n" in received:
                    line, _, received = received.partition(b"\n")
                    if len(line) > 1024:
                        raise ValueError("Oversized server record")
                    text = line.rstrip(b"\r").decode("ascii", errors="replace")
                    if text.startswith("SESSION "):
                        match = re.fullmatch(r"SESSION ([0-9a-f]{16}) READY", text)
                        if not match:
                            raise ValueError(f"Invalid SESSION READY record: {text}")
                        if session_id is not None:
                            if session_id != match[1]:
                                raise ValueError("Session changed; explicit reconnect required")
                            continue  # Duplicate READY is not a new session.
                        session_id = match[1]
                        heartbeat = time.monotonic()  # First HB before queued user commands.
                        ready_event.set()
                    emit(text)
                    if text.startswith("DISCONNECT "):
                        reason = text
                        return  # Display the server reason and stop all further writes.
                    if closing_at is not None and text == "[MOTOR] OK STOP":
                        return
                if len(received) > 1024:
                    raise ValueError("Oversized server record")
            if writable and pending:
                try:
                    count = sock.send(pending)
                    if count == 0:
                        raise ConnectionError("Zero-length write")
                    pending = pending[count:]
                except BlockingIOError:
                    pass
            if pending and now - write_at >= 0.5:
                raise TimeoutError("Network write stalled; dropping connection")
    except (OSError, ValueError) as error:
        reason = f"Connection lost: {error}; no movement will be replayed."
        emit(reason)
    finally:
        ready_event.clear()
        sock.close()
        if on_disconnect is not None:
            on_disconnect(reason)
        done.set()


def submit_command(text, commands, ready, state):
    try:
        command = validate(text)
        if command is None:
            return
        if not ready.is_set():
            state.event("Rejected: no ready session; wait for SESSION READY and re-enter the command")
            return
        commands.put_nowait(command)
        state.event(f"> {command} (queued locally; not acknowledged)")
    except queue.Full:
        state.event("Rejected: command queue full")
    except ValueError as error:
        state.event(f"Rejected: {error}")


class PlainOutput:
    def __init__(self, stream):
        self.stream = stream
        self.event_number = 0
        self.last_sample = None
        self.sensor_at = 0.0

    def render(self, state, *, final=False):
        _, sample, events, _ = state.snapshot()
        if events and events[0][0] > self.event_number + 1:
            print("[display] Older events dropped from bounded history", file=self.stream)
        for number, text in events:
            if number > self.event_number:
                print(text, file=self.stream)
                self.event_number = number
        now = time.monotonic()
        if sample is not None and sample is not self.last_sample and (final or now >= self.sensor_at):
            # Plain output is line-oriented; coalesce fast telemetry to <=1 Hz.
            print(sample.raw, file=self.stream)
            self.last_sample, self.sensor_at = sample, now + 1.0
        self.stream.flush()


def run_plain(state, submit, done, stop, *, input_stream=None, output_stream=None):
    """Poll stdin so an open/idle pipe or terminal cannot conceal a disconnect."""
    input_stream = sys.stdin if input_stream is None else input_stream
    output = PlainOutput(sys.stdout if output_stream is None else output_stream)
    line = bytearray()
    discard = False
    try:
        while not done.is_set():
            output.render(state)
            if not select.select([input_stream], [], [], 0.05)[0]:
                continue
            chunk = os.read(input_stream.fileno(), 256)
            if not chunk:
                break
            for byte in chunk:
                if byte == 10:
                    if discard:
                        state.event("Rejected: oversized input line (maximum 128 bytes)")
                    else:
                        try:
                            submit(line.decode("ascii"))
                        except UnicodeDecodeError:
                            state.event("Rejected: command must be ASCII")
                    line.clear()
                    discard = False
                elif not discard:
                    if len(line) >= 128:
                        discard = True
                    else:
                        line.append(byte)
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        stop.set()
        done.wait(1.5)  # Network thread owns best-effort STOP and socket cleanup.
        output.render(state, final=True)


def use_terminal_ui(args):
    return (args.ui or (not args.plain and sys.stdin.isatty() and sys.stdout.isatty()
                       and os.environ.get("TERM", "dumb") not in ("dumb", "unknown", "")))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ip")
    parser.add_argument("--port", type=int, default=3333)
    parser.add_argument("--logs", action="store_true", help="Show optional LOG records")
    parser.add_argument("--verbose", action="store_true", help="Also display routine motor heartbeat acknowledgements")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--plain", action="store_true", help="Separate line-oriented mode; no UI dependency required")
    modes.add_argument("--ui", action="store_true", help="Require the persistent full-screen terminal interface")
    args = parser.parse_args()
    commands = queue.Queue(maxsize=32)
    stop, done, ready = threading.Event(), threading.Event(), threading.Event()
    state = ClientState(logs=args.logs, verbose=args.verbose)
    submit = lambda text: submit_command(text, commands, ready, state)
    terminal = None
    if use_terminal_ui(args):
        if not sys.stdin.isatty() or not sys.stdout.isatty():
            parser.error("--ui requires terminal stdin/stdout; use --plain for pipes")
        if os.environ.get("TERM", "dumb") in ("dumb", "unknown", ""):
            parser.error("--ui requires a supported TERM setting; use --plain on unsupported terminals")
        try:
            from wifi_client_ui import TerminalUI
            terminal = TerminalUI(state, submit, done, stop, sensor_text)
        except ImportError:
            print("Terminal UI requires prompt_toolkit. Install tools/requirements.txt or explicitly use --plain.", file=sys.stderr)
            return 2
    try:
        sock = socket.create_connection((args.ip, args.port), timeout=5)
    except (OSError, KeyboardInterrupt) as error:
        print(f"Connection failed: {error}; no commands sent.", file=sys.stderr)
        return 1
    worker = threading.Thread(target=run_network, args=(sock, commands, stop, done),
                              kwargs={"ready_event": ready, "on_record": state.receive,
                                      "on_disconnect": state.disconnected}, daemon=True)
    worker.start()
    state.event("Connected. Speeds are commanded steps/s, not measured wheel speed. HELP lists commands. Ctrl-C/EOF exits.")
    try:
        if terminal is None:
            run_plain(state, submit, done, stop)
        else:
            try:
                terminal.run()
            finally:
                stop.set()
                worker.join(timeout=1.5)
                # The alternate screen is gone; retain recent events/reason in scrollback.
                for _, text in state.snapshot()[2][-8:]:
                    print(text)
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        stop.set()
        worker.join(timeout=1.5)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
