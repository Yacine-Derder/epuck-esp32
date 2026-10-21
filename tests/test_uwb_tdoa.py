import importlib.util
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location('tdoa',Path(__file__).parents[1]/'tools/uwb_tdoa.py');t=importlib.util.module_from_spec(spec);spec.loader.exec_module(t)

def record(node,seq,tx,rx,epoch='0000000000000001',remote='0000000000000002'):
    return dict(kind='telemetry',mode=3,type='observation',node=node,peer=7,radio=2,remote_radio=1,epoch=epoch,remote_epoch=remote,
                transaction=seq,flags=8,quality=0,model_version=seq,stamps=[f'{rx%t.MOD:010X}',f'{tx%t.MOD:010X}'])
def mapping(node,local,ref):
    return dict(domain=dict(node=node,radio=2,epoch='0000000000000001'),local_anchor=f'{local:010X}',reference_anchor=f'{ref:010X}',
                rate='1',max_delta_dtu=t.MAX,uncertainty_dtu=10,provenance='synthetic independent two-way reference fixture')
class Tdoa(unittest.TestCase):
    def test_no_invented_sync(self):
        rows=list(t.analyze([record(10,1,100,200),record(11,1,100,999999)]))
        self.assertEqual(rows[-1]['status'],'missing_sync');self.assertNotIn('tdoa_ns',rows[-1])
        self.assertFalse(rows[1]['absolute_offset_identifiable'])
    def test_wrap_external_mapping_and_epoch(self):
        maps=t.Maps(dict(target_domain=dict(node=0,radio=1,epoch='0000000000000009'),mappings=[mapping(10,t.MOD-100,t.MOD-100),mapping(11,1000,t.MOD-100)]))
        rows=list(t.analyze([record(10,2,0,50),record(11,2,0,1155)],maps))
        self.assertEqual(rows[-1]['status'],'externally_mapped');self.assertEqual(rows[-1]['tdoa_b_minus_a_dtu'],5)
        self.assertEqual(rows[-1]['uncertainty_bound_dtu'],20)
        changed=list(t.analyze([record(10,2,0,50),record(11,2,0,1155,epoch='0000000000000003')],maps))
        self.assertEqual(changed[-1]['status'],'missing_sync')
    def test_drift_duplicates_outlier_gap_and_partial_sync(self):
        model=t.Arrival()
        for i in range(30):self.assertTrue(model.update((t.MOD-100+i*t.HZ)%t.MOD,(1000000+i*(t.HZ+t.HZ//50000))%t.MOD))
        self.assertTrue(model.valid);self.assertAlmostEqual(float(model.rate),1.00002,places=8)
        self.assertFalse(model.update(*model.last));self.assertFalse(model.update((model.last[0]+t.HZ)%t.MOD,(model.last[1]+t.HZ+999999)%t.MOD))
        self.assertFalse(model.update((model.last[0]+t.HZ*6)%t.MOD,(model.last[1]+t.HZ*6)%t.MOD));self.assertFalse(model.valid)
        rows=list(t.analyze([record(10,1,0,1),record(11,1,0,2,remote='0000000000000004')]))
        self.assertEqual(sum(r.get('status')=='missing_receiver' for r in rows),2)
    def test_conflicting_remote_tx_and_nonobjects(self):
        rows=list(t.analyze([[],record(10,1,100,500),record(11,1,101,600)]))
        self.assertEqual(sum(r['kind']=='invalid' for r in rows),2)
        for value in ('+000000001',' 000000001','10000000000'):
            with self.assertRaises(ValueError):t.stamp(value)
    def test_invalid_conflict_limits(self):
        bad=record(10,1,0,1);bad['stamps'][0]='10000000000'
        rows=list(t.analyze([bad,record(10,1,0,1),record(10,1,0,2),record(10,2,1,2)],max_groups=1))
        self.assertEqual(sum(r['kind']=='invalid' for r in rows),2);self.assertTrue(any(r.get('evicted') for r in rows))
        m=mapping(10,0,0);m['rate']='1.1'
        with self.assertRaises(ValueError):t.Maps(dict(target_domain=dict(node=0,radio=1,epoch='0000000000000001'),mappings=[m]))
if __name__=='__main__':unittest.main()
