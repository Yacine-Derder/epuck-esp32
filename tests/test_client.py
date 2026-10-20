import importlib.util
import pathlib
import queue
import socket
import threading
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("client", pathlib.Path(__file__).parents[1] / "tools/wifi_client.py")
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)

class ClientTests(unittest.TestCase):
    def test_stream_validation_and_20hz_network_updates(self):
        for text, expected in [("stream 100", "STREAM 100"), ("StReAm +50", "STREAM 50"),
                               ("stream 0", "STREAM 0"), ("STREAM 10000", "STREAM 10000"),
                               ("stream_status", "STREAM_STATUS")]:
            self.assertEqual(client.validate(text), expected)
        for text in ["STREAM", "STREAM -0", "STREAM -1", "STREAM 49", "STREAM 10001",
                     "STREAM 100 extra", "STREAM_STATUS 1", "STREAM 5e1", "STREAM 50.0"]:
            with self.assertRaises(ValueError): client.validate(text)
        local, peer = socket.socketpair()
        peer.settimeout(.01)
        stop, done, ready = threading.Event(), threading.Event(), threading.Event()
        commands, state = queue.Queue(32), client.ClientState()
        worker = threading.Thread(target=client.run_network, args=(local, commands, stop, done),
            kwargs={"ready_event": ready, "on_record": state.receive, "on_disconnect": state.disconnected})
        worker.start()
        seen = bytearray()
        try:
            peer.sendall(b"SESSION 0123456789abcdef READY\n")
            self.assertTrue(ready.wait(.5))
            for command in ["stream 50", "stream_status"]:
                client.submit_command(command, commands, ready, state)
            for i in range(12):
                record = f"DATA {i} 0 1 " + " ".join(["0"]*21)
                peer.sendall((record+"\n[MOTOR] OK HB\n").encode())
                until=time.monotonic()+.05
                while time.monotonic()<until:
                    try: seen.extend(peer.recv(1024))
                    except socket.timeout: pass
            peer.sendall(b"[STREAM] OK STREAM acknowledged=50 ms desired=50 ms\n")
            stop.set()
            deadline=time.monotonic()+1
            while b"STOP\n" not in seen and time.monotonic()<deadline:
                try: seen.extend(peer.recv(1024))
                except socket.timeout: pass
            self.assertIn(b"STOP\n", seen)
            peer.sendall(b"[MOTOR] OK STOP\n"); worker.join(1.5)
            self.assertTrue(done.is_set())
            self.assertIn(b"STREAM 50\n", seen); self.assertIn(b"STREAM_STATUS\n", seen)
            self.assertGreaterEqual(seen.count(b"CONTROL_HB\n"), 5)
            self.assertEqual(state.snapshot()[1].sequence, 11)
            events=[text for _, text in state.snapshot()[2]]
            self.assertFalse(any(text.startswith("DATA ") or text=="[MOTOR] OK HB" for text in events))
            self.assertTrue(any("acknowledged=50" in text for text in events))
        finally:
            stop.set(); peer.close(); worker.join(2)

    def test_validation(self):
        self.assertEqual(client.validate("M -1000 +1000"), "M -1000 1000")
        for command in ["M 1001 0", "M 0", "ARM 1", "HB", "M 1.0 0", "M 1 2 3"]:
            with self.assertRaises(ValueError): client.validate(command)

    def test_heartbeats_and_exit_without_auto_arm(self):
        local, peer = socket.socketpair()
        peer.settimeout(1)
        commands = queue.Queue(32)
        stop, done = threading.Event(), threading.Event()
        worker = threading.Thread(target=client.run_network, args=(local, commands, stop, done))
        worker.start()
        peer.sendall(b"SESSION 0123456789abcdef RE")
        peer.sendall(b"ADY\nDATA 1 0 0\n")
        time.sleep(.35)  # Typing is absent/blocked; heartbeats still arrive.
        stop.set()
        seen = b""
        while b"STOP\n" not in seen:
            seen += peer.recv(1)  # Fragment every command.
        peer.sendall(b"[MOTOR] OK STOP\n")
        worker.join(2)
        self.assertTrue(done.is_set())
        self.assertGreaterEqual(seen.count(b"CONTROL_HB\n"), 3)
        self.assertNotIn(b"ARM\n", seen)
        self.assertNotIn(b"M ", seen)
        peer.close()

    def test_handshake_gates_commands_and_heartbeats_with_partial_writes(self):
        class PartialSocket:
            def __init__(self, sock): self.sock = sock
            def fileno(self): return self.sock.fileno()
            def setblocking(self, flag): self.sock.setblocking(flag)
            def recv(self, size): return self.sock.recv(size)
            def send(self, data): return self.sock.send(data[:2])
            def close(self): self.sock.close()
        local, peer = socket.socketpair()
        peer.settimeout(.04)
        commands = queue.Queue(32)
        commands.put("HELP")
        stop, done, ready = threading.Event(), threading.Event(), threading.Event()
        with patch("builtins.print"):
            worker = threading.Thread(target=client.run_network,
                args=(PartialSocket(local), commands, stop, done),
                kwargs={"ready_event": ready, "handshake_timeout": .8})
            worker.start()
            peer.sendall(b"LOG SESSION 0123456789abcdef READY\nSESSION 0123456789abcdef RE")
            with self.assertRaises(socket.timeout): peer.recv(100)
            self.assertFalse(ready.is_set())
            peer.sendall(b"ADY\n")
            self.assertTrue(ready.wait(.4))
            peer.settimeout(.5)
            seen = b""
            while b"HELP\n" not in seen: seen += peer.recv(100)
            self.assertTrue(seen.startswith(b"CONTROL_HB\n"))
            stop.set()
            while b"STOP\n" not in seen: seen += peer.recv(100)
            peer.sendall(b"[MOTOR] OK STOP\n")
            worker.join(1)
            self.assertTrue(done.is_set())
        peer.close()

    def test_handshake_timeout_sends_nothing(self):
        local, peer = socket.socketpair()
        commands = queue.Queue(32)
        commands.put("HELP")
        stop, done = threading.Event(), threading.Event()
        with patch("builtins.print") as printed:
            worker = threading.Thread(target=client.run_network, args=(local, commands, stop, done),
                                      kwargs={"handshake_timeout": .08})
            worker.start(); worker.join(1)
            self.assertTrue(done.is_set())
            self.assertEqual(peer.recv(100), b"")
            self.assertTrue(any("handshake timed out" in str(call) for call in printed.call_args_list))
        peer.close()

    def test_invalid_ready_does_not_authorize_any_output(self):
        local, peer = socket.socketpair()
        stop, done = threading.Event(), threading.Event()
        with patch("builtins.print") as printed:
            worker = threading.Thread(target=client.run_network, args=(local, queue.Queue(32), stop, done))
            worker.start(); peer.sendall(b"SESSION invalid READY\n"); worker.join(1)
            self.assertTrue(done.is_set())
            self.assertEqual(peer.recv(100), b"")
            self.assertTrue(any("Invalid SESSION READY" in str(call) for call in printed.call_args_list))
        peer.close()

    def test_server_reason_displayed_without_log_flag(self):
        local, peer = socket.socketpair()
        stop, done = threading.Event(), threading.Event()
        with patch("builtins.print") as printed:
            worker = threading.Thread(target=client.run_network, args=(local, queue.Queue(32), stop, done))
            worker.start()
            peer.sendall(b"DISCONNECT 0123456789abcdef reason=uart_fifo_overflow detail=1\n")
            worker.join(1)
            self.assertTrue(done.is_set())
            self.assertTrue(any("reason=uart_fifo_overflow" in str(call) for call in printed.call_args_list))
            self.assertEqual(peer.recv(100), b"")
        peer.close()

if __name__ == "__main__": unittest.main()
