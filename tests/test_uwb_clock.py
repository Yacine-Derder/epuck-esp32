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
        for key in ('reference_tx_dtu', 'local_rx_dtu', 'reference_sequence', 'fitted_ppb', 'applied_ppb', 'fallback'):
            self.assertEqual(compact[key], row[key])
        self.assertEqual(compact['physical_uncertainty'], 'unknown')

    def test_crc_valid_contradictory_and_reserved_records_rejected(self):
        data = bytearray.fromhex(self.line[5:])
        for offset, value in ((79, 2), (80, 7), (81, 25), (83, 4), (85, 1), (86, 4), (88, 1)):
            bad = data.copy()
            bad[offset] = value
            with self.assertRaises(ValueError):
                collector.decode(wire(bad))
