import copy
import unittest
from tools.antenna_calibration import propose,C_M_S,DTU_HZ
class Antenna(unittest.TestCase):
    def fixture(self):
        offsets={132:120,133:-40,141:60}
        pairs=[]
        for a,b in ((132,133),(132,141),(133,141)):
            measured=2+(offsets[a]+offsets[b])*C_M_S/(2*DTU_HZ)
            pairs.append(dict(nodes=[a,b],known_m=2,ranges_m=[measured]*5+[20],power_policy='applied'))
        return dict(additive_correction_removed=True,devices={str(n):dict(tx=14000,rx=18000) for n in offsets},pairs=pairs)
    def test_three_pair_sign_factor_split_and_no_mutation(self):
        data=self.fixture();old=copy.deepcopy(data);out=propose(data)
        self.assertEqual([p['aggregate_delta_dtu'] for p in out['proposals']],[120,-40,60]);self.assertEqual(data,old)
        for p in out['proposals']:self.assertEqual(p['delta_tx_dtu']+p['delta_rx_dtu'],p['aggregate_delta_dtu'])
    def test_requires_all_pairs_and_consistent_policy(self):
        d=self.fixture();d['pairs'].pop()
        with self.assertRaises(ValueError):propose(d)
        d=self.fixture();d['pairs'][0]['power_policy']='raw'
        with self.assertRaises(ValueError):propose(d)
    def test_rejects_invalid_measurements_and_overflow(self):
        for value in (float('nan'),float('inf'),-1):
            d=self.fixture();d['pairs'][0]['known_m']=value
            with self.assertRaises(ValueError):propose(d)
        d=self.fixture();d['devices']['132']['tx']=65535
        with self.assertRaises(ValueError):propose(d)
