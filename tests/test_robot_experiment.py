import asyncio
import io
import json
import queue
import socket
import threading
import unittest
import tempfile
from pathlib import Path
from tools.wifi_client import run_network
from tools.uwb_collect import collect
from tools.uwb_logging import CompactLog

DATA='DATA2 4294967295 0 65535 '+' '.join(['0']*11)+' -2147483648 2147483647'

class Robot(unittest.TestCase):
    def test_default_client_direct_commands_and_no_external_heartbeat(self):
        local,peer=socket.socketpair();peer.settimeout(.2)
        commands=queue.Queue(4);commands.put('M 10 -20')
        stop=threading.Event();done=threading.Event()
        thread=threading.Thread(target=run_network,args=(local,commands,stop,done),kwargs=dict(on_record=lambda _:None))
        thread.start()
        try:
            peer.sendall(b'SESSION 0123456789abcdef READY\n')
            self.assertEqual(peer.recv(200),b'M 10 -20\n')
            with self.assertRaises(socket.timeout):peer.recv(200)
            commands.put('M -30 40');self.assertEqual(peer.recv(200),b'M -30 40\n')
            stop.set();self.assertEqual(peer.recv(200),b'STOP\n')
            peer.sendall(b'[MOTOR] OK STOP\n');thread.join(2);self.assertTrue(done.is_set())
        finally:stop.set();peer.close();thread.join(2)

    def test_compact_raw_steps_motor_ack_and_session(self):
        output=io.StringIO();log=CompactLog(output,'test')
        def emit(line):log.accept(dict(kind='bridge',line=line,host_received_utc='2026-11-06T10:00:00Z',host_monotonic_ns=5))
        log.accept(dict(kind='connected',connections=1,host_received_utc='2026-11-06T10:00:00Z'))
        emit('SESSION 0123456789abcdef READY');emit(DATA)
        emit(DATA.replace('4294967295 0 65535','0 400 0'));emit('[MOTOR] OK STOP')
        rows=list(map(json.loads,output.getvalue().splitlines()));steps=[r for r in rows if r['type']=='steps']
        self.assertEqual(steps[0]['left'],-2147483648);self.assertIsNone(steps[0]['producer_counter_width'])
        self.assertEqual(steps[0]['transport_session'],'0123456789abcdef')
        self.assertFalse(steps[1]['discontinuity']);self.assertTrue(steps[1]['stale']);self.assertEqual(steps[1]['missing_samples'],0)
        self.assertTrue(any(r['type']=='motor_status' for r in rows))

    def test_offline_traffic_summary(self):
        from tools.robot_bandwidth import summarize
        with tempfile.TemporaryDirectory() as work:
            path=Path(work)/'fixture.jsonl'
            path.write_text('\n'.join(json.dumps(dict(kind='received',raw=DATA.replace('4294967295',str(i)),
                received_unix=100+i*.1,capture='fixture',session='session',generation=1)) for i in range(11)))
            result=summarize(path)
            self.assertAlmostEqual(result['steps_hz'],10.)
            self.assertEqual(result['counts']['missing_source_sequences'],0)
            self.assertGreater(result['application_bytes_per_second'],0)

class Collector(unittest.IsolatedAsyncioTestCase):
    async def test_default_collector_does_not_send_heartbeat_and_keeps_steps(self):
        stop=asyncio.Event();received=[]
        async def server(reader,writer):
            try:
                writer.write(('SESSION 0123456789abcdef READY\n'+DATA+'\n').encode());await writer.drain()
                try:received.append(await asyncio.wait_for(reader.read(100),.2))
                except asyncio.TimeoutError:pass
                stop.set()
            finally:writer.close();await writer.wait_closed()
        listener=await asyncio.start_server(server,'127.0.0.1',0);out=io.StringIO()
        try:await asyncio.wait_for(collect(f'127.0.0.1:{listener.sockets[0].getsockname()[1]}',out,stop),2)
        finally:listener.close();await listener.wait_closed()
        self.assertFalse(received)
        self.assertTrue(any(r['type']=='steps' for r in map(json.loads,out.getvalue().splitlines())))
