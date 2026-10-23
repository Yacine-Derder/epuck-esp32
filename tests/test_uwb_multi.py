"""Real bounded C producer -> strict parser -> compact self-describing vectors."""
import binascii
import io
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from test_uwb_collect import collector
from tools.uwb_logging import CompactLog

ROOT = Path(__file__).parents[1]


def wire(data):
    data = bytearray(data)
    data[-2:] = binascii.crc_hqx(data[:-2], 0xffff).to_bytes(2, 'little')
    return 'UWB3 '+data.hex().upper()


class Multi(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory()
        executable = Path(cls.work.name)/'produce'
        core = ROOT/'ThirdParty/mul_core'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I'+str(core),
                        str(core/'tests/test_multi_telemetry.c'), str(core/'mul_telemetry.c'),
                        '-o', str(executable)], check=True)
        cls.lines = subprocess.check_output([str(executable), 'emit'], text=True).splitlines()

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def test_all_sizes_partial_validity_no_rate_and_compact_json(self):
        stream = io.StringIO()
        log = CompactLog(stream, 'test:3333')
        for index, line in enumerate(self.lines):
            value = collector.decode(line)
            self.assertEqual(value['alr_count'], (2, 3, 4, 8)[index//2])
            self.assertEqual(value['cycle'], 0xffffffff)
            self.assertEqual(len(value['followers']), value['alr_count']-1)
            for follower in value['followers']:
                if follower['valid']:
                    self.assertEqual(follower['reason_name'], 'ok')
                    self.assertIsInstance(follower['range_diff_mm'], int)
                else:
                    self.assertIsNone(follower['range_diff_mm'])
                    self.assertIsNone(follower['tdoa_dtu'])
            log.accept(dict(value, kind='telemetry', host_received_utc='2026-10-23T17:00:00Z'))
        vectors = [row for row in map(json.loads, stream.getvalue().splitlines()) if row['type']=='tdoa_vector']
        self.assertEqual(len(vectors), 8)
        for row in vectors:
            self.assertEqual(row['reference_node'], row['alr_ids'][0])
            self.assertEqual(len(row['followers']), len(row['alr_ids'])-1)
            self.assertEqual(row['schedule_revision'], 9)
            self.assertEqual(row['slot_ms'], 10)
        self.assertLess(vectors[1]['followers'][0]['range_diff_mm'], 0)
        self.assertEqual(vectors[-1]['followers'][1]['reason_name'], 'missing')

    def test_crc_valid_malformed_envelopes_and_entries_fail_closed(self):
        original = bytearray.fromhex(self.lines[-1][5:])
        for offset, value in ((61, 9), (63, original[63]|1), (65, original[64]),
                              (80, 0xc4), (81, 10), (148, 0), (149, 1)):
            bad = original.copy()
            bad[offset] = value
            with self.assertRaises(ValueError, msg=f'offset={offset}'):
                collector.decode(wire(bad))
        for line in (self.lines[-1][:-2], self.lines[-1]+'00'):
            with self.assertRaises(ValueError):
                collector.decode(line)

    def test_full_multi_raw_frame_and_explicit_legacy_separation(self):
        data = bytearray.fromhex(self.lines[-1][5:])
        data[3] = 9
        data[40:150] = bytes(110)
        data[40:130] = bytes(range(90))
        data[130:135] = bytes([255])*5
        data[137] = 90
        data[138:140] = (10).to_bytes(2, 'little')
        data[144:148] = (9).to_bytes(4, 'little')
        data[148] = 1
        raw = collector.decode(wire(data))
        self.assertEqual(raw['type'], 'multi_rx')
        self.assertEqual(raw['multi_length'], 90)
        self.assertEqual(raw['rx'], 'FFFFFFFFFF')
        data[3] = 6
        with self.assertRaises(ValueError):
            collector.decode(wire(data))
