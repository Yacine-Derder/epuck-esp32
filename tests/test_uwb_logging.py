import asyncio
import binascii
import io
import json
import os
import sys
from pathlib import Path
import struct
import subprocess
import tempfile
import threading
import unittest
from unittest.mock import patch
from test_uwb_collect import collector
from tools.uwb_logging import CompactLog, LiveMonitor, DEFAULT_LOG_DIR, migrate_logs, digest

ROOT = Path(__file__).parents[1]


class WireFixture:
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory()
        cls.producer = Path(cls.work.name) / 'produce'
        source = Path(cls.work.name) / 'produce.c'
        source.write_text('''
#include "mul_telemetry.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv) {
 unsigned status=argc>1?(unsigned)atoi(argv[1]):4;
 mulTelemetry_t r={.type=mulTlmTdoa,.node=141,.peer=133,.radio=2,.mode=status==4||status==2?5:4,.role=2,.epoch=0x1234,.exchange=0xabc,
 .sequence=argc>2?(unsigned)atoi(argv[2]):7,.uptimeMs=0xffffffff,.legacySlotMs=500,.ratePolicy=status==4||status==2,
 .referenceGeneration=3,.followerGeneration=4,.captureAgeMs=1,
 .tdoa={.reference=132,.follower=133,.referenceSequence=3666,.followerSequence=3676,.rx=0xffffffffffULL,
 .status=status,.clockValid=status!=0,.rateState=status==4?1:status==2?2:0,.ratePpb=status==4?-1234:INT32_MIN,
 .tdoaDtu=status==4?-28:INT32_MIN,.rangeDifferenceMm=status==4?-131:INT32_MIN,
 .delayAgeMs=4879,.delayDtu=120,.clockAgeMs=502,.delayState=2,.unknownComponents=63}};
 char line[MUL_TLM_LINE];if(!mulTelemetryEncode(line,sizeof(line),&r))return 1;puts(line);return 0;
}''')
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT/'ThirdParty/mul_core'), str(source), str(ROOT/'ThirdParty/mul_core/mul_telemetry.c'), '-o', str(cls.producer)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    @classmethod
    def wire(cls, status=4, sequence=7):
        return subprocess.check_output([str(cls.producer), str(status), str(sequence)], text=True).strip()

    @classmethod
    def row(cls, status=4, sequence=7):
        return dict(collector.decode(cls.wire(status, sequence)), kind='telemetry', host_received_utc='2026-10-23T14:25:12.123456+00:00', host_monotonic_ns=1000000000, sequence_status='first', missing=0, missing_total=0)

