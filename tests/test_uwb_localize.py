import importlib.util
from pathlib import Path
import math
import unittest
spec=importlib.util.spec_from_file_location('localize',Path(__file__).parents[1]/'tools/uwb_localize.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
A={'0':[0,0],'1':[5,0],'2':[0,5],'3':[5,5]}
def observations(position,kind):
    distances={k:math.dist(v,position) for k,v in A.items()}
    return [dict(kind=kind,anchor=k,reference='0',value_m=d if kind=='range' else d-distances['0'],valid=True) for k,d in distances.items() if kind=='range' or k!='0']
class Localization(unittest.TestCase):
    def test_ground_truth_ranges_tdoa_and_hybrid(self):
        for p in ((1.,2.),(3.,4.),(-2.,1.),(2.5,2.5)):
            for kind in ('range','range_difference'):
                r=m.solve_2d(A,observations(p,kind));self.assertEqual(r['status'],'available',r);self.assertLess(math.dist(p,[r['position']['x'],r['position']['y']]),1e-5)
            hybrid=observations(p,'range')[:2]+observations(p,'range_difference')[1:]
            self.assertEqual(m.solve_2d(A,hybrid)['status'],'available')
    def test_two_alr_and_collinear_are_not_positions(self):
        r=m.solve_2d({'132':[0,0],'133':[3,0]},[dict(kind='range_difference',reference='132',anchor='133',value_m=1,valid=True)])
        self.assertEqual(r['status'],'insufficient_geometry');self.assertIsNone(r['position'])
        line={str(i):[i,0] for i in range(4)}
        self.assertEqual(m.solve_2d(line,observations((1,2),'range'))['status'],'degenerate_geometry')
    def test_invalid_stale_impossible_and_inconsistent(self):
        data=observations((1,2),'range')
        for item in data:item['valid']=False
        self.assertEqual(m.solve_2d(A,data)['status'],'insufficient_geometry')
        data=observations((1,2),'range_difference');data[0]['value_m']=100
        self.assertEqual(m.solve_2d(A,data)['status'],'impossible_range_difference')
        data=observations((1,2),'range');data[0]['value_m']+=5
        self.assertEqual(m.solve_2d(A,data)['status'],'inconsistent_measurements')
        data[0]['value_m']=float('nan');self.assertEqual(m.solve_2d(A,data)['status'],'invalid_measurement')
    def test_ambiguous_and_bounded_geometry(self):
        a={k:v for k,v in A.items() if k!='3'};p=(-2,-2);d={k:math.dist(v,p) for k,v in a.items()}
        data=[dict(kind='range_difference',reference='0',anchor=k,value_m=d[k]-d['0'],valid=True) for k in ('1','2')]
        self.assertEqual(m.solve_2d(a,data)['status'],'insufficient_constraints_without_bounds')
        self.assertEqual(m.solve_2d(a,data,bounds=(-20,20,-20,20))['status'],'ambiguous_geometry')
        self.assertEqual(m.solve_2d(a,data,bounds=(-4,-1,-4,-1))['status'],'available')
    def test_duplicate_and_dependent_edges_are_not_new_geometry(self):
        a={'0':[0,0],'1':[5,0],'2':[0,5]};p=(1,2);d={k:math.dist(v,p) for k,v in a.items()}
        data=[dict(kind='range_difference',reference=i,anchor=j,value_m=d[j]-d[i],valid=True) for i,j in (('0','1'),('0','2'),('1','2'))]
        self.assertEqual(m.solve_2d(a,data)['status'],'insufficient_constraints_without_bounds')
        data.append(dict(data[0]));self.assertEqual(m.solve_2d(a,data)['status'],'insufficient_constraints_without_bounds')
        data[-1]['value_m']+=1;self.assertEqual(m.solve_2d(a,data)['status'],'conflicting_measurements')
    def test_normalized_ids_and_no_fake_uncertainty(self):
        r=m.solve_2d(A,observations((1,2),'range'));self.assertEqual(r['physical_uncertainty'],'unknown');self.assertLess(r['rms_m'],1e-6)
        self.assertEqual(m.identity('0x84'),'132')
if __name__=='__main__':unittest.main()
