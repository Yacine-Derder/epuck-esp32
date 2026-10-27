import binascii
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.uwb_collect import decode
from tools.uwb_logging import CompactLog

ROOT=Path(__file__).resolve().parents[1]

class PoseProvenance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work=tempfile.TemporaryDirectory()
        core=ROOT/'ThirdParty/mul_core'
        exe=Path(cls.work.name)/'emit'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(core),
                        str(core/'tests/test_pose_telemetry.c'),str(core/'mul_telemetry.c'),'-o',str(exe)],check=True)
        cls.lines=subprocess.check_output([str(exe),'emit'],text=True).splitlines()

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def test_actual_c_provenance_survives_debug_and_compact(self):
        vector,external,none=map(decode,self.lines)
        self.assertEqual([v['propagation_source'] for v in vector['followers']],['twr','external','none'])
        self.assertEqual(external['propagation_source'],'external')
        self.assertEqual(none['propagation_source'],'none')
        output=io.StringIO()
        log=CompactLog(output,'pose')
        for r in (vector,external,none):
            self.assertEqual(r['physical_uncertainty'],'UNKNOWN')
            self.assertEqual(r['propagation_metadata_version'],1)
            log.accept(dict(r,kind='telemetry',host_received_utc='2026-10-27T00:00:00Z'))
        rows=[r for r in map(json.loads,output.getvalue().splitlines()) if r['type'] in ('tdoa_vector','tdoa')]
        self.assertEqual(len(rows),3)
        self.assertEqual(rows[0]['followers'],vector['followers'])
        self.assertEqual([r['propagation_source'] for r in rows[1:]],['external','none'])

    def test_crc_valid_ambiguous_or_invalid_provenance_is_rejected(self):
        for index,changes in ((0,((149,2),(149,0),(80,0),(100,200))),
                              (1,((115,0),(116,3),(117,1),(114,4))),
                              (2,((63,1),(112,1)))):
            for offset,value in changes:
                data=bytearray.fromhex(self.lines[index][5:])
                data[offset]=value
                data[-2:]=binascii.crc_hqx(data[:-2],0xffff).to_bytes(2,'little')
                with self.assertRaises(ValueError,msg=(index,offset)):
                    decode('UWB3 '+data.hex().upper())