class TdoaWire(WireFixture, unittest.TestCase):
    def test_c_wire_values_and_every_invalid_status(self):
        for status in range(5):
            wire = self.wire(status)
            self.assertEqual(len(wire), 309)
            r = collector.decode(wire)
            self.assertEqual((r['reference_sequence'], r['follower_sequence']), (3666, 3676))
            self.assertEqual((r['node'], r['epoch'], r['uptime_ms'], r['rx']), (141, '0000000000001234', 0xffffffff, 'FFFFFFFFFF'))
            self.assertEqual(r['status'], status)
            self.assertEqual(r['valid'], status == 4)
            self.assertEqual(r['range_diff_mm'], -131 if status == 4 else None)
            self.assertEqual(r['tdoa_dtu'], -28 if status == 4 else None)
            self.assertNotIn('legacy_frame', r)
            self.assertNotIn('raw_mm', r)  # Never call TDoA an ADS range.
            self.assertNotIn('-2147483648', json.dumps(r))

    def test_validation_crc_length_reserved_and_inconsistent_validity(self):
        wire = self.wire()
        for length in range(len(wire)):
            with self.assertRaises(ValueError):
                collector.decode(wire[:length])
        for offset, value in ((71,1),(63,0),(65,0),(64,0),(7,4),(60,133),(130,1),(3,8)):
            data=bytearray.fromhex(wire[5:]);data[offset]=value
            struct.pack_into('<H', data,150,binascii.crc_hqx(data[:150],0xffff))
            with self.assertRaises(ValueError):collector.decode('UWB3 '+data.hex())
        data=bytearray.fromhex(wire[5:]);struct.pack_into('<i',data,52,-2147483648)
        struct.pack_into('<H',data,150,binascii.crc_hqx(data[:150],0xffff))
        with self.assertRaises(ValueError):collector.decode('UWB3 '+data.hex())

    def test_compact_schema_metadata_dedup_and_invalid_nulls(self):
        output=io.StringIO();log=CompactLog(output,'192.168.1.141:3333')
        a=self.row();log.accept(a);log.accept(dict(a, sequence=8))
        b=self.row(1);log.accept(b)
        records=[json.loads(line) for line in output.getvalue().splitlines()]
        measurements=[r for r in records if r['type']=='tdoa']
        self.assertEqual(len(measurements),3)
        self.assertEqual(measurements[0]['dd_mm'],-131)
        self.assertNotIn('status',measurements[0]);self.assertNotIn('valid',measurements[0])
        self.assertEqual(measurements[-1]['status'],1);self.assertIsNone(measurements[-1]['dd_mm']);self.assertIsNone(measurements[-1]['dt_dtu'])
        self.assertEqual(sum(r['type']=='capture' for r in records),1)
        self.assertEqual(sum(r['type']=='session' for r in records),2) # Firmware mode changes.
        self.assertEqual(sum(r['type']=='roster' for r in records),2)
        self.assertEqual(measurements[0]['t'],'2026-10-23T14:25:12.123Z')
        self.assertNotIn('legacy_frame',output.getvalue());self.assertNotIn('stamps',output.getvalue())

    def test_compact_roster_exposes_runtime_slot_changes(self):
        output=io.StringIO();log=CompactLog(output,'x')
        for slot in (10,50,500):
            log.accept(dict(self.row(),slot_ms=slot))
        rows=[json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual([r['slot_ms'] for r in rows if r['type']=='roster'],[10,50,500])

    def test_unchanged_bridge_diagnostics_are_suppressed_and_health_throttled(self):
        output=io.StringIO();log=CompactLog(output,'x',30)
        def bridge(line):log.accept(dict(kind='bridge',line=line,host_received_utc='2026-10-23T00:00:00Z'))
        for _ in range(5):bridge('UWBSTAT version=1 dropped=0 corrupt=0 uart_errors=0 depth=0 high=1')
        for _ in range(5):bridge('UWBSTAT version=1 dropped=3 corrupt=0 uart_errors=0 depth=0 high=1')
        bridge('UWBSTAT version=1 dropped=0 corrupt=0 uart_errors=0 depth=0 high=1')
        r=self.row();r.update(type='summary',attempts=123,completed=100)
        for n in range(40):log.accept(dict(r,host_monotonic_ns=n*1000000000))
        rows=[json.loads(s) for s in output.getvalue().splitlines()]
        self.assertEqual(sum(r['type']=='transport' for r in rows),2)
        self.assertTrue(next(r for r in rows if r.get('reset'))['reset'])
        self.assertEqual(sum(r['type']=='health' for r in rows),2)
        log.accept(dict(self.row(), type='hybrid_rx'))
        self.assertNotIn('"type":"hybrid_rx"',output.getvalue())

    def test_live_statistics_invalid_and_duplicate_do_not_pollute_mean(self):
        monitor=LiveMonitor(False);monitor.enabled=True
        monitor.accept('x',dict(kind='connected'))
        monitor.accept('x',self.row())
        monitor.accept('x',dict(self.row(),range_diff_mm=-231))
        monitor.accept('x',self.row(1))
        monitor.accept('x',dict(self.row(),sequence_status='duplicate'))
        s=monitor.snapshot()['x']
        self.assertEqual((s['valid'],s['invalid']),(2,1));self.assertAlmostEqual(s['mean'],-.181)
        self.assertAlmostEqual(s['std'],.1/(2**.5));self.assertEqual(s['connection'],'connected')

    def test_slow_console_does_not_hold_acquisition_lock(self):
        entered=threading.Event();release=threading.Event()
        def slow(*args):entered.set();release.wait(1);return 0
        monitor=LiveMonitor(False,fd=2,interval=.01);monitor.enabled=True
        monitor.accept('x',self.row())
        with patch('tools.uwb_logging.os.write',slow):
            monitor.thread=threading.Thread(target=monitor._render,daemon=True);monitor.thread.start()
            self.assertTrue(entered.wait(1))
            monitor.accept('x',self.row(1)) # Renderer is blocked, measurement update completes.
            self.assertEqual(monitor.snapshot()['x']['invalid'],1)
            release.set();monitor.close()


class CompactLoopback(WireFixture, unittest.IsolatedAsyncioTestCase):
    async def test_real_collector_compact_reconnect_and_readonly_heartbeat(self):
        stop=asyncio.Event();heartbeats=[];connections=0
        async def server(reader,writer):
            nonlocal connections
            connections+=1
            payload=('SESSION 0123456789abcdef READY\n'+self.wire(sequence=connections)+'\n').encode()
            writer.write(payload[:50]);await writer.drain();await asyncio.sleep(.01);writer.write(payload[50:]);await writer.drain()
            heartbeats.append(await asyncio.wait_for(reader.readline(),1))
            writer.close();await writer.wait_closed()
            if connections==2:stop.set()
        listener=await asyncio.start_server(server,'127.0.0.1',0);output=io.StringIO()
        try:await asyncio.wait_for(collector.collect('127.0.0.1:'+str(listener.sockets[0].getsockname()[1]),output,stop),5)
        finally:listener.close();await listener.wait_closed()
        rows=[json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual(sum(r['type']=='tdoa' for r in rows),2)
        self.assertEqual(sum(r['type']=='session' for r in rows),1)
        self.assertEqual(heartbeats,[b'CONTROL_HB\n',b'CONTROL_HB\n'])
        self.assertTrue(all('host_monotonic_ns' not in r for r in rows))


class Migration(unittest.TestCase):
    def test_directory_creation_copy_verification_and_collision_without_deletion(self):
        with tempfile.TemporaryDirectory() as work:
            src=Path(work)/'old';src.mkdir();dst=Path(work)/'Documents'/'uwb_logs'
            original=src/'run.jsonl';original.write_text(json.dumps(dict(kind='telemetry',type='hybrid_rx',node=141,radio=2,epoch='42'))+'\n')
            (src/'alr.log').write_bytes(b'MUL ALR: TX seq=1\r\n')
            (src/'unrelated.log').write_text('database log')
            (src/'link.log').symlink_to(src/'alr.log')
            result=migrate_logs(src,dst);self.assertEqual(len(result['copied']),2)
            self.assertEqual(original.read_bytes(),(dst/'run.jsonl').read_bytes())
            self.assertEqual(len(migrate_logs(src,dst)['identical']),2)
            original.write_text(original.read_text()+json.dumps(dict(kind='telemetry',type='summary',node=141,radio=2,epoch='43'))+'\n')
            old=dst/'run.jsonl';before=digest(old)
            result=migrate_logs(src,dst);self.assertEqual(len(result['copied']),1)
            self.assertEqual(before,digest(old));self.assertTrue(original.exists())
            self.assertEqual(digest(original),digest(Path(result['copied'][0])))
            self.assertFalse((dst/'unrelated.log').exists());self.assertFalse((dst/'link.log').exists())
            self.assertEqual(len(migrate_logs(src,dst)['identical']),2)
            with self.assertRaises(ValueError):migrate_logs(src,src)

    def test_missing_old_directory_still_creates_new_location(self):
        with tempfile.TemporaryDirectory() as work:
            path=Path(work)/'Documents'/'uwb_logs';result=migrate_logs(Path(work)/'missing',path)
            self.assertTrue(path.is_dir());self.assertEqual(result['copied'],[])
            cli=subprocess.run([sys.executable,str(ROOT/'tools/uwb_collect.py'),'--migrate-logs'],env=dict(os.environ,HOME=work),capture_output=True,text=True,check=True)
            self.assertEqual(json.loads(cli.stdout)['destination'],str(path))
        self.assertEqual(DEFAULT_LOG_DIR,Path.home()/'Documents'/'uwb_logs')

    def test_destination_symlink_collision_is_preserved(self):
        with tempfile.TemporaryDirectory() as work:
            src=Path(work)/'old';src.mkdir();dst=Path(work)/'new';dst.mkdir()
            (src/'alr.log').write_text('MUL ALR: packet\n')
            (dst/'alr.log').symlink_to(dst/'missing')
            result=migrate_logs(src,dst)
            self.assertTrue((dst/'alr.log').is_symlink())
            self.assertEqual(len(result['copied']),1)
            self.assertIn('.collision-',result['copied'][0])
            self.assertEqual(digest(src/'alr.log'),digest(Path(result['copied'][0])))
