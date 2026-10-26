import binascii
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.uwb_collect import decode
from tools.uwb_logging import CompactLog
ROOT=Path(__file__).parents[1]
def wire(b):
    b=bytearray(b);b[-2:]=binascii.crc_hqx(b[:-2],0xffff).to_bytes(2,'little');return 'UWB3 '+b.hex().upper()
class Ar(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work=tempfile.TemporaryDirectory();core=ROOT/'ThirdParty/mul_core';exe=Path(cls.work.name)/'emit'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(core),str(core/'tests/test_ar_telemetry.c'),str(core/'mul_telemetry.c'),'-o',str(exe)],check=True)
        cls.lines=subprocess.check_output([str(exe),'emit'],text=True).splitlines()
    @classmethod
    def tearDownClass(cls):cls.work.cleanup()
    def test_mixed_clock_result_and_reset_quality_survive_compact(self):
        r,q=map(decode,self.lines)
        self.assertEqual(r['type'],'ar_range');self.assertEqual(r['raw_tof_dtu'],2131)
        self.assertEqual(r['stamp_clock_nodes'],[33,141,141,33,33,141]);self.assertEqual(r['tof_clock_node'],33)
        self.assertEqual(r['physical_uncertainty'],'UNKNOWN');self.assertFalse(r['calibrated']);self.assertEqual(r['measurement_age_at_report_ms'],31)
        self.assertEqual(q['type'],'ar_quality');self.assertEqual(q['reset_generation'],4);self.assertEqual(q['last_reason'],'deadline')
        stream=io.StringIO();log=CompactLog(stream,'test')
        for row in (r,q):log.accept(dict(row,kind='telemetry',host_received_utc='2026-10-26T15:00:00Z'))
        rows=[r for r in map(json.loads,stream.getvalue().splitlines()) if r['type'].startswith('ar_')]
        self.assertEqual(len(rows),2);self.assertEqual(rows[0]['stamps'],r['stamps']);self.assertEqual(rows[1]['reset_generation'],4)
    def test_crc_valid_wrong_domain_quality_and_unknown_semantics_rejected(self):
        for index,mutations in enumerate((((11,3),(53,142),(109,0),(110,0),(111,2),(140,1),(54,32),(9,11),(97,65)),((97,1),(99,1),(100,16),(104,2),(149,1)))):
            original=bytearray.fromhex(self.lines[index][5:])
            for offset,value in mutations:
                b=original.copy();b[offset]=value
                with self.assertRaises(ValueError,msg=(index,offset)):decode(wire(b))
