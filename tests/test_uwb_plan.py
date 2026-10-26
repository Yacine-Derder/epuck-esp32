import binascii
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.uwb_collect import decode
from tools.uwb_logging import CompactLog

ROOT = Path(__file__).parents[1]

def wire(b):
    b = bytearray(b)
    b[-2:] = binascii.crc_hqx(b[:-2], 0xffff).to_bytes(2, 'little')
    return 'UWB3 '+b.hex().upper()

class Planned(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory()
        core = ROOT/'ThirdParty/mul_core'
        executable = Path(cls.work.name)/'emit'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I'+str(core),
                        str(core/'tests/test_plan_telemetry.c'), str(core/'mul_telemetry.c'), '-o', str(executable)], check=True)
        cls.lines = subprocess.check_output([str(executable), 'emit'], text=True).splitlines()

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def test_real_c_codec_both_types_profiles_and_compact_scope(self):
        stream = io.StringIO()
        compact = CompactLog(stream, 'test')
        for line in self.lines:
            row = decode(line)
            self.assertEqual(row['schedule_profile'], 4)
            self.assertEqual(row['fingerprint_scope'], 'ranging_plan')
            self.assertEqual(row['schedule_fingerprint'], '34567890')
            self.assertEqual(row['slot_ms'], 3)
            if row['type'] == 'tdoa_vector':
                self.assertEqual(row['cycle_ms'], 200)
                compact.accept(dict(row, kind='telemetry', host_received_utc='2026-10-26T15:00:00Z'))
            else:
                self.assertEqual(row['multi_length'], len(bytes.fromhex(row['multi_frame'])))
        vectors = [r for r in map(json.loads, stream.getvalue().splitlines()) if r['type']=='tdoa_vector']
        self.assertEqual(len(vectors), 14)
        self.assertTrue(all(r['schedule_profile']==4 and r['fingerprint_scope']=='ranging_plan' for r in vectors))

    def test_crc_valid_type_marker_and_full_raw_binding_rejection(self):
        for line in self.lines[:4]:
            original = bytearray.fromhex(line[5:])
            for offset, value in ((11, 3), (3, 8 if original[3]==11 else 9), (149, 1)):
                b = original.copy(); b[offset] = value
                with self.assertRaises(ValueError): decode(wire(b))
            if original[3] == 12:
                n = original[137]
                for offset in (40+(84 if n==90 else 34), 140):
                    b = original.copy(); b[offset] ^= 1
                    with self.assertRaises(ValueError): decode(wire(b))
