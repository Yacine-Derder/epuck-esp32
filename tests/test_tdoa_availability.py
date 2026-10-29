import unittest
from tools.tdoa_availability import summarize
class Availability(unittest.TestCase):
    def test_distinguishes_observability_and_boot_counters(self):
        rows=[]
        for cycle,(seen,fol,valid,relation,status,delay) in enumerate(((3,True,True,True,2,1),(2,True,False,False,2,0),(1,False,False,False,2,0),(3,True,False,False,2,0),(3,True,False,False,2,3),(3,True,False,True,1,1))):
            rows.append(dict(type='tdoa_vector',s='s',cycle=cycle,seen_mask=seen,followers=[dict(slot=1,seen=fol,valid=valid)]))
            rows.append(dict(type='tdoa_replay',s='s',cycle=cycle,pair_slot=1,relation=dict(version=1,available=relation,delay_state=delay),clock=dict(status=status),diagnostic_dropped=100+(cycle>=4)))
        out=summarize(rows)
        self.assertEqual(out['valid'],1);self.assertEqual(out['diagnostic_dropped_capture_increments'],1)
        for key in ('reference_not_observed','follower_not_observed','follower_relation_unavailable_cause_unknown','propagation_expired','receiver_clock_invalid'):self.assertEqual(out['causes'][key],1)
