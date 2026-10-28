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


async def verify(stm32, paced=False, kalman=False):
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        fixture, uart, bridge = work/'stm', work/'trace.uart', work/'bridge'
        subprocess.run(['python3', str(stm32/'tools/build_replay_host.py'), '--transport' if paced else '--fixture', '--sanitize', '--output', str(fixture)], check=True)
        subprocess.run([str(fixture)], env=dict(os.environ, MUL_REPLAY_TRACE=str(uart), **({"MUL_REPLAY_KALMAN":"1"} if kalman else {})), check=True)
        lines = uart.read_text().splitlines()
        chosen = lines
        assert len(chosen) <= 4095
        uart.write_text('\n'.join(chosen)+'\n')
        sources = ['tests/fixtures/bridge_e2e.c', 'main/bridge_protocol.c', 'main/app_ekf.c', 'main/ekf_service.c']
        sources += ['ThirdParty/mul_core/'+name+'.c' for name in ('mul_ekf', 'mul_ekf_stream', 'mul_pose', 'mul_schedule', 'mul_common', 'mul_telemetry')]
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-misleading-indentation', '-Wno-return-type', '-DBRIDGE_HOST_TEST', '-DBRIDGE_E2E_RECORDS=4096', '-DBRIDGE_E2E_STEP_NS=1000000' if paced else '-DBRIDGE_E2E_STEP_NS=10000000', '-Itests', '-Imain', '-IThirdParty/mul_core', *sources, '-lm', '-o', str(bridge)], cwd=ROOT, check=True)
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
        if paced:
            # The connection opens while cycle60 is in flight; the complete
            # generated file must report that boundary, not certify it away.
            assert not result['replay_complete'] and not result['mismatches'], result
            assert result['diagnostic_dropped']==0, result
            rows=list(map(json.loads,output.getvalue().splitlines()))
            opening=min(r['cycle'] for r in rows if r.get('type')=='tdoa_vector')
            interior=work/'interior.jsonl'
            interior.write_text('\n'.join(json.dumps(r) for r in rows if r.get('cycle')!=opening)+'\n')
            result=validate(interior,stm32,sanitize=True)
            assert result['replay_complete'] and result['passed'],result
            assert not result['transport_complete'] and result['transport_missing']>0,result
            assert result['reconstructible_pairs']==140 and result['vector_comparisons']==140,result
        else:
            assert result['passed'] and result['replay_complete'], result
            assert result['reconstructible_pairs']==45 and result['vector_comparisons']==45, result
        rows = [json.loads(line) for line in output.getvalue().splitlines()]
        assert len([r for r in rows if r.get('state')=='connected']) >= 2
        result.pop('results')
        print(json.dumps(result, indent=2))
        print('PASS: actual STM32 snapshot/UART -> actual ESP32 owner/framing/queues -> TCP reconnect -> existing compact collector -> exact production C replay')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--stm32', type=Path, default=ROOT.parent/'epuck-stm32')
    ap.add_argument('--paced', action='store_true', help='200s virtual capture, offline startup, AA bursts, 27ms UART and TCP reconnect')
    ap.add_argument('--kalman', action='store_true', help='Select persisted Kalman in STM fixture before simulated RF ON')
    args=ap.parse_args()
    asyncio.run(verify(args.stm32,args.paced,args.kalman))
