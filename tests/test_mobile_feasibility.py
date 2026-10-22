"""World-model counterexample, independent of an estimator implementation."""
import math
import unittest
C=299792458.0
HZ=63897600000
MASK=(1<<40)-1
class Feasibility(unittest.TestCase):
 def test_two_anchor_all_packet_times_cannot_identify_true_clock(self):
  alpha=1+20e-6;speed=.2;x0=10.;beta=.003
  apparent=alpha/(1-speed/C)
  # All broadcasts and directed TWR types; fixed anchors0/5m behind the robot.
  packets=[(0,0.),(5,.5),(0,.65),(5,.67),(0,.69),(5,.71),(0,.73)]
  for cycle in range(30):
   for anchor,offset in packets:
    emission=cycle+offset
    moving_arrival=(emission+(x0-anchor)/C)/(1-speed/C)
    moving_stamp=alpha*moving_arrival+beta
    stationary_stamp=apparent*(emission+(x0-anchor)/C)+beta
    self.assertAlmostEqual(moving_stamp,stationary_stamp,places=13)
    self.assertLessEqual(abs(round(moving_stamp*HZ)-round(stationary_stamp*HZ)),1)
    self.assertNotEqual(alpha,apparent)
  self.assertAlmostEqual((apparent/alpha-1)*.5*C,.1,places=6)
 def test_same_clock_cross_time_range_difference_is_not_simultaneous(self):
  x0=2.;v=.2;slot=.5
  t_ref=x0/(C-v);t_fol=(slot+(5-x0)/C)/(1+v/C)
  arrival_difference=C*(t_fol-t_ref-slot)
  simultaneous=5-2*(x0+v*t_fol)
  self.assertAlmostEqual(arrival_difference-simultaneous,v*(t_fol-t_ref),places=6)
 def test_apparent_arrival_mapping_cancels_linear_radial_motion(self):
  x0=2.;v=.2;alpha=1+20e-6
  ref_rx=lambda emission:alpha*(emission+x0/C)/(1-v/C)
  t_fol=(.5+(5-x0)/C)/(1+v/C)
  apparent=ref_rx(1.)-ref_rx(0.)
  interpolated=.5*(ref_rx(0.)+ref_rx(1.))
  difference=(alpha*t_fol-interpolated)/apparent*C
  truth=5-2*(x0+v*t_fol)
  self.assertAlmostEqual(difference,truth,places=6)
if __name__=='__main__':unittest.main()
