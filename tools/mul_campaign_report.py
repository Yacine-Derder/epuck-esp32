#!/usr/bin/env python3
"""Offline MUL capture audit. No truth/clock alignment is inferred.

Accepts existing compact or debug JSONL. Evaluation truth uses exact exported
sample_key values and value_m; it must be prepared independently with justified
acquisition-time alignment. No host-time matching, calibration fitting or network.
"""
import argparse
import hashlib
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import statistics


def stats(values, errors=False):
    result = dict(count=len(values), mean=None, std=None, rms=None)
    if values:
        result.update(mean=statistics.mean(values), std=statistics.pstdev(values),
                      rms=math.sqrt(statistics.mean(x*x for x in values)))
    if errors:
        result.update(bias=result['mean'], rmse=result['rms'])
    return result


def samples(row, domain):
    kind = row.get('type')
    base = [domain, row.get('node'), row.get('epoch'), row.get('schedule_fingerprint')]
    if kind == 'tdoa_vector':
        for follower in row['followers']:
            key = base+[kind, row['cycle'], row.get('uptime_ms', row.get('u_ms')),
                        row['reference_node'], follower['follower_node']]
            yield key, follower.get('range_diff_mm'), follower.get('valid', False), follower.get('reason_name', follower.get('reason'))
    elif kind == 'tdoa':
        key = base+[kind, row.get('reference_sequence', row.get('ref')),
                    row.get('follower_sequence', row.get('fol')), row.get('uptime_ms', row.get('u_ms'))]
        value = row.get('range_diff_mm', row.get('dd_mm'))
        yield key, value, row.get('valid', value is not None and row.get('status', 4) == 4), row.get('status_name', row.get('status', 4))
    elif kind == 'ar_range':
        key = base+[kind, row['cycle'], row['operation'], row['initiator_node'], row['responder_node'], row['acquisition_uptime_ms']]
        yield key, row.get('range_mm'), row.get('valid', False), None


