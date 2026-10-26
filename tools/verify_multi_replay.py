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
from uwb_logging import CompactLog

ROOT = Path(__file__).resolve().parents[1]


async def replay(path, experimental=False, planned=False, ar=False):
    lines = path.read_text().splitlines()
    decoded = [decode(line.strip()) for line in lines]
    if ar:
        expected=decoded
        assert 0<len(expected)<128 and any(r["type"]=="ar_range" for r in expected)
    elif planned:
        chosen = []
        for i, row in enumerate(decoded):
            if row['type'] == 'tdoa_vector' and row['schedule_profile'] == 4:
                if row['cycle'] == 10: # Final converged cycle, both policies/all sizes.
                    chosen.extend(lines[max(0, i-row['alr_count']-1):i+1])
                elif row['cycle'] in (1, 3): # Wrong-plan and missing-follower vectors.
                    chosen.append(lines[i])
        lines = chosen
        expected = [decode(line) for line in lines]
        assert 0 < len(expected) < 128
    elif experimental:
        # Keep two cycles per timing profile, including raw and clock observations,
        # inside the bounded bridge fixture; full adapter scenarios run upstream.
        chosen = []
        periods = set()
        for line, row in zip(lines, decoded):
            if row['uptime_ms'] <= 10200 or row['type'] == 'clock' and row['capture_valid'] is False or row['type'] == 'clock' and row['fit_applied'] and row['sample_period_ms'] not in periods:
                chosen.append(line)
                if row['type'] == 'clock' and row['fit_applied']:
                    periods.add(row['sample_period_ms'])
        # Last cycle of each profile contains fully converged vectors.
        for i, row in enumerate(decoded):
            if row['type'] == 'tdoa_vector' and row['cycle'] == 134:
                chosen.extend(lines[max(0, i-5):i+1])
        lines = chosen
        expected = [decode(line) for line in lines]
        assert 0 < len(expected) < 128
    else:
        expected = decoded
        assert expected and all(row['type'] == 'tdoa_vector' for row in expected)
    with tempfile.TemporaryDirectory() as work:
        executable = Path(work)/'bridge'
        fixture = Path(work)/'trace.uart'
        fixture.write_text('\n'.join(lines)+'\n')
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-Wno-misleading-indentation', '-Wno-return-type',
                        '-DBRIDGE_HOST_TEST', '-I'+str(ROOT/'tests'), '-I'+str(ROOT/'main'),
                        '-I'+str(ROOT/'ThirdParty/mul_core'), str(ROOT/'tests/fixtures/bridge_e2e.c'),
                        str(ROOT/'main/bridge_protocol.c'), str(ROOT/'ThirdParty/mul_core/mul_telemetry.c'),
                        '-o', str(executable)], check=True)
        process = subprocess.Popen([str(executable), str(fixture)], stdout=subprocess.PIPE, text=True)
        stop = asyncio.Event()
        output = io.StringIO()
        try:
            port = int(process.stdout.readline())
            task = asyncio.create_task(collect(f'127.0.0.1:{port}', output, stop, raw=planned or ar))
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
    if ar:
        for kind,keys in (
            ('ar_range',('cycle','initiator_node','responder_node','operation','raw_tof_dtu','tof_dtu','stamps','power','acquisition_uptime_ms','reported_uptime_ms','physical_uncertainty','stamp_clock_nodes','schedule_fingerprint')),
            ('ar_quality',('attempts','completed','aborted','rejected','reset_generation','last_reason','raw_dropped','result_dropped')),
            ('multi_rx',('multi_frame','direction','marker_dtu','rx','tx','schedule_fingerprint'))):
            actual=[r for r in rows if r.get('type')==kind];original=[r for r in expected if r['type']==kind]
            assert len(actual)>=len(original)>0,(kind,len(actual),len(original))
            for a,b in zip(actual,original):
                for key in keys:assert a[key]==b[key],(kind,key)
        compact_output=io.StringIO();compact=CompactLog(compact_output,'loopback-test')
        for row in rows:compact.accept(row)
        compact_rows=list(map(json.loads,compact_output.getvalue().splitlines()))
        ranges=[r for r in compact_rows if r['type']=='ar_range']
        assert len(ranges)>=3 and all(r['physical_uncertainty']=='UNKNOWN' and not r['calibrated'] for r in ranges)
        assert any(r['direction']=='tx' for r in expected if r['type']=='multi_rx')
        print(f'PASS: {len(expected)} actual STM32 AR range/quality/raw records through ESP UART/priority/TCP reconnect/debug+compact JSON; mixed clock stamps, TX/RX markers, acquired time and reset counters preserved')
        return
    if planned:
        raw_rows = [row for row in rows if row.get('type') == 'multi_rx']
        raw_expected = [row for row in expected if row['type'] == 'multi_rx']
        assert len(raw_rows) >= len(raw_expected) > 0
        for actual, original in zip(raw_rows, raw_expected):
            for key in ('multi_frame', 'multi_length', 'schedule_fingerprint', 'schedule_profile', 'fingerprint_scope', 'rx'):
                assert actual[key] == original[key], key
        compact_output = io.StringIO()
        compact = CompactLog(compact_output, 'loopback-test')
        for row in rows:
            compact.accept(row)
        rows = list(map(json.loads, compact_output.getvalue().splitlines()))
    vectors = [row for row in rows if row['type'] == 'tdoa_vector']
    original_vectors = [row for row in expected if row['type'] == 'tdoa_vector']
    assert len(vectors) >= len(original_vectors), (len(vectors), len(original_vectors))
    for actual, original in zip(vectors, original_vectors):
        for key in ('cycle', 'followers', 'alr_ids', 'schedule_revision', 'schedule_fingerprint', 'seen_mask', 'valid_mask', 'slot_ms', 'cycle_ms'):
            assert actual[key] == original[key], key
    assert any(row['valid_mask'] for row in vectors)
    if planned:
        assert {len(row['alr_ids']) for row in vectors} == {2, 4, 8}
        assert {row['cycle_ms'] for row in vectors} == {100, 200, 1000}
        assert all(row['schedule_profile'] == 4 and row['fingerprint_scope'] == 'ranging_plan' for row in vectors)
        assert any(any(f['reason_name'] == 'schedule_mismatch' for f in row['followers']) for row in vectors)
        assert any(any(f['reason_name'] == 'missing' for f in row['followers']) for row in vectors)
        print(f'PASS: {len(expected)} planned STM32 RF4 records through ESP UART/queues/TCP reconnect/compact JSON; full-plan identity, N2/4/8,100/200/1000ms and validity preserved')
    elif experimental:
        assert {row['cycle_ms'] for row in vectors} == {100, 200, 500, 1000, 2000}
        assert {row['slot_ms'] for row in vectors} == {2, 3, 5, 7, 17}
        assert all(row['experimental_timing'] and row['schedule_profile'] == 3 for row in vectors)
        clocks = [row for row in rows if row['type'] == 'clock']
        original_clocks = [row for row in expected if row['type'] == 'clock']
        assert len(clocks) >= len(original_clocks)
        assert any(row['capture_valid'] is False for row in clocks)
        for actual, original in zip(clocks, original_clocks):
            for key in ('reference_tx_dtu', 'local_rx_dtu', 'reference_sequence', 'sample_period_ms', 'capture_age_ms', 'capture_valid', 'fit_applied'):
                assert actual[key] == original[key], key
        print(f'PASS: {len(expected)} experimental STM32 clock/raw/vector records through ESP UART/queues/TCP reconnect/compact JSON; numerical2/3/5/7/17ms, cycles100..2000ms')
    else:
        assert {len(row['alr_ids']) for row in vectors} == {2, 3, 4, 8}
        assert any(any(f['reason_name'] == 'missing' for f in row['followers']) for row in vectors)
        print(f'PASS: {len(expected)} STM32 production vectors -> ESP UART/parser/priority queue -> TCP reconnect -> compact JSONL; N2/3/4/8, partial validity and identities preserved')



if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--experimental', action='store_true')
    mode.add_argument('--planned', action='store_true')
    mode.add_argument('--ar', action='store_true')
    args = parser.parse_args()
    asyncio.run(replay(args.trace.resolve(), args.experimental, args.planned, args.ar))
