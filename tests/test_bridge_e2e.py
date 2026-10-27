"""C UART codec -> real ESP bridge owner -> TCP -> actual Python collector."""
import asyncio
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_uwb_logging import ROOT, WireFixture, collector


class BridgeEndToEnd(WireFixture, unittest.IsolatedAsyncioTestCase):
    async def test_bridge_to_compact_and_raw_collector_with_reconnect(self):
        with tempfile.TemporaryDirectory() as work:
            work = Path(work)
            executable = work / 'bridge'
            subprocess.run([
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-Wno-misleading-indentation', '-Wno-return-type',
                '-DBRIDGE_HOST_TEST', '-I'+str(ROOT/'tests'), '-I'+str(ROOT/'main'),
                '-I'+str(ROOT/'ThirdParty/mul_core'),
                str(ROOT/'tests/fixtures/bridge_e2e.c'),
                str(ROOT/'main/bridge_protocol.c'),
                        str(ROOT/'main/app_ekf.c'), str(ROOT/'main/ekf_service.c'), str(ROOT/'ThirdParty/mul_core/mul_ekf.c'), str(ROOT/'ThirdParty/mul_core/mul_ekf_stream.c'), str(ROOT/'ThirdParty/mul_core/mul_pose.c'), str(ROOT/'ThirdParty/mul_core/mul_schedule.c'), str(ROOT/'ThirdParty/mul_core/mul_common.c'), '-lm',
                str(ROOT/'ThirdParty/mul_core/mul_telemetry.c'), '-o', str(executable),
            ], check=True)
            payload = work / 'uart.txt'
            # Actual C codec emits every validity status; sequences stay ordered.
            payload.write_text(''.join(self.wire(i % 5, i)+'\n' for i in range(20)))
            for raw in (False, True):
                process = subprocess.Popen([str(executable), str(payload)], stdout=subprocess.PIPE, text=True)
                try:
                    port = int(process.stdout.readline())
                    output = io.StringIO()
                    stop = asyncio.Event()
                    task = asyncio.create_task(collector.collect(f'127.0.0.1:{port}', output, stop, raw=raw))
                    try:
                        async def finished():
                            while process.poll() is None:
                                await asyncio.sleep(.02)
                        await asyncio.wait_for(finished(), 10)
                        await asyncio.sleep(.05)
                    finally:
                        stop.set()
                        await asyncio.wait_for(task, 2)
                    self.assertEqual(process.returncode, 0)
                    rows = [json.loads(line) for line in output.getvalue().splitlines()]
                    measurements = [r for r in rows if r.get('type') == 'tdoa']
                    # Reconnected duplicate sequences are intentionally excluded
                    # from compact observations; raw retains them for diagnostics.
                    self.assertEqual(len(measurements), 40 if raw else 20)
                    for i, r in enumerate(measurements):
                        status = (i % 20) % 5
                        self.assertEqual(r.get('status', 4), status)
                        self.assertEqual(r['range_diff_mm' if raw else 'dd_mm'], -131 if status == 4 else None)
                    connections = [r for r in rows if r.get('kind') == 'connected' or r.get('state') == 'connected']
                    self.assertEqual(len(connections), 2)
                finally:
                    if process.poll() is None:
                        process.terminate()
                    process.wait(timeout=2)
                    process.stdout.close()

    def test_shared_codec_matches_stm32_and_canonical_lps(self):
        for name in ('mul_telemetry.c', 'mul_telemetry.h'):
            expected = (ROOT/'ThirdParty/mul_core'/name).read_bytes()
            siblings = (ROOT.parent/'epuck-stm32/ThirdParty/mul_core'/name,
                        ROOT.parent/'lps-node-firmware/portable/mul_core'/name)
            for sibling in siblings:
                if sibling.exists():
                    self.assertEqual(expected, sibling.read_bytes(), str(sibling))
