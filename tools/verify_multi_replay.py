#!/usr/bin/env python3
"""Replay a STM32 host-test UART trace through the real ESP owner/TCP/collector.

No device or network endpoint is accepted: sockets bind only to host loopback.
Generate input with MUL_MULTI_TRACE=/tmp/multi.uart sh tests/runtime_config/run.sh
inside epuck-stm32, then run this script with /tmp/multi.uart.
"""
import argparse
import asyncio
import io
import json
from pathlib import Path
import subprocess
import tempfile
from uwb_collect import collect, decode

ROOT = Path(__file__).resolve().parents[1]


async def replay(path):
    expected = [decode(line.strip()) for line in path.read_text().splitlines()]
    assert expected and all(row['type'] == 'tdoa_vector' for row in expected)
    with tempfile.TemporaryDirectory() as work:
        executable = Path(work)/'bridge'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-Wno-misleading-indentation', '-Wno-return-type',
                        '-DBRIDGE_HOST_TEST', '-I'+str(ROOT/'tests'), '-I'+str(ROOT/'main'),
                        '-I'+str(ROOT/'ThirdParty/mul_core'), str(ROOT/'tests/fixtures/bridge_e2e.c'),
                        str(ROOT/'main/bridge_protocol.c'), str(ROOT/'ThirdParty/mul_core/mul_telemetry.c'),
                        '-o', str(executable)], check=True)
        process = subprocess.Popen([str(executable), str(path)], stdout=subprocess.PIPE, text=True)
        stop = asyncio.Event()
        output = io.StringIO()
        try:
            port = int(process.stdout.readline())
            task = asyncio.create_task(collect(f'127.0.0.1:{port}', output, stop))
            async def finished():
                while process.poll() is None:
                    await asyncio.sleep(.02)
            await asyncio.wait_for(finished(), 28)
            await asyncio.sleep(.1)
            stop.set()
            await asyncio.wait_for(task, 3)
            assert process.returncode == 0
        finally:
            stop.set()
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=3)
    rows = list(map(json.loads, output.getvalue().splitlines()))
    vectors = [row for row in rows if row['type'] == 'tdoa_vector']
    assert len(vectors) >= len(expected), (len(vectors), len(expected))
    for actual, original in zip(vectors, expected):
        for key in ('cycle', 'followers', 'alr_ids', 'schedule_revision', 'schedule_fingerprint', 'seen_mask', 'valid_mask'):
            assert actual[key] == original[key], key
    assert {len(row['alr_ids']) for row in vectors} == {2, 3, 4, 8}
    assert any(row['valid_mask'] for row in vectors)
    assert any(any(f['reason_name'] == 'missing' for f in row['followers']) for row in vectors)
    print(f'PASS: {len(expected)} STM32 production vectors -> ESP UART/parser/priority queue -> TCP reconnect -> compact JSONL; N2/3/4/8, partial validity and identities preserved')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    asyncio.run(replay(parser.parse_args().trace.resolve()))
