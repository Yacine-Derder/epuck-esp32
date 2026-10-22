import importlib.util
from pathlib import Path
import subprocess
import tempfile
import json
import sys
import unittest
ROOT=Path(__file__).parents[1]
def module(name):
 s=importlib.util.spec_from_file_location(name,ROOT/'tools'/f'{name}.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
class Hybrid(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.work=tempfile.TemporaryDirectory();core=ROOT/'ThirdParty/mul_core';cls.producer=Path(cls.work.name)/'producer'
  subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-DMUL_PROP_MAX_AGE_MS=6000','-I'+str(core),str(ROOT/'tests/fixtures/hybrid_capture.c'),str(core/'mul_common.c'),str(core/'mul_telemetry.c'),str(core/'mul_relation.c'),str(core/'mul_clock.c'),'-o',str(cls.producer)],check=True)
  cls.collect=module('uwb_collect');cls.host=module('uwb_hybrid');cls.loc=module('uwb_localize')
 @classmethod
 def tearDownClass(cls):cls.work.cleanup()
 def records(self,stationary):
  lines=subprocess.check_output([str(self.producer),str(int(stationary))],text=True).splitlines()
  return [dict(self.collect.decode(line),kind='telemetry') for line in lines]
 def test_actual_wire_collector_shared_replay_and_insufficient_position(self):
  replay=self.host.Replay()
  try:
   rows=[replay.feed(r) for r in self.records(True)]
   observations=[r for r in rows if r.get('status')=='AVAILABLE'];self.assertGreater(len(observations),3)
   for r in observations:
    self.assertLessEqual(abs(r['tdoa_dtu']-477),2);self.assertTrue(r['stationarity_assumed']);self.assertFalse(r['remote_boot_epoch_known'])
    result=self.loc.solve_2d({'0x84':[0,0],'0x85':[5,0]},[r['measurement']]);self.assertEqual(result['status'],'insufficient_geometry')
  finally:replay.close()
 def test_unknown_rate_loss_epochs_and_invalid_domain(self):
  replay=self.host.Replay()
  try:
   records=self.records(False);rows=[replay.feed(r) for r in records]
   self.assertTrue(any(r.get('status')=='NO_RATE' for r in rows));self.assertFalse(any(r.get('measurement') for r in rows))
   changed=dict(records[10],epoch='0000000000009999');self.assertFalse(replay.feed(changed)['associated'])
   changed=dict(records[-2],reference_generation=999);self.assertTrue(replay.feed(changed)['replay_reset_on_captured_generation'])
   bad=dict(records[0],flags=0);self.assertEqual(replay.feed(bad)['kind'],'invalid')
   bad=dict(records[0],slot_ms=50);self.assertEqual(replay.feed(bad)['kind'],'invalid')
   bad=dict(records[0],transaction=999);self.assertEqual(replay.feed(bad)['kind'],'invalid')
  finally:replay.close()
 def test_executable_jsonl_pipeline(self):
  with tempfile.TemporaryDirectory() as directory:
   d=Path(directory);capture=d/'capture.jsonl';anchors=d/'anchors.json';output=d/'observations.jsonl';positions=d/'positions.jsonl'
   capture.write_text(''.join(json.dumps(r)+'\n' for r in self.records(True)))
   anchors.write_text(json.dumps({'anchors':{'0x84':[0,0],'0x85':[5,0]}}))
   subprocess.run([sys.executable,str(ROOT/'tools/uwb_hybrid.py'),str(capture),'--output',str(output),'--anchors',str(anchors),'--positions',str(positions)],check=True)
   values=[json.loads(line) for line in positions.read_text().splitlines()]
   self.assertGreater(len(values),3);self.assertTrue(all(v['position'] is None and v['status']=='insufficient_geometry' for v in values))
   self.assertEqual(len(output.read_text().splitlines()),22)
 def test_malformed_v3_crc_and_length(self):
  record=self.records(True)[0]
  self.assertEqual(record['legacy_length'],34);self.assertNotIn('raw_mm',record)
  line=subprocess.check_output([str(self.producer),'1'],text=True).splitlines()[0]
  for at in (0,3,5,100,308):
   broken=line[:at]+('X' if line[at]!='X' else 'Y')+line[at+1:]
   with self.assertRaises(ValueError):self.collect.decode(broken)
if __name__=='__main__':unittest.main()
