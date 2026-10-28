import binascii
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from uwb_collect import decode, Ordering
from uwb_logging import CompactLog
from uwb_replay import snapshot, digest, Reassembly
from validate_tdoa_replay import validate

ROOT = Path(__file__).resolve().parents[1]
STM = ROOT.parent/'epuck-stm32'


class Replay(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not (STM/'tools/build_replay_host.py').exists():
            raise unittest.SkipTest('integration requires matching sibling STM32 checkout')
        cls.work = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.work.cleanup)
        cls.path = Path(cls.work.name)
        exe = cls.path/'fixture'
        trace = cls.path/'trace.uart'
        subprocess.run(['python3', str(STM/'tools/build_replay_host.py'), '--fixture', '--sanitize', '--output', str(exe)], check=True)
        subprocess.run([str(exe)], env=dict(os.environ, MUL_REPLAY_TRACE=str(trace)), check=True, stdout=subprocess.DEVNULL)
        cls.lines = trace.read_text().splitlines()
        cls.fragments = [line for line in cls.lines if decode(line)['type']=='tdoa_replay_fragment']

    def project(self, lines):
        output, order = io.StringIO(), Ordering()
        log = CompactLog(output, 'host-test')
        for line in lines:
            r = decode(line)
            log.accept(dict(r, kind='telemetry', host_received_utc='2026-10-28T00:00:00Z', host_monotonic_ns=1, **order.accept(r), missing_total=0))
        log.finish_replay()
        return output.getvalue()

    def test_exact_baseline_and_vector(self):
        recording = self.path/'capture.jsonl'
        recording.write_text(self.project(self.lines))
        result = validate(recording, STM, sanitize=True)
        self.assertTrue(result['passed'], result['mismatches'])
        self.assertTrue(result['replay_complete'], result)
        self.assertEqual(result['exact_dtu_matches'], 45)
        self.assertEqual(result['exact_mm_matches'], 45)
        self.assertEqual(result['status_reason_matches'], 45)
        self.assertEqual(result['vector_comparisons'], 45)
        rows = [json.loads(line) for line in recording.read_text().splitlines() if '"type":"tdoa_replay"' in line]
        self.assertTrue(any(r['seen_mask']==1 for r in rows))
        self.assertTrue(any(r['seen_mask']==2 for r in rows))
        self.assertTrue(any(r['expected']['tdoa_dtu']<0 and r['expected']['status']==4 for r in rows))
        self.assertTrue(any(r['relation']['delay_dtu']>0 for r in rows))

    def test_disabled_instrumentation_preserves_outputs(self):
        exe=self.path/'disabled';trace=self.path/'disabled.uart'
        subprocess.run(['python3',str(STM/'tools/build_replay_host.py'),'--fixture','--disabled','--sanitize','--output',str(exe)],check=True)
        subprocess.run([str(exe)],env=dict(os.environ,MUL_REPLAY_TRACE=str(trace)),check=True,stdout=subprocess.DEVNULL)
        disabled=[r for line in trace.read_text().splitlines() if (r:=decode(line))['type']=='tdoa_vector']
        enabled=[decode(line) for line in self.lines if decode(line)['type']=='tdoa_vector']
        self.assertEqual(len(disabled),len(enabled))
        for a,b in zip(disabled,enabled):
            for key in ('cycle','followers','seen_mask','valid_mask','schedule_fingerprint'):
                self.assertEqual(a[key],b[key],key)

    def test_loss_is_diagnostic_not_missing_uwb(self):
        text = self.project(self.fragments[:1]+self.fragments[2:4])
        rows = list(map(json.loads, text.splitlines()))
        self.assertTrue(any(r['type']=='tdoa_replay_incomplete' for r in rows))
        self.assertFalse(any(r['type']=='tdoa_replay' for r in rows))
        self.assertTrue(any(r['type']=='gap' for r in rows))

    def test_unknown_schema_crc_and_truncation(self):
        b = bytearray.fromhex(self.fragments[0][5:]);b[11]=99
        b[-2:]=binascii.crc_hqx(b[:-2],0xffff).to_bytes(2,'little')
        with self.assertRaisesRegex(ValueError,'unsupported'):
            decode('UWB3 '+b.hex())
        with self.assertRaises(ValueError):decode(self.fragments[0][:-1])
        with self.assertRaises(ValueError):decode(self.fragments[0][:-1]+'?')

    def test_integrity_association_and_bounds(self):
        r = decode(self.fragments[0]);assembly=Reassembly()
        assembly.accept(r, 'test')
        changed=dict(r,digest='00000000')
        with self.assertRaisesRegex(ValueError,'association'):assembly.accept(changed,'test')
        assembly=Reassembly();losses=[]
        for i in range(10):losses += assembly.accept(dict(r,node=i,diagnostic_id=i),'test')
        self.assertEqual(len(assembly.pending),8)
        self.assertEqual(len(losses),2)
        self.assertEqual(len(assembly.finish()),8)
        assembly=Reassembly()
        for line in self.fragments[:3]:assembly.accept(decode(line),'test')
        final=decode(self.fragments[3]);final['payload']=f'{int(final["payload"][:2],16)^1:02X}'+final['payload'][2:]
        with self.assertRaisesRegex(ValueError,'integrity'):assembly.accept(final,'test')

    def test_initial_partial_is_reported_when_next_snapshot_arrives(self):
        assembly=Reassembly()
        for line in self.fragments[1:4]:
            self.assertEqual(assembly.accept(decode(line),'test'),[])
        rows=assembly.accept(decode(self.fragments[4]),'test')
        self.assertEqual(len(rows),1)
        self.assertEqual(rows[0]['received_parts'],[1,2,3])
        self.assertEqual(rows[0]['reason'],'new_snapshot')
        self.assertEqual(rows[0]['diagnostic_id'],0)
        self.assertEqual(len(assembly.finish()),1) # New partial still gets a real tail report.

    def test_domains_and_tampering(self):
        rows=list(map(json.loads,self.project(self.fragments[:4]).splitlines()))
        row=next(r for r in rows if r['type']=='tdoa_replay')
        b=bytearray.fromhex(row['blob']);b[88]=99
        with self.assertRaisesRegex(ValueError,'clock domain'):snapshot(b,141)
        row['expected']['tdoa_dtu']+=1
        path=self.path/'tampered.jsonl';path.write_text(json.dumps(row)+'\n')
        self.assertTrue(validate(path,STM)['invalid_records'])

