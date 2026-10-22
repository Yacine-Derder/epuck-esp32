import math
import json
import subprocess
import tempfile
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).parents[1]/'tools'))
import uwb_frequency as f
class Frequency(unittest.TestCase):
 def line(self,ci=-12345,coe=-2048,present=1):
  return f'MULH FREQ slot=0 peer=0x84 type=0x01 seq=10 rx=FFFFFFFFFF observed_ms=4294967295 age_ms=1 ci_raw={ci} coe_raw={coe} coe_present={present} status=0x00004400 metric_rate=UNKNOWN'
 def test_raw_api_scaling_sign_ambiguity_and_zero_is_not_missing(self):
  for ci in (-1048576,-1,0,1,1048575):
   row=f.decode(self.line(ci,0));self.assertEqual(row['ci_raw'],ci)
   self.assertAlmostEqual(row['ci_api_offset_hz'],ci*3.719329833984375)
   self.assertFalse(row['metric_rate_valid']);self.assertIsNone(row['measurement'])
   self.assertFalse(row['oscillator_rate_identified'])
  row=f.decode(self.line(0,0));self.assertTrue(row['coe_present']);self.assertEqual(row['coe_api_scaled_ppb'],0)
  row=f.decode(self.line(0,0,0));self.assertIsNone(row['coe_raw']);self.assertIsNone(row['coe_api_scaled_ppb'])
  self.assertAlmostEqual(f.decode(self.line(0,1))['coe_api_scaled_ppb'],14.901161193847656)
 def test_unavailable_malformed_width_duplicate_and_unknown_status(self):
  self.assertIsNone(f.decode('other console output'))
  self.assertEqual(f.decode('MULH FREQ slot=2 absent')['kind'],'frequency_unavailable')
  self.assertEqual(f.decode('MULH FREQ disabled; build with MUL_HYBRID_FREQ_DIAG=ON; metric_rate=UNKNOWN')['kind'],'frequency_unavailable')
  for bad in (self.line(1<<20),self.line(0,4096),self.line(0,5,0),self.line()+' seq=20',self.line().replace('UNKNOWN','VALID'),self.line().replace('age_ms=1','age_ms=65536')):
   with self.assertRaises(ValueError):f.decode(bad)
 def test_configurable_sensitivity_separates_clock_motion_hold_and_slot(self):
  scenario=dict(radial_velocity_error_m_s=.2,frequency_error_hz=1.,calibration_error_ppb=2.,clock_slew_ppb_s=1.,hold_seconds=10.,speed_m_s=.2)
  long=f.sensitivity(scenario,500);short=f.sensitivity(scenario,10)
  self.assertAlmostEqual(long['doppler_equivalent_range_error_m'],.1)
  self.assertAlmostEqual(long['stationary_hold_rate_error_ppb'],12.)
  self.assertAlmostEqual(long['cross_time_motion_bound_m'],.1)
  self.assertAlmostEqual(long['coe_api_one_count_equivalent_range_m'],2.2336278706789017)
  self.assertAlmostEqual(long['frequency_equivalent_range_error_m']/short['frequency_equivalent_range_error_m'],50.)
  self.assertFalse(long['metric_rate_valid'])
  for bad in ({},dict(scenario,hold_seconds=-1),dict(scenario,clock_slew_ppb_s=math.nan),dict(scenario,frequency_error_hz=True),dict(scenario,clock_slew_ppb_s=10**300,hold_seconds=10**300)):
   with self.assertRaises(ValueError):f.sensitivity(bad,500)
 def test_noiseless_carrier_and_cadence_have_same_clock_motion_ambiguity(self):
  alpha=1+20e-6;v=.2;apparent=alpha/(1-v/f.C)
  moving_carrier=(1-v/f.C)/alpha;stationary_carrier=1/apparent
  self.assertAlmostEqual(moving_carrier,stationary_carrier,places=15)
  self.assertAlmostEqual(moving_carrier*apparent,1.,places=15)
 def test_two_known_bearings_still_leave_clock_plus_2d_velocity_underdetermined(self):
  # Apparent fractional rates: clock + radial speed/c. Two independent bearings
  # still give only two equations in clock,vx,vy. Exhibited null direction.
  k=.1/f.C;clock=20e-6;vx=.15;vy=-.1
  initial=(clock+vx/f.C,clock+vy/f.C)
  changed=(clock+k+(vx-.1)/f.C,clock+k+(vy-.1)/f.C)
  for a,b in zip(initial,changed):self.assertAlmostEqual(a,b,places=15)
  # One independently known radial component removes this specific ambiguity.
  self.assertAlmostEqual(initial[0]-vx/f.C,clock,places=15)
 def test_cli_schema_errors_and_explicit_scenarios(self):
  with tempfile.TemporaryDirectory() as directory:
   root=Path(directory);log=root/'serial.log';out=root/'frequency.jsonl';scenario=root/'scenario.json'
   log.write_text('unrelated console output\n'+self.line()+'\n'+self.line(1<<20)+'\n')
   script=Path(f.__file__)
   subprocess.run([sys.executable,str(script),'decode',str(log),'--output',str(out)],check=True,capture_output=True)
   rows=[json.loads(line) for line in out.read_text().splitlines()]
   self.assertEqual([r['kind'] for r in rows],['hybrid_frequency_raw','invalid_frequency_snapshot'])
   self.assertTrue(all(not r['metric_rate_valid'] for r in rows))
   self.assertNotEqual(subprocess.run([sys.executable,str(script),'decode',str(log),'--output',str(out)],capture_output=True).returncode,0)
   scenario.write_text(json.dumps(dict(radial_velocity_error_m_s=.2,frequency_error_hz=1,calibration_error_ppb=2,clock_slew_ppb_s=1,hold_seconds=10,speed_m_s=.2)))
   result=subprocess.run([sys.executable,str(script),'sensitivity','--scenario',str(scenario)],check=True,capture_output=True,text=True)
   self.assertEqual([json.loads(line)['slot_ms'] for line in result.stdout.splitlines()],[500,50,10])
   scenario.write_text('{}')
   self.assertNotEqual(subprocess.run([sys.executable,str(script),'sensitivity','--scenario',str(scenario)],capture_output=True).returncode,0)
if __name__=='__main__':unittest.main()
