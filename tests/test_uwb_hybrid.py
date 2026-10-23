import importlib.util
from pathlib import Path
import subprocess
import tempfile
import json
import math
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
  cls.producers={500:cls.producer}
  for slot in (50,10):
   executable=Path(cls.work.name)/f'producer-{slot}'
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-DMUL_PROP_MAX_AGE_MS=6000',f'-DMUL_FOLLOWER_SLOT_MS={slot}','-I'+str(core),str(ROOT/'tests/fixtures/hybrid_capture.c'),str(core/'mul_common.c'),str(core/'mul_telemetry.c'),str(core/'mul_relation.c'),str(core/'mul_clock.c'),'-o',str(executable)],check=True)
   cls.producers[slot]=executable
  cls.collect=module('uwb_collect');cls.host=module('uwb_hybrid');cls.loc=module('uwb_localize')
 @classmethod
 def tearDownClass(cls):cls.work.cleanup()
 def records(self,stationary):
  lines=subprocess.check_output([str(self.producer),str(int(stationary))],text=True).splitlines()
  return [dict(self.collect.decode(line),kind='telemetry') for line in lines]
 def test_runtime_schedule_advertisements_replay_all_slots(self):
  for slot in (10,50,500):
   replay=self.host.Replay(slot)
   try:
    args=[str(self.producers[slot]),'1','4000','4477','10000','-5000','4263','900000','132','133','1']
    records=[dict(self.collect.decode(line),kind='telemetry') for line in subprocess.check_output(args,text=True).splitlines()]
    tag={10:0x40,50:0x80,500:0xc0}[slot]
    self.assertTrue(all(bytes.fromhex(r['legacy_frame'])[24]&0xc0==tag for r in records))
    rows=[replay.feed(r) for r in records]
    self.assertGreater(sum(r.get('status')=='AVAILABLE' for r in rows),3)
    self.assertTrue(all(r.get('slot_ms')==slot for r in rows))
   finally:replay.close()
 def test_actual_wire_collector_shared_replay_and_insufficient_position(self):
  replay=self.host.Replay()
  try:
   rows=[replay.feed(r) for r in self.records(True)]
   observations=[r for r in rows if r.get('status')=='AVAILABLE'];self.assertGreater(len(observations),3)
   for r in observations:
    self.assertLessEqual(abs(r['tdoa_dtu']-477),2);self.assertTrue(r['stationarity_assumed']);self.assertFalse(r['remote_boot_epoch_known'])
    result=self.loc.solve_2d({'0x84':[0,0],'0x85':[5,0]},[r['measurement']]);self.assertEqual(result['status'],'insufficient_geometry')
  finally:replay.close()
 def test_physical_geometry_signed_full_wire_all_slots_and_independent_clocks(self):
  hz=63897600000;c=299792458
  # Reference (0,0), follower (5,0); separate stationary receiver positions
  # lie on both sides of, and on, the perpendicular bisector.
  # Path lengths and expected sign come from geometry, not receiver implementation.
  for slot in (500,50,10):
   replay=self.host.Replay(slot)
   try:
    for policy in (False,True):
     for x,y in ((1.,1.),(2.5,1.),(4.,1.)):
      for receiver_ppb,follower_ppb in ((75000,-35000),(-65000,45000)):
       path_a=math.hypot(x,y);path_b=math.hypot(x-5,y)
       args=[str(self.producers[slot]),str(int(policy)),str(round(path_a*hz/c)),str(round(path_b*hz/c)),str(receiver_ppb),str(follower_ppb),str(round(5*hz/c)),str((1<<40)-300000)]
       records=[dict(self.collect.decode(line),kind='telemetry') for line in subprocess.check_output(args,text=True).splitlines()]
       self.assertGreater(int(records[0]['rx'],16),int(records[2]['rx'],16)) # Native receiver40-bit wrap.
       # Separate physical captures/clock hypotheses must not share replay state.
       for record in records:record['epoch']=f'{1+int(policy)*10000+round(x*100)+receiver_ppb+100000:016X}'
       rows=[replay.feed(record) for record in records]
       observations=[row for row in rows if row.get('associated') and row['clock_valid']]
       self.assertEqual(len(observations),6)
       for row in observations:
        if not policy:
         self.assertEqual(row['status'],'NO_RATE');self.assertIsNone(row['measurement']);self.assertIsNone(row['tdoa_dtu'])
        else:
         self.assertEqual(row['status'],'AVAILABLE')
         self.assertLessEqual(abs(row['tdoa_dtu']-(path_b-path_a)*hz/c),3)
         self.assertLessEqual(abs(row['receiver_rate_ppb']-receiver_ppb),1)
         self.assertAlmostEqual(row['measurement']['value_m'],path_b-path_a,delta=3*c/hz)
         self.assertTrue(row['stationarity_assumed'])
         if x!=2.5:self.assertGreater(row['tdoa_dtu']*(path_b-path_a),0)
   finally:replay.close()
 def test_custom_and_swapped_rosters_survive_full_wire_replay_without_global_routing(self):
  for slot in (500,50,10):
   for reference,follower in ((0x85,0x84),(0x42,0x23)):
    for policy in (False,True):
     args=[str(self.producers[slot]),str(int(policy)),'4000','4477','10000','-5000','4263',str((1<<40)-300000),str(reference),str(follower)]
     records=[dict(self.collect.decode(line),kind='telemetry') for line in subprocess.check_output(args,text=True).splitlines()]
     custom=self.host.Replay(slot,reference,follower);default=self.host.Replay(slot)
     try:
      rows=[custom.feed(record) for record in records]
      values=[row for row in rows if row.get('associated') and row['clock_valid']]
      self.assertEqual(len(values),6)
      for row in values:
       self.assertEqual((row['reference'],row['follower']),(reference,follower))
       self.assertEqual(row['status'],'AVAILABLE' if policy else 'NO_RATE')
       self.assertIsNotNone(row['tx_separation_ref_dtu'])
       if policy:self.assertLessEqual(abs(row['tdoa_dtu']-477),2)
       else:self.assertIsNone(row['measurement'])
      # Default and custom contexts are live together; no mutable global roster.
      self.assertFalse(any(default.feed(record).get('clock_valid') for record in records))
      self.assertEqual(custom.feed(dict(records[0],node=reference))['kind'],'invalid')
     finally:custom.close();default.close()
  with tempfile.TemporaryDirectory() as directory:
   capture=Path(directory)/'capture.jsonl';output=Path(directory)/'observations.jsonl'
   capture.write_text(''.join(json.dumps(record)+'\n' for record in records))
   subprocess.run([sys.executable,str(ROOT/'tools/uwb_hybrid.py'),str(capture),'--slot','10','--reference','0x42','--follower','0x23','--output',str(output)],check=True)
   self.assertEqual(sum(json.loads(line).get('status')=='AVAILABLE' for line in output.read_text().splitlines()),6)
   previous=output.read_bytes()
   bad=subprocess.run([sys.executable,str(ROOT/'tools/uwb_hybrid.py'),str(capture),'--reference','255','--output',str(output)],capture_output=True,text=True)
   self.assertEqual(bad.returncode,2);self.assertNotIn('Traceback',bad.stderr);self.assertEqual(previous,output.read_bytes())
  for pair in ((1,1),(-1,2),(1,255),(True,2)):
   with self.assertRaises(ValueError):self.host.Replay(reference=pair[0],follower=pair[1])
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
