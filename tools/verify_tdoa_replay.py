#!/usr/bin/env python3
"""Actual STM fixture -> ESP owner -> loopback TCP -> collector -> production C.

Loopback-only: no device addresses, ports or control/configuration accepted.
"""
import argparse
import asyncio
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
from uwb_collect import collect, decode
from validate_tdoa_replay import validate

ROOT = Path(__file__).resolve().parents[1]


async def verify(stm32):
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        fixture, uart, bridge = work/'stm', work/'trace.uart', work/'bridge'
        subprocess.run(['python3', str(stm32/'tools/build_replay_host.py'), '--fixture', '--sanitize', '--output', str(fixture)], check=True)
        subprocess.run([str(fixture)], env=dict(os.environ, MUL_REPLAY_TRACE=str(uart)), check=True)
        lines = uart.read_text().splitlines()
        chosen = lines
        assert len(chosen) <= 511
        uart.write_text('\n'.join(chosen)+'\n')
        sources = ['tests/fixtures/bridge_e2e.c', 'main/bridge_protocol.c', 'main/app_ekf.c', 'main/ekf_service.c']
        sources += ['ThirdParty/mul_core/'+name+'.c' for name in ('mul_ekf', 'mul_ekf_stream', 'mul_pose', 'mul_schedule', 'mul_common', 'mul_telemetry')]
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-misleading-indentation', '-Wno-return-type', '-DBRIDGE_HOST_TEST', '-DBRIDGE_E2E_RECORDS=512', '-Itests', '-Imain', '-IThirdParty/mul_core', *sources, '-lm', '-o', str(bridge)], cwd=ROOT, check=True)
        process = subprocess.Popen([str(bridge), str(uart)], stdout=subprocess.PIPE, text=True)
        stop, output = asyncio.Event(), io.StringIO()
        port = int(process.stdout.readline())
        task = asyncio.create_task(collect(f'127.0.0.1:{port}', output, stop, health_seconds=0))
        try:
            async def finished():
                while process.poll() is None:
                    await asyncio.sleep(.03)
            await asyncio.wait_for(finished(), 120)
            await asyncio.sleep(.1)
            stop.set()
            await asyncio.wait_for(task, 3)
            assert process.returncode == 0
        finally:
            stop.set()
            if not task.done():
                await asyncio.wait_for(task, 3)
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=3)
        compact = work/'recording.jsonl'
        compact.write_text(output.getvalue())
        result = validate(compact, stm32, sanitize=True)
        assert result['passed'], result
        assert result['replay_complete'], result
        assert result['reconstructible_pairs']==45, result
        assert result['vector_comparisons']==45, result
        rows = [json.loads(line) for line in output.getvalue().splitlines()]
        assert len([r for r in rows if r.get('state')=='connected']) >= 2
        result.pop('results')
        print(json.dumps(result, indent=2))
        print('PASS: actual STM32 snapshot/UART -> actual ESP32 owner/framing/queues -> TCP reconnect -> existing compact collector -> exact production C replay')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--stm32', type=Path, default=ROOT.parent/'epuck-stm32')
    asyncio.run(verify(ap.parse_args().stm32))
