#!/usr/bin/env python3
"""Audit existing compact telemetry without devices, re-estimation or data mutation."""
import argparse
from collections import Counter,defaultdict
import json
from pathlib import Path

def summarize(rows):
    vectors=[r for r in rows if r.get('type')=='tdoa_vector']
    replay={(r.get('s'),r['cycle'],r['pair_slot']):r for r in rows if r.get('type')=='tdoa_replay'}
    counts=Counter();digits=defaultdict(Counter);versions=Counter();drops=defaultdict(list)
    for r in rows:
        if r.get('type')=='tdoa_replay':
            versions[r['relation']['version']]+=1
            drops[r.get('s')].append(r['diagnostic_dropped'])
    for v in vectors:
        for f in v['followers']:
            snap=replay.get((v.get('s'),v['cycle'],f['slot']))
            if f['valid']:category='available'
            elif not v['seen_mask']&1:category='reference_not_observed'
            elif not f['seen']:category='follower_not_observed'
            elif snap and not snap['relation']['available']:
                category='propagation_expired' if snap['relation'].get('delay_state')==3 else 'follower_relation_unavailable_cause_unknown'
            elif snap and snap['clock']['status']!=2:category='receiver_clock_invalid'
            else:category='other_or_missing_replay'
            counts[category]+=1;digits[v['cycle']%10][category]+=1
    total=sum(counts.values());valid=counts['available']
    return dict(vector_pairs=total,valid=valid,valid_percent=100*valid/total if total else None,
        causes=dict(counts),cycle_last_digit={str(k):dict(v) for k,v in sorted(digits.items())},
        relation_versions=dict(versions),gap_rows=sum(r.get('missing',0) for r in rows if r.get('type')=='gap'),
        diagnostic_dropped_capture_increments=sum(sum(max(0,b-a) for a,b in zip(v,v[1:])) for v in drops.values()),
        scope='RF not-observed is not proof of over-air loss; v1 unavailable reason cannot distinguish expiry from follower clock failure. Sequence gaps may include intentional filtering. Boot counters are not capture increments.')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('recordings',type=Path,nargs='+');a=p.parse_args()
    for path in a.recordings:
        print(json.dumps(dict(recording=str(path),**summarize([json.loads(l) for l in path.open() if l.strip()])),indent=2))
