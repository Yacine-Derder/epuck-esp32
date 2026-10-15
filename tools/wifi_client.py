#!/usr/bin/env python3
"""Interactive e-puck client. Never automatically ARM/reconnect/replay movement."""
import argparse
import queue
import re
import select
import socket
import threading
import time


def validate(command):
    words = command.strip().split()
    if words in [["HELP"], ["MOTOR_STATUS"], ["ARM"], ["STOP"]]:
        return words[0]
    if len(words) == 3 and words[0] == "M":
        if all(re.fullmatch(r"[+-]?[0-9]+", word) for word in words[1:]):
            speeds = [int(word) for word in words[1:]]
            if all(-1000 <= speed <= 1000 for speed in speeds):
                return f"M {speeds[0]} {speeds[1]}"
    raise ValueError("HELP, MOTOR_STATUS, ARM, M <left> <right>, STOP; speeds -1000..1000 commanded steps/s")


def run_network(sock, commands, stop, done, logs=False, *, handshake_timeout=1.0, ready_event=None):
    """Sole socket writer/closer; stdin only submits bounded queue entries."""
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
    sock.setblocking(False)
    try:
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
                    print("Disconnected; explicit reconnect and ARM required.")
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
                    if logs or not text.startswith("LOG "):
                        print(text, flush=True)
                    if text.startswith("DISCONNECT "):
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
        print(f"Connection lost: {error}; no movement will be replayed.", flush=True)
    finally:
        ready_event.clear()
        sock.close()
        done.set()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ip")
    parser.add_argument("--port", type=int, default=3333)
    parser.add_argument("--logs", action="store_true")
    args = parser.parse_args()
    commands = queue.Queue(maxsize=32)
    stop, done, ready_event = threading.Event(), threading.Event(), threading.Event()
    sock = socket.create_connection((args.ip, args.port), timeout=5)
    worker = threading.Thread(target=run_network, args=(sock, commands, stop, done, args.logs), kwargs={"ready_event": ready_event}, daemon=True)
    worker.start()
    print("Connected. Commanded steps/s, not measured wheel speed. Wait for SESSION READY; HELP lists commands. Ctrl-C/EOF exits.")
    try:
        while not done.is_set():
            text = input("> ")
            try:
                command = validate(text)
                if not ready_event.is_set():
                    print("Rejected: no ready session; wait for SESSION READY and re-enter the command")
                    continue
                commands.put_nowait(command)
            except queue.Full:
                print("Rejected: command queue full")
            except ValueError as error:
                print(f"Rejected: {error}")
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        stop.set()
        worker.join(timeout=1.5)


if __name__ == "__main__":
    main()
