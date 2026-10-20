"""Client model, plain-mode and terminal integration tests; no robot required."""
import importlib.util
import asyncio
import io
import os
from pathlib import Path
import queue
import socket
import subprocess
import sys
import threading
import time
import unittest
from unittest.mock import patch

from test_client import client

try:
    from prompt_toolkit.input import create_pipe_input
    from prompt_toolkit.output import DummyOutput
except ImportError:
    create_pipe_input = None

ROOT = Path(__file__).parents[1]
DATA = "DATA 42 17 65535 " + " ".join(map(str, range(-8, 11))) + " -2147483648 2147483647"


class ModelTests(unittest.TestCase):
    def test_case_normalization_blanks_and_rejection(self):
        for text, result in [(" arm ", "ARM"), ("stop", "STOP"), ("hElP", "HELP"),
                             ("motor_status", "MOTOR_STATUS"), ("m -200 +200", "M -200 200"),
                             (" \t", None)]:
            self.assertEqual(client.validate(text), result)
        for text in ["arm now", "m 1 2 3", "m 1001 0", "hb", "m 1e2 0", "M 1\x00 2", "help " * 30]:
            with self.assertRaises(ValueError):
                client.validate(text)
        state, commands, ready = client.ClientState(), queue.Queue(1), threading.Event()
        client.submit_command("arm", commands, ready, state)
        self.assertTrue(commands.empty())
        ready.set()
        client.submit_command("", commands, ready, state)
        self.assertTrue(commands.empty())
        client.submit_command("help", commands, ready, state)
        client.submit_command("arm", commands, ready, state)
        self.assertEqual(commands.get_nowait(), "HELP")
        self.assertIn("queue full", state.snapshot()[2][-1][1])
        self.assertTrue(any("not acknowledged" in t for _, t in state.snapshot()[2]))

    def test_data_types_latest_only_and_invalid_recovery(self):
        sample = client.parse_data(DATA, now=10)
        self.assertEqual(sample.proximity, tuple(range(-8, 0)))
        self.assertEqual(sample.ambient_light, tuple(range(0, 8)))
        self.assertEqual(sample.accelerometer, (8, 9, 10))
        self.assertEqual(sample.commanded_steps, (-2147483648, 2147483647))
        panel = client.sensor_text(sample, now=10.2)
        for label in ["Sequence 42", "Reported age 17 ms", "Since reception", "Proximity", "Accelerometer", "Commanded steps"]:
            self.assertIn(label, panel)
        state = client.ClientState()
        for i in range(1000):
            state.receive(DATA.replace("DATA 42", f"DATA {i}"))
        self.assertEqual(state.snapshot()[1].sequence, 999)
        self.assertEqual(state.snapshot()[2], ())
        invalid = [DATA + " 1", DATA.rsplit(" ", 1)[0], DATA.replace("65535", "65536"),
                   DATA.replace("-2147483648", "-2147483649"), DATA.replace("-8", "32768", 1),
                   DATA.replace("42", "-1", 1), DATA.replace("17", "nope", 1), "DATA"]
        for text in invalid:
            state.receive(text)
            self.assertEqual(state.snapshot()[1].sequence, 999)
        self.assertEqual(state.snapshot()[3], len(invalid))
        state.receive(DATA)
        self.assertEqual(state.snapshot()[1].sequence, 42)

    def test_compact_data_without_lights_and_legacy_compatibility(self):
        compact = "DATA2 42 17 65535 " + " ".join(map(str, range(-8, 3))) + " -2147483648 2147483647"
        sample = client.parse_data(compact, now=10)
        self.assertEqual(sample.ambient_light, ())
        self.assertEqual(sample.proximity, tuple(range(-8, 0)))
        self.assertEqual(sample.accelerometer, (0, 1, 2))
        self.assertEqual(sample.commanded_steps, (-2147483648, 2147483647))
        self.assertNotIn("light", client.sensor_text(sample).lower())
        state = client.ClientState()
        state.receive(compact)
        self.assertEqual(state.snapshot()[1].sequence, 42)
        self.assertFalse(state.snapshot()[2])
        for bad in [compact+" 0", compact.rsplit(" ",1)[0], compact.replace("65535","65536"), "DATA2"]:
            with self.assertRaises(ValueError): client.parse_data(bad)
        self.assertTrue(client.parse_data(DATA).ambient_light)  # older firmware remains readable

    def test_filtering_is_only_display_and_history_bounded(self):
        for logs, verbose in [(False, False), (True, False), (False, True), (True, True)]:
            state = client.ClientState(logs=logs, verbose=verbose)
            state.receive("[MOTOR] OK HB")
            state.receive("LOG debug")
            state.receive("[MOTOR] OK ARM")
            state.receive("[MOTOR] ERR NOT_ARMED")
            self.assertEqual(state.heartbeat_acks, 1)
            events = [t for _, t in state.snapshot()[2]]
            self.assertEqual("[MOTOR] OK HB" in events, verbose)
            self.assertEqual("LOG debug" in events, logs)
            self.assertIn("[MOTOR] OK ARM", events)
            self.assertIn("[MOTOR] ERR NOT_ARMED", events)
        for i in range(500):
            state.event(str(i))
        self.assertEqual(len(state.snapshot()[2]), 200)
        state.event("\x1b[31munsafe")
        self.assertNotIn("\x1b", state.snapshot()[2][-1][1])

    def test_plain_render_has_no_escape_codes_and_coalesces_sensors(self):
        state, stream = client.ClientState(), io.StringIO()
        output = client.PlainOutput(stream)
        state.receive(DATA)
        state.receive("[MOTOR] OK HB")
        state.receive("[MOTOR] OK ARM")
        output.render(state)
        state.receive(DATA.replace("DATA 42", "DATA 43"))
        output.render(state)
        self.assertEqual(stream.getvalue().count("DATA "), 1)
        output.render(state, final=True)
        self.assertEqual(stream.getvalue().count("DATA "), 2)
        self.assertIn("[MOTOR] OK ARM", stream.getvalue())
        self.assertNotIn("OK HB", stream.getvalue())
        self.assertNotIn("\x1b", stream.getvalue())


