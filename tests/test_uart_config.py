import subprocess
import tempfile
from pathlib import Path
import unittest
from tools.uart_link_budget import budget
ROOT=Path(__file__).parents[1]

class Uart(unittest.TestCase):
    def test_default_and_legacy_build_definitions(self):
        for baud in (115200,921600):
            with tempfile.TemporaryDirectory() as work:
                source=Path(work)/'baud.c';source.write_text('#include "bridge_config.h"\n_Static_assert(BRIDGE_UART_BAUD=='+str(baud)+',"baud");')
                subprocess.run(['cc','-std=c11','-Werror','-DBRIDGE_UART_BAUD='+str(baud),'-I'+str(ROOT/'main'),'-c',str(source),'-o',str(Path(work)/'baud.o')],check=True)
        self.assertIn('.baud_rate = BRIDGE_UART_BAUD',(ROOT/'main/main.c').read_text())
        self.assertIn('.source_clk = UART_SCLK_XTAL',(ROOT/'main/main.c').read_text())

    def test_nominal_clock_dividers_and_utilization(self):
        fast=budget();slow=budget(115200)
        self.assertEqual(fast['stm32_brr'],46);self.assertEqual(fast['esp32_divider_sixteenths'],694)
        self.assertAlmostEqual(fast['stm32_nominal_actual_baud'],913043.4782608695)
        self.assertAlmostEqual(fast['esp32_nominal_actual_baud'],922190.2017291066)
        self.assertLess(fast['requested_capacity_utilization_percent'],14)
        self.assertGreater(slow['requested_capacity_utilization_percent'],100)
        self.assertEqual(slow['stm32_brr'],364)

    def test_new_loss_counters_survive_compact_logging(self):
        from tools.uwb_logging import CompactLog
        import io,json
        out=io.StringIO();log=CompactLog(out,'synthetic')
        log.accept(dict(kind='bridge',host_received_utc='2026-11-06T10:00:00Z',line='UWBSTAT version=1 dropped=0 corrupt=0 uart_errors=0 ekf_dropped=0 sensor_dropped=2 control_full=1 rx_bytes=1000 tcp_bytes=900 baud=921600'))
        rows=list(map(json.loads,out.getvalue().splitlines()));row=next(r for r in rows if r['type']=='transport')
        self.assertEqual(row['sensor_dropped'],2);self.assertEqual(row['control_full'],1)
