#!/usr/bin/env python3
"""Validate compact JSONL against the recorded build's production C baseline.

No radios, serial ports or device endpoints. Builds a temporary native oracle
from the explicitly selected STM32 checkout (default: sibling epuck-stm32).
"""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
from uwb_replay import snapshot, digest
from uwb_replay_audit import audit

ROOT = Path(__file__).resolve().parents[1]


def validate(path, stm32, sanitize=False):
    manifest = json.loads((Path(stm32)/'ThirdParty/mul_core/UPSTREAM.json').read_text())
    if manifest['revision'] != '506913dc676d0a3c0d43c48977ef11be2085aa34':
        raise ValueError('schema 1 requires its recorded production core revision')
    records, invalid, losses, incomplete, vectors = [], [], [], [], {}
    identities = set()
    for line_no, line in enumerate(Path(path).read_text().splitlines(), 1):
        try:
            row = json.loads(line)
            if row.get('type') == 'tdoa_replay':
                blob = bytes.fromhex(row['blob'])
                decoded = snapshot(blob, row['node'])
                if f'{digest(blob):08X}' != row['snapshot_digest']:
                    raise ValueError('snapshot digest')
                for key, value in decoded.items():
                    if row.get(key) != value:
                        raise ValueError(f'JSON/blob disagreement: {key}')
                identity = (row['s'], row['diagnostic_id'])
                if identity in identities:
                    raise ValueError('duplicate diagnostic ID')
                identities.add(identity)
                records.append((row, blob))
            elif row.get('type') == 'tdoa_vector':
                vectors[(row['s'], row['cycle'])] = row
            elif row.get('type') in ('gap', 'transport'):
                losses.append(row)
            elif row.get('type') == 'tdoa_replay_incomplete':
                incomplete.append(row)
            elif row.get('type') == 'error':
                invalid.append(dict(line=line_no, reason=row.get('reason', 'collector_error')))
        except (ValueError, KeyError, TypeError) as error:
            invalid.append(dict(line=line_no, reason=str(error)))
    with tempfile.TemporaryDirectory() as work:
        builder = Path(stm32) / 'tools/build_replay_host.py'
        spec = importlib.util.spec_from_file_location('stm_replay_build', builder)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        executable = Path(work) / 'oracle'
        module.build(executable, sanitize=sanitize)
        process = subprocess.run([str(executable)], input=b''.join(b for _, b in records), stdout=subprocess.PIPE, check=True)
        outcomes = [json.loads(line) for line in process.stdout.splitlines()]
    if len(outcomes) != len(records):
        raise ValueError('native oracle record count')
    mismatch, comparisons, missing_vectors = [], 0, 0
    for (row, _), actual in zip(records, outcomes):
        actual['s'] = row['s']
        if not actual.get('match') or actual.get('intermediates_match') is False:
            mismatch.append(actual)
        vector = vectors.get((row['s'], row['cycle']))
        if vector is None:
            missing_vectors += 1
            continue
        follower = next((f for f in vector['followers'] if f['follower_node'] == row['follower_node']), None)
        if follower is None:
            mismatch.append(dict(cycle=row['cycle'], reason='missing_vector_follower'))
            continue
        comparisons += 1
        expected = row['expected']
        for key, vector_key in [('tdoa_dtu', 'tdoa_dtu'), ('range_diff_mm', 'range_diff_mm'), ('status', 'status'), ('reason', 'reason')]:
            value = expected[key]
            if key in ('tdoa_dtu', 'range_diff_mm') and expected['status'] != 4:
                value = None
            if follower.get(vector_key) != value:
                mismatch.append(dict(cycle=row['cycle'], field=key, expected=value, vector=follower.get(vector_key)))
    current_records, accounting = audit(records, vectors, losses, incomplete)
    diagnostic_dropped = accounting['diagnostic_dropped']
    id_gaps = accounting['diagnostic_id_gaps']
    transport_missing = sum(r.get('missing', 0) for r in losses)
    bridge_changes = accounting['bridge_counter_samples']['observed_increments']
    baseline_passed = bool(records) and not invalid and not mismatch
    transport_complete = not transport_missing and not any(bridge_changes.values()) and not invalid
    captured_pairs = {(r['s'], r['cycle'], r['follower_node']) for r, _ in records}
    missing_fields = [dict(s=sid, cycle=cycle, follower_node=f['follower_node'], missing='tdoa_replay')
                      for (sid, cycle), vector in vectors.items() for f in vector['followers']
                      if (sid, cycle, f['follower_node']) not in captured_pairs]
    first_last = {}
    for (sid, cycle) in vectors:
        first_last.setdefault(sid, [cycle, cycle])[1] = cycle
    for missing in missing_fields:
        first, last = first_last[missing['s']]
        missing['boundary_assessment'] = 'initial_boundary_possible' if missing['cycle']==first else 'final_boundary_possible' if missing['cycle']==last else 'interior_or_unresolved'
    total = len({(r['s'], r['cycle']) for r, _ in records} | set(vectors))
    return dict(total_cycles=total, reconstructible_cycles=len({(r['s'], r['cycle']) for r, _ in records}),
                reconstructible_pairs=len(records), exact_dtu_matches=sum(r.get('dtu_match', False) for r in outcomes),
                exact_mm_matches=sum(r.get('mm_match', False) for r in outcomes),
                intermediate_matches=sum(r.get('intermediates_captured', False) and r.get('intermediates_match', False) for r in outcomes),
                status_reason_matches=sum(r.get('status_match', False) for r in outcomes),
                vector_comparisons=comparisons, missing_vector_records=missing_vectors,
                missing_fields=missing_fields, invalid_records=invalid, incomplete_snapshots=incomplete, transport_reports=len(losses),
                transport_missing=sum(r.get('missing', 0) for r in losses),
                **accounting,
                mismatches=mismatch, baseline_passed=baseline_passed, transport_complete=transport_complete,
                replay_complete=baseline_passed and not missing_fields and not incomplete and not id_gaps and not diagnostic_dropped and not accounting['current_missing_vector_records'],
                passed=baseline_passed and not incomplete,
                scope='exact logged receiver baseline; received follower relation fixed',
                production_source=str(Path(stm32).resolve()),
                results=outcomes)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('recording', type=Path)
    ap.add_argument('--stm32', type=Path, default=ROOT.parent/'epuck-stm32')
    ap.add_argument('--sanitize', action='store_true')
    ap.add_argument('--details', action='store_true', help='Include per-pair native replay and intermediates')
    args = ap.parse_args()
    result = validate(args.recording, args.stm32, args.sanitize)
    if not args.details:
        result.pop('results')
    print(json.dumps(result, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