class NetworkUIBase(unittest.TestCase):
    def start_network(self, state, timeout=1):
        local, self.peer = socket.socketpair()
        self.peer.settimeout(.04)
        self.commands = queue.Queue(32)
        self.stop, self.done, self.ready = threading.Event(), threading.Event(), threading.Event()
        self.worker = threading.Thread(target=client.run_network,
            args=(local, self.commands, self.stop, self.done),
            kwargs={"on_record": state.receive, "on_disconnect": state.disconnected,
                    "ready_event": self.ready, "handshake_timeout": timeout})
        self.worker.start()
        self.addCleanup(self.cleanup_network)

    def cleanup_network(self):
        self.stop.set()
        self.peer.close()
        self.worker.join(2)
        self.assertFalse(self.worker.is_alive())


class PlainTests(NetworkUIBase):
    def test_disconnect_does_not_wait_for_stdin(self):
        state = client.ClientState()
        self.start_network(state)
        read_fd, write_fd = os.pipe()
        with os.fdopen(read_fd, "r") as stream:
            output = io.StringIO()
            thread = threading.Thread(target=client.run_plain,
                args=(state, lambda t: None, self.done, self.stop),
                kwargs={"input_stream": stream, "output_stream": output})
            thread.start()
            self.peer.sendall(b"DISCONNECT 0123456789abcdef reason=wifi_lost\n")
            thread.join(1)
            self.assertFalse(thread.is_alive())
            self.assertIn("reason=wifi_lost", output.getvalue())
        os.close(write_fd)

    def test_plain_cli_auto_fallback_and_eof_stop(self):
        for flags in [[], ["--plain"]]:
            with self.subTest(flags=flags):
                self.run_plain_cli(flags)

    def run_plain_cli(self, flags):
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0)); listener.listen(1); listener.settimeout(2)
        process = subprocess.Popen([sys.executable, str(ROOT / "tools/wifi_client.py"),
            "127.0.0.1", "--port", str(listener.getsockname()[1]), "--verbose", "--logs"] + flags,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            peer, _ = listener.accept()
            with peer:
                peer.settimeout(2)
                peer.sendall(b"SESSION 0123456789abcdef READY\n")
                self.assertIn(b"CONTROL_HB\n", peer.recv(100))
                peer.sendall((DATA + "\n[MOTOR] OK HB\nLOG debug\n").encode())
                process.stdin.close()  # Real EOF, no blocking input() thread.
                seen = b""
                while b"STOP\n" not in seen:
                    chunk = peer.recv(100)
                    self.assertTrue(chunk)
                    seen += chunk
                peer.sendall(b"[MOTOR] OK STOP\n")
            process.wait(2)
            output = process.stdout.read().decode()
            self.assertEqual(process.returncode, 0, process.stderr.read().decode())
            self.assertIn("DATA 42", output)
            self.assertIn("OK HB", output)
            self.assertIn("LOG debug", output)
            self.assertNotIn("\x1b", output)
        finally:
            if process.poll() is None: process.kill(); process.wait()
            for stream in (process.stdin, process.stdout, process.stderr): stream.close()
            listener.close()


@unittest.skipIf(create_pipe_input is None, "Install tools/requirements.txt for terminal UI tests")
class TerminalTests(NetworkUIBase):
    def make_ui(self, state, pipe, output=None):
        spec = importlib.util.spec_from_file_location("wifi_client_ui", ROOT / "tools/wifi_client_ui.py")
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        return module.TerminalUI(state, lambda t: client.submit_command(t, self.commands, self.ready, state),
            self.done, self.stop, client.sensor_text, input=pipe,
            output=DummyOutput() if output is None else output)

    def run_script(self, ui, script):
        async def checked():
            try:
                await asyncio.wait_for(script(), 8)
            except Exception as error:
                ui.app.exit(exception=error)
            else:
                ui.app.exit()
        ui.app.pre_run_callables.append(lambda: ui.app.create_background_task(checked()))
        # Neither the network sink nor the UI may print while Application runs.
        with patch("builtins.print", side_effect=AssertionError("out-of-band print in terminal UI")):
            ui.run()

    def test_slow_typing_selection_history_edit_keys_and_resize_under_flood(self):
        from prompt_toolkit.data_structures import Size

        class ResizableOutput(DummyOutput):
            rows, columns = 30, 100
            def get_size(self):
                return Size(rows=self.rows, columns=self.columns)

        state = client.ClientState(logs=True)
        self.start_network(state)
        output = ResizableOutput()
        seen, errors = bytearray(), []
        pumping = threading.Event()
        def flood():
            try:
                self.peer.sendall(b"SESSION 0123456789abcdef READY\n")
                self.peer.settimeout(.01)
                while not pumping.is_set() and not self.done.is_set():
                    self.peer.sendall((DATA + "\nLOG telemetry test\n[MOTOR] OK HB\n[MOTOR] OK M\n").encode())
                    try:
                        seen.extend(self.peer.recv(2048))
                    except socket.timeout:
                        pass
                    if b"STOP\n" in seen:
                        self.peer.sendall(b"[MOTOR] OK STOP\n")
                        break
            except OSError as error:
                if not self.done.is_set(): errors.append(error)
        thread = threading.Thread(target=flood)
        thread.start()
        try:
            with create_pipe_input() as pipe:
                ui = self.make_ui(state, pipe, output)
                buffer = ui.command.buffer
                async def script():
                    async def key(text):
                        pipe.send_text(text)
                        await asyncio.sleep(.1)
                    async def editor_unchanged():
                        before = (buffer.text, buffer.cursor_position, buffer.selection_state,
                                  buffer.document.selection_range(), ui.history.get_strings())
                        await asyncio.sleep(.21)
                        self.assertEqual((buffer.text, buffer.cursor_position, buffer.selection_state,
                                          buffer.document.selection_range(), ui.history.get_strings()), before)
                        self.assertIs(ui.app.layout.current_control, ui.command.control)

                    while not self.ready.is_set(): await asyncio.sleep(.01)
                    expected = ""
                    for char in "m 200 200":
                        expected += char
                        await key(char)
                        self.assertEqual(buffer.text, expected)
                        self.assertEqual(buffer.cursor_position, len(expected))
                    await key("\x1b[D\x1b[D")  # Left, Left: edit in the middle.
                    self.assertEqual(buffer.cursor_position, 7)
                    await editor_unchanged()
                    buffer.start_selection()
                    buffer.cursor_position = 5
                    selection = buffer.selection_state
                    await editor_unchanged()
                    self.assertIs(buffer.selection_state, selection)
                    # Resize rendered screens down to one row, then grow back.
                    for rows, columns in [(18, 60), (8, 32), (3, 20), (1, 20), (30, 100)]:
                        output.rows, output.columns = rows, columns
                        ui.app.invalidate()
                        await editor_unchanged()
                        position = ui.app.renderer._last_screen.visible_windows_to_write_positions[ui.command.window]
                        self.assertEqual((position.ypos, position.height), (rows-1, 1))
                    buffer.exit_selection()
                    await key("\x1b[F")  # End
                    self.assertEqual(buffer.cursor_position, 9)
                    await key("\x7f")  # Backspace
                    self.assertEqual(buffer.text, "m 200 20")
                    await key("0")
                    await key("\x1b[H")  # Home
                    self.assertEqual(buffer.cursor_position, 0)
                    await key("\x1b[3~")  # Delete
                    self.assertEqual(buffer.text, " 200 200")
                    await key("m")
                    await key("\x1b[C")
                    self.assertEqual(buffer.cursor_position, 2)
                    await key("\r")
                    self.assertIn(b"M 200 200\n", seen)
                    self.assertEqual(buffer.text, "")
                    await key("he")
                    await key("\x1b[A")  # Up recalls history; does not send.
                    self.assertEqual(buffer.text, "m 200 200")
                    await editor_unchanged()
                    self.assertEqual(seen.count(b"M 200 200\n"), 1)
                    await key("\x1b[B")  # Down restores the partial draft.
                    self.assertEqual(buffer.text, "he")
                    await key("lp\r")
                    self.assertIn(b"HELP\n", seen)
                    for text in ["help", "m 200 200"]:
                        await key("\x1b[A")
                        self.assertEqual(buffer.text, text)
                    self.assertGreaterEqual(seen.count(b"CONTROL_HB\n"), 10)
                    self.assertGreater(state.heartbeat_acks, 10)
                    self.assertLessEqual(len(state.snapshot()[2]), 200)
                    self.assertFalse(any(t == "[MOTOR] OK HB" for _, t in state.snapshot()[2]))
                    for i in range(150): ui.history.append_string(f"help {i}")
                    self.assertEqual(len(ui.history.get_strings()), 100)
                self.run_script(ui, script)
        finally:
            self.stop.set()
            self.done.wait(1.3)
            pumping.set()
            thread.join(2)
        if errors: raise errors[0]
        self.assertFalse(thread.is_alive())

    def test_disconnect_preserves_partial_edit_cursor_and_selection(self):
        state = client.ClientState(logs=True)
        self.start_network(state)
        self.peer.sendall(b"SESSION 0123456789abcdef READY\n")
        with create_pipe_input() as pipe:
            ui = self.make_ui(state, pipe)
            saved = []
            async def script():
                pipe.send_text("m 200")
                await asyncio.sleep(.15)
                buffer = ui.command.buffer
                buffer.cursor_position = 2
                buffer.start_selection()
                buffer.cursor_position = 4
                saved.append((buffer.text, buffer.cursor_position, buffer.selection_state))
                self.peer.sendall(b"LOG before loss\nDISCONNECT 0123456789abcdef reason=wifi_lost\n")
                # Let the UI's own watcher exit; this script should be cancelled.
                await asyncio.sleep(3)
                self.fail("UI did not exit promptly on disconnect")
            start = time.monotonic()
            self.run_script(ui, script)
            self.assertLess(time.monotonic()-start, 1)
            buffer = ui.command.buffer
            self.assertEqual((buffer.text, buffer.cursor_position, buffer.selection_state), saved[0])
            self.assertIn("reason=wifi_lost", state.disconnect_reason)
            self.assertEqual(self.commands.qsize(), 0)

    def test_editing_telemetry_heartbeats_and_ctrl_c(self):
        state = client.ClientState()
        self.start_network(state)
        errors, seen = [], bytearray()
        with create_pipe_input() as pipe:
            ui = self.make_ui(state, pipe)
            def driver():
                try:
                    self.peer.sendall(b"SESSION 0123456789abcdef READY\n")
                    self.assertTrue(self.ready.wait(.5))
                    pipe.send_text("motor_sta")
                    deadline = time.monotonic() + .45
                    while time.monotonic() < deadline:
                        self.peer.sendall((DATA + "\n[MOTOR] OK HB\n").encode())
                        try: seen.extend(self.peer.recv(1024))
                        except socket.timeout: pass
                    self.assertEqual(ui.command.text, "motor_sta")
                    self.assertGreaterEqual(seen.count(b"CONTROL_HB\n"), 3)
                    self.assertGreater(state.heartbeat_acks, 0)
                    self.assertFalse(any("DATA " in t or "OK HB" in t for _, t in state.snapshot()[2]))
                    pipe.send_text("tus\r")
                    deadline = time.monotonic() + .5
                    while b"MOTOR_STATUS\n" not in seen and time.monotonic() < deadline:
                        try: seen.extend(self.peer.recv(1024))
                        except socket.timeout: pass
                    self.assertIn(b"MOTOR_STATUS\n", seen)
                    pipe.send_text("\x03")
                    deadline = time.monotonic() + .5
                    while b"STOP\n" not in seen and time.monotonic() < deadline:
                        try: seen.extend(self.peer.recv(1024))
                        except socket.timeout: pass
                    self.assertIn(b"STOP\n", seen)
                    self.peer.sendall(b"[MOTOR] OK STOP\n")
                except BaseException as error:
                    errors.append(error)
                finally:
                    pipe.send_text("\x03")
            thread = threading.Thread(target=driver); thread.start()
            ui.run(); thread.join(2)
            self.assertFalse(thread.is_alive())
            if errors: raise errors[0]
            self.assertTrue(self.done.wait(1))

    def test_handshake_failure_and_disconnect_exit_without_enter(self):
        for timeout, notice in [(.08, None), (1, b"DISCONNECT 0123456789abcdef reason=uart_parity_error\n")]:
            with self.subTest(notice=notice), create_pipe_input() as pipe:
                state = client.ClientState()
                self.start_network(state, timeout)
                ui = self.make_ui(state, pipe)
                if notice: self.peer.sendall(notice)
                started = time.monotonic()
                ui.run()
                self.assertLess(time.monotonic()-started, 1)
                self.assertIn("Disconnected", state.snapshot()[0])
                self.assertIn("uart_parity_error" if notice else "handshake timed out", state.disconnect_reason)
                self.worker.join(1)
                self.peer.close()

    def test_ctrl_d_and_oversized_input(self):
        state = client.ClientState()
        self.start_network(state)
        with create_pipe_input() as pipe:
            ui = self.make_ui(state, pipe)
            ui.command.text = "arm" + " " * 200
            self.assertLessEqual(len(ui.command.text), 128)
            ui.accept(ui.command.buffer)
            self.assertTrue(self.commands.empty())
            pipe.send_text("\x04")
            ui.run()
            self.assertTrue(self.stop.is_set())

    def test_ui_exception_exits_without_out_of_band_print(self):
        state = client.ClientState()
        self.start_network(state)
        with create_pipe_input() as pipe:
            ui = self.make_ui(state, pipe)
            with patch.object(state, "snapshot", side_effect=RuntimeError("simulated UI failure")), \
                 patch("builtins.print", side_effect=AssertionError("printed during UI failure")):
                with self.assertRaisesRegex(RuntimeError, "simulated UI failure"):
                    ui.run()
            self.assertTrue(self.stop.is_set())
            self.assertFalse(ui.app.is_running)

    @unittest.skipUnless(os.name == "posix", "Pseudo-terminal test requires POSIX")
    def test_real_terminal_ctrl_c_restores_terminal_and_sends_stop(self):
        import fcntl
        import pty
        import select
        import struct
        import signal
        import termios
        master, slave = pty.openpty()
        before = termios.tcgetattr(slave)
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 18, 60, 0, 0))
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0)); listener.listen(1); listener.settimeout(3)
        process = subprocess.Popen([sys.executable, str(ROOT / "tools/wifi_client.py"),
            "127.0.0.1", "--port", str(listener.getsockname()[1]), "--ui", "--logs"],
            stdin=slave, stdout=slave, stderr=slave,
            env={**os.environ, "TERM": "xterm-256color", "PROMPT_TOOLKIT_NO_CPR": "1"})
        try:
            peer, _ = listener.accept()
            with peer:
                peer.settimeout(2)
                peer.sendall(b"SESSION 0123456789abcdef READY\n")
                deadline = time.monotonic() + 2
                screen = bytearray()
                while time.monotonic() < deadline:
                    if select.select([master], [], [], .05)[0]:
                        screen.extend(os.read(master, 65536))
                    if b"Command > " in screen:
                        break
                self.assertIn(b"Latest sensors", screen)
                self.assertFalse(termios.tcgetattr(slave)[3] & termios.ICANON)
                # A real SIGWINCH must not switch modes or lose a partial edit.
                os.write(master, b"he")
                peer.sendall((DATA + "\nLOG resize test\n[MOTOR] OK HB\n").encode())
                fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 12, 40, 0, 0))
                os.kill(process.pid, signal.SIGWINCH)
                resized_at = time.monotonic()
                while time.monotonic()-resized_at < .25:
                    if select.select([master], [], [], .05)[0]:
                        os.read(master, 65536)
                os.write(master, b"lp\r")
                seen = bytearray()
                while b"HELP\n" not in seen:
                    chunk = peer.recv(1024)
                    self.assertTrue(chunk)
                    seen.extend(chunk)
                os.write(master, b"\x03")
                while b"STOP\n" not in seen:
                    chunk = peer.recv(1024)
                    self.assertTrue(chunk)
                    seen.extend(chunk)
                peer.sendall(b"[MOTOR] OK STOP\n")
                process.wait(3)
            self.assertEqual(process.returncode, 0)
            self.assertEqual(termios.tcgetattr(slave), before)
        finally:
            if process.poll() is None: process.kill(); process.wait()
            listener.close()
            os.close(master); os.close(slave)


if __name__ == "__main__":
    unittest.main()
