import importlib.util
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import uwb_mobile as m
B=dict(max_speed_m_s=.5,max_acceleration_m_s2=.3,min_reference_distance_m=1.,
       relative_clock_slew_ppb_s=2.,oscillator_ppm_bound=100.,baseline_max_m=5.,metrology_error_m=0.)

def arrival(emission,anchor,trajectory):
    # Solve light travel time in physical SI world, independent of estimator.
    t=emission
    for _ in range(12):
        x,y=trajectory(t);t=emission+math.hypot(x-anchor,y)/m.C
    return t

class Mobile(unittest.TestCase):
 def test_moving_2d_acceleration_and_clock_slew_signed_truth_in_envelope(self):
  path=lambda t:(2+.03*t+.0003*t*t,1+.15*math.sin(t/3))
  receiver=lambda t:(1+25e-6)*t+.5e-9*t*t
  for slot in (.5,.05,.01):
   for cycle in range(30):
    t0=arrival(cycle,0,path);t1=arrival(cycle+1,0,path);tf=arrival(cycle+slot,5,path)
    offset=m.MASK-1000
    rx=lambda t:(round(receiver(t)*m.HZ)+offset)&m.MASK
    out=m.interpolate(m.HZ,m.delta(rx(t1),rx(t0)),round(slot*m.HZ),m.delta(rx(tf),rx(t0)),B)
    self.assertEqual(out['status'],'conditional')
    x,y=path(tf);truth=math.hypot(x-5,y)-math.hypot(x,y)
    self.assertLessEqual(out['interval_m'][0],truth);self.assertGreaterEqual(out['interval_m'][1],truth)
    self.assertFalse(out['oscillator_rate_identified']);self.assertIsNone(out['measurement'])
 def test_stationary_baseline_positive_negative_sign_and_representation_wrap(self):
  for x in (1.,4.):
   path=lambda t:(x,1.)
   ts=[arrival(0,0,path),arrival(1,0,path),arrival(.5,5,path)]
   rx=lambda t:(m.MASK-500+round((1-18e-6)*t*m.HZ))&m.MASK
   out=m.interpolate(m.HZ,m.delta(rx(ts[1]),rx(ts[0])),m.HZ//2,m.delta(rx(ts[2]),rx(ts[0])),B)
   truth=math.hypot(x-5,1)-math.hypot(x,1)
   self.assertEqual(out['status'],'conditional')
   self.assertGreater(out['nominal_estimate_m']*truth,0)
   self.assertLessEqual(abs(out['nominal_estimate_m']-truth),.01)
 def test_unknown_bounds_invalid_values_missing_reordered_and_extrapolation(self):
  args=(m.HZ,m.HZ,m.HZ//2,m.HZ//2)
  self.assertEqual(m.interpolate(*args,None)['status'],'assumptions_required')
  for bad in ({},dict(B,max_speed_m_s=-1),dict(B,oscillator_ppm_bound=float('nan')),
              dict(B,min_reference_distance_m=0),dict(B,max_acceleration_m_s2=True),dict(B,extra=1),dict(B,baseline_max_m=10**1000)):
   self.assertEqual(m.interpolate(*args,bad)['status'],'invalid_assumptions')
  for bad in ((2*m.HZ,2*m.HZ,m.HZ//2,m.HZ//2),(m.HZ,-m.HZ,m.HZ//2,m.HZ//2),
              (m.HZ,m.HZ,m.HZ+1,m.HZ//2),(m.HZ,m.HZ,True,m.HZ//2)):
   self.assertNotEqual(m.interpolate(*bad,B)['status'],'conditional')
  impossible=m.interpolate(m.HZ,m.HZ,m.HZ//2,m.HZ//2+round(50/m.C*m.HZ),B)
  self.assertEqual(impossible['status'],'inconsistent');self.assertIsNone(impossible['measurement'])
 def test_physical_acceleration_term_and_slot_scaling(self):
  b=dict(B,max_speed_m_s=0,max_acceleration_m_s2=1,relative_clock_slew_ppb_s=0,oscillator_ppm_bound=0)
  half=m.interpolate(m.HZ,m.HZ,m.HZ//2,m.HZ//2,b)
  short=m.interpolate(m.HZ,m.HZ,m.HZ//100,m.HZ//100,b)
  self.assertAlmostEqual(half['interpolation_error_m'],.125,places=8)
  self.assertAlmostEqual(short['interpolation_error_m'],.00495,places=8)

class WireMobile(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.work=tempfile.TemporaryDirectory();cls.producer=Path(cls.work.name)/'producer'
  core=ROOT/'ThirdParty/mul_core'
  subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-DMUL_PROP_MAX_AGE_MS=6000',
   '-I'+str(core),str(ROOT/'tests/fixtures/hybrid_capture.c'),*[str(core/(name+'.c')) for name in ('mul_common','mul_telemetry','mul_relation','mul_clock')],'-o',str(cls.producer)],check=True)
  from uwb_collect import decode
  cls.records=[dict(decode(line),kind='telemetry') for line in subprocess.check_output([str(cls.producer),'0'],text=True).splitlines()]
 @classmethod
 def tearDownClass(cls):cls.work.cleanup()
 def test_actual_wire_shared_relation_default_no_rate_and_conditional_mobile(self):
  replay=m.MobileReplay(B)
  try:
   output=[row for r in self.records if (row:=replay.feed(r)) is not None]
   self.assertGreater(len(output),3)
   for row in output:
    self.assertEqual(row['status'],'conditional');self.assertIsNone(row['measurement'])
    truth=477*m.C/m.HZ
    self.assertLessEqual(row['interval_m'][0],truth);self.assertGreaterEqual(row['interval_m'][1],truth)
    self.assertTrue(row['output_delayed_until_next_reference'])
  finally:replay.close()
 def test_loss_duplicate_bad_age_and_epoch_do_not_create_measurements(self):
  for transform in ('missing','reordered','duplicate','wrap','age','epoch','generation'):
   records=[dict(r) for r in self.records]
   if transform=='missing':records.pop(14) # Lose reference7, which would close pair6.
   if transform=='reordered':records[12],records[13]=records[13],records[12]
   if transform=='duplicate':records=[r for item in records for r in (item,dict(item))]
   if transform=='wrap':
    for r in records:r['uptime_ms']=(r['uptime_ms']+(1<<32)-6000)&0xffffffff
   if transform=='age':
    for r in records:r['capture_age_ms']=65535
   if transform=='epoch':
    for i,r in enumerate(records):r['epoch']=f'{i+100:016X}'
   if transform=='generation':
    for i,r in enumerate(records):r['reference_generation']=i+1
   replay=m.MobileReplay(B)
   try:
    output=[row for r in records if (row:=replay.feed(r)) is not None]
    self.assertTrue(all(r['measurement'] is None for r in output))
    count=sum(r['status']=='conditional' for r in output)
    if transform in ('age','epoch','generation'):self.assertEqual(count,0)
    if transform in ('duplicate','wrap'):self.assertGreater(count,3)
    if transform in ('missing','reordered'):self.assertLess(count,5)
   finally:replay.close()
 def test_native_context_recreation_cannot_close_old_mobile_pair(self):
  replay=m.MobileReplay(B)
  try:
   # ref5/fol5 create the first valid candidate. Simulate the native context
   # being evicted at its bounded capacity before ref6, without a radio epoch.
   for r in self.records[:12]:replay.feed(r)
   self.assertTrue(any('pair' in state for state in replay.states.values()))
   key=next(iter(replay.shared.contexts))
   replay.shared.lib.hybrid_free(replay.shared.contexts.pop(key))
   replay.shared.generations.pop(key)
   self.assertIsNone(replay.feed(self.records[12]))
  finally:replay.close()
 def test_executable_requires_explicit_bounds_and_writes_conditional_jsonl(self):
  with tempfile.TemporaryDirectory() as temp:
   d=Path(temp);capture=d/'capture.jsonl';bounds=d/'bounds.json';output=d/'out.jsonl'
   capture.write_text(''.join(json.dumps(r)+'\n' for r in self.records));bounds.write_text(json.dumps(B))
   subprocess.run([sys.executable,str(ROOT/'tools/uwb_mobile.py'),str(capture),'--bounds',str(bounds),'--output',str(output)],check=True)
   rows=[json.loads(line) for line in output.read_text().splitlines()]
   self.assertGreater(len(rows),3);self.assertTrue(all(not r['point_measurement_valid'] for r in rows))
if __name__=='__main__':unittest.main()
