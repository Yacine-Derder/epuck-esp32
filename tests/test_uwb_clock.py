import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_uwb_collect import collector
from test_uwb_multi import wire
from tools.uwb_logging import CompactLog

ROOT = Path(__file__).parents[1]


class ClockTelemetry(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory()
        executable = Path(cls.work.name)/'clock'
        core = ROOT/'ThirdParty/mul_core'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I'+str(core),
                        str(ROOT/'tests/fixtures/clock_capture.c'), str(core/'mul_telemetry.c'),
                        '-o', str(executable)], check=True)
        cls.line = subprocess.check_output([str(executable)], text=True).strip()

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def test_production_c_diagnostic_preserves_domains_and_fallback_in_compact_log(self):
        row = collector.decode(self.line)
        self.assertEqual(row['type'], 'clock')
        self.assertEqual(row['clock_record_version'], 2)
        self.assertEqual(row['sample_period_ms'], 200)
        self.assertEqual(row['capture_age_ms'], 16)
        self.assertFalse(row['capture_valid'])
        self.assertEqual(row['reference_tx_dtu'], 0xffffffffff)
        self.assertEqual(row['local_rx_dtu'], 1)
        self.assertEqual(row['reference_sequence'], 0xffffffff)
        self.assertEqual((row['applied_ppb'], row['fitted_ppb']), (-15, -38))
        self.assertIsNone(row['fit_sigma_ppb'])
        self.assertTrue(row['fit_ready'])
        self.assertFalse(row['fit_applied'])
        self.assertEqual(row['fallback'], 'changing_rate')
        stream = io.StringIO()
        log = CompactLog(stream, 'test')
        log.accept(dict(row, kind='telemetry', host_received_utc='2026-10-26T10:00:00Z'))
        compact = json.loads(stream.getvalue().splitlines()[-1])
        for key in ('reference_tx_dtu', 'local_rx_dtu', 'reference_sequence', 'fitted_ppb', 'applied_ppb', 'fallback', 'sample_period_ms', 'capture_age_ms', 'capture_valid'):
            self.assertEqual(compact[key], row[key])
        self.assertEqual(compact['physical_uncertainty'], 'unknown')

    def test_crc_valid_contradictory_and_reserved_records_rejected(self):
        data = bytearray.fromhex(self.line[5:])
        for offset, value in ((79, 2), (80, 7), (81, 25), (83, 4), (85, 1), (86, 4), (88, 1)):
            bad = data.copy()
            bad[offset] = value
            with self.assertRaises(ValueError):
                collector.decode(wire(bad))

    def test_old_record_marks_age_and_period_unknown(self):
        data = bytearray.fromhex(self.line[5:])
        data[88:92] = bytes(4)
        data[32:36] = (0xffffffff).to_bytes(4, 'little')  # No v1 sample-period semantics.
        row = collector.decode(wire(data))
        self.assertEqual(row['clock_record_version'], 1)
        self.assertIsNone(row['capture_age_ms'])
        self.assertIsNone(row['capture_valid'])
        self.assertIsNone(row['sample_period_ms'])

    def test_new_capture_age_and_period_boundaries(self):
        data = bytearray.fromhex(self.line[5:])
        for age in (0, 15, 16, 65535):
            data[90:92] = age.to_bytes(2, 'little')
            row = collector.decode(wire(data))
            self.assertEqual(row['capture_valid'], age <= 15)
        for period in (0, 100, 2000):
            data[32:36] = period.to_bytes(4, 'little')
            self.assertEqual(collector.decode(wire(data))['sample_period_ms'], period or 1000)
        for period in (1, 9, 2001, 0xffffffff):
            data[32:36] = period.to_bytes(4, 'little')
            with self.assertRaises(ValueError):
                collector.decode(wire(data))