class ReplayAccounting(unittest.TestCase):
    def test_boot_baselines_and_backlog_are_not_capture_losses(self):
        from uwb_replay_audit import audit
        rows=[(dict(s='one',cycle=c,diagnostic_id=c,diagnostic_dropped=223,cycle_start_ms=t,cycle_ms=1000),b'')
              for c,t in ((68,174994),(69,175994),(70,176994),(284,389991),(285,390991))]
        vectors={('one',c):dict(cycle=c,u_ms=388991+(c-283)*1000) for c in (283,284,285)}
        _, result=audit(rows,vectors,[dict(type='transport',bridge_dropped=1,corrupt=0,uart_errors=0)],[])
        self.assertEqual(result['diagnostic_dropped'],0)
        self.assertEqual(result['diagnostic_dropped_boot_last'],223)
        self.assertEqual(result['diagnostic_id_gaps'],0)
        self.assertEqual(result['diagnostic_id_gaps_including_backlog'],213)
        self.assertEqual(len(result['historical_replay_records']),3)
        self.assertEqual(result['current_missing_vector_records'],0)
        self.assertEqual(result['bridge_counter_samples']['observed_increments']['bridge_dropped'],0)

    def test_counter_wrap_reset_and_real_interior_loss(self):
        from uwb_replay_audit import increments,audit
        self.assertEqual(increments([dict(n=0xfffffffe),dict(n=1)],['n'])['observed_increments']['n'],3)
        self.assertEqual(increments([dict(n=100),dict(n=2)],['n'])['observed_increments']['n'],2)
        self.assertEqual(increments([dict(a=100,b=200),dict(a=1,b=2)],['a','b'])['resets'],1)
        rows=[(dict(s='one',cycle=c,diagnostic_id=i,diagnostic_dropped=d,cycle_start_ms=c*1000,cycle_ms=1000),b'') for c,i,d in ((1,1,10),(2,3,11))]
        _, result=audit(rows,{},[],[])
        self.assertEqual(result['diagnostic_dropped'],1)
        self.assertEqual(result['diagnostic_id_gaps'],1)