def analyze(paths, truth=None):
    summary = dict(schema=1, physical_uncertainty='UNKNOWN', alignment='exact key only; never host-time interpolation',
                   files=[], samples={}, clock={}, quality={}, truth_unmatched=0, raw_keys=[])
    measurement = defaultdict(lambda: dict(total=0, valid=0, reasons=Counter(), values=[], errors=[]))
    clock = defaultdict(lambda: defaultdict(list))
    sessions, seen, matched = {}, set(), set()
    last_valid, pair_intervals, pair_ages, pair_discontinuities = {}, defaultdict(list), defaultdict(list), Counter()
    spans = defaultdict(list)
    counters, deltas, resets = {}, Counter(), Counter()
    for path in paths:
        digest = hashlib.sha256()
        with path.open('rb') as source:
            for block in iter(lambda: source.read(1048576), b''):
                digest.update(block)
        domain = 'sha256:'+digest.hexdigest(); connection = 0
        summary['files'].append(str(path))
        with path.open() as stream:
            for line in stream:
                row = json.loads(line)
                if row.get('type') == 'session':
                    sessions[row['s']] = row
                if row.get('kind') == 'connected':
                    connection += 1
                if row.get('sequence_status') in ('duplicate', 'old'):
                    continue
                if row.get('s') in sessions:
                    row = sessions[row['s']] | row
                identity = row.get('s') or f'{domain}#{connection}'
                for key, mm, valid, reason in samples(row, identity):
                    sample_key = json.dumps(key, separators=(',', ':'))
                    if sample_key in seen:
                        continue
                    seen.add(sample_key)
                    kind = row['type'];m = measurement[kind];m['total'] += 1
                    usable = valid and isinstance(mm, (int, float)) and math.isfinite(mm)
                    value = mm/1000 if usable else None
                    if usable:
                        m['valid'] += 1;m['values'].append(value)
                    else:
                        m['reasons'][str(reason)] += 1
                    expected = truth.get(sample_key) if truth else None
                    if expected is not None and usable:
                        m['errors'].append(value-expected);matched.add(sample_key)
                    tick = row.get('acquisition_uptime_ms') if kind == 'ar_range' else row.get('uptime_ms', row.get('u_ms'))
                    pair = json.dumps(key[:5]+(key[-2:] if kind == 'tdoa_vector' else
                        [row.get('initiator_node'), row.get('responder_node')] if kind == 'ar_range' else
                        [row.get('reference_node'), row.get('follower_node')]), separators=(',', ':'))
                    if isinstance(tick, int) and 0 <= tick <= 0xffffffff:
                        previous = last_valid.get(pair)
                        if previous is not None:
                            elapsed = (tick-previous) & 0xffffffff
                            if elapsed < 0x80000000:
                                pair_ages[pair].append(elapsed)
                                if usable:
                                    pair_intervals[pair].append(elapsed)
                            else:
                                pair_discontinuities[pair] += 1
                        if usable:
                            last_valid[pair] = tick
                    summary['raw_keys'].append(dict(sample_key=sample_key, type=kind, value_m=value, valid=bool(usable)))
                if row.get('type') == 'ar_range' and row.get('valid') and len(row.get('stamps', [])) == 6:
                    nodes = row.get('stamp_clock_nodes', [])
                    for a, b, label in ((0, 4, 'initiator_p1tx_to_p3tx_ms'), (1, 5, 'responder_p1rx_to_p3rx_ms')):
                        if len(nodes) == 6 and nodes[a] == nodes[b]:
                            delta = (int(row['stamps'][b], 16)-int(row['stamps'][a], 16)) & ((1 << 40)-1)
                            if 0 < delta < (1 << 39):
                                spans[label].append(delta/63897600)
                if row.get('type') == 'clock':
                    group = json.dumps([identity, row.get('node'), row.get('epoch'), row.get('mapping_epoch'),
                                        row['clock_estimator'], row['clock_window']])
                    for name in ('fitted_ppb', 'applied_ppb', 'residual_rms_dtu', 'fit_sigma_ppb', 'mapping_age_ms', 'capture_age_ms'):
                        value = row.get(name)
                        if isinstance(value, (int, float)) and math.isfinite(value):
                            clock[group][name].append(value)
                if row.get('type') == 'ar_quality':
                    marker = (identity, row.get('node'), row.get('epoch'), row['reset_generation'])
                    for name in ('attempts', 'completed', 'aborted', 'rejected', 'math_invalid', 'result_dropped', 'raw_dropped', 'late', 'radio_errors', 'timeouts'):
                        key = marker+(name,);value = row[name];previous = counters.get(key);counters[key] = value
                        if previous is None:
                            continue
                        delta = (value-previous) & 0xffffffff
                        if delta < 0x80000000:
                            deltas[name] += delta
                        else:
                            resets[name] += 1
    for kind, m in measurement.items():
        summary['samples'][kind] = dict(total=m['total'], valid=m['valid'],
            availability=m['valid']/m['total'], invalid_reasons=dict(m['reasons']),
            values_m=stats(m['values']), aligned_errors_m=stats(m['errors'], errors=True))
    summary['clock'] = {key: {name: stats(values) for name, values in names.items()} for key, names in clock.items()}
    attempts = deltas.get('attempts', 0)
    summary['quality'] = dict(observed_counter_deltas=dict(deltas), counter_discontinuities=dict(resets),
                             completed_per_attempt=deltas.get('completed', 0)/attempts if attempts else None,
                             scope='within-session/reset generation deltas only; first snapshot excluded')
    summary['freshness'] = {pair: dict(valid_interval_ms=stats(pair_intervals[pair]),
        age_since_previous_valid_at_observed_sample_ms=stats(pair_ages[pair]),
        counter_discontinuities=pair_discontinuities[pair]) for pair in last_valid}
    summary['freshness_scope'] = 'receiver acquisition uptime within domain, modulo32 half-period; excludes unobserved gaps and host latency'
    summary['twr_same_node_spans_ms'] = {name: stats(values) for name, values in spans.items()}
    summary['duration_scope'] = 'P1-to-P3 spans only, nominal DTU; not the complete five-message exchange duration'
    summary['truth_unmatched'] = len(set(truth or {})-matched)
    return summary


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('captures', type=Path, nargs='+')
    p.add_argument('--truth', type=Path, help='JSONL: sample_key string and finite value_m')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--keys', type=Path, help='Export exact observation keys for independent truth preparation')
    args = p.parse_args()
    truth = {}
    if args.truth:
        for line in args.truth.read_text().splitlines():
            row = json.loads(line);value = row['value_m'];key = row['sample_key']
            if key in truth or not isinstance(value, (int, float)) or not math.isfinite(value):
                p.error('duplicate truth key or nonfinite value')
            truth[key] = value
    result = analyze(args.captures, truth)
    keys = result.pop('raw_keys')
    if args.keys:
        with args.keys.open('x') as stream:
            for row in keys:
                stream.write(json.dumps(row, allow_nan=False)+'\n')
    with args.output.open('x') as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
    print(json.dumps(dict(measurement_types=list(result['samples']), truth_unmatched=result['truth_unmatched'], output=str(args.output))))


if __name__ == '__main__':
    main()
