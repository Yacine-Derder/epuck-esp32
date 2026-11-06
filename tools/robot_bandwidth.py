#!/usr/bin/env python3
"""Compare received experiment JSONL traffic; no hardware connections."""
import argparse
import collections
import json
from pathlib import Path
try:
    from wifi_client import parse_data
    from uwb_collect import decode
except ModuleNotFoundError:
    from tools.wifi_client import parse_data
    from tools.uwb_collect import decode


def summarize(path):
    counts=collections.Counter();times=[];wire_bytes=0;steps=[];previous={};diagnostics=[]
    with Path(path).open() as stream:
        for text in stream:
            row=json.loads(text)
            if row.get('kind')!='received':continue
            line=row['raw'];now=row['received_unix'];times.append(now);wire_bytes+=len(line.encode('ascii'))+1
            counts['received_lines']+=1
            if line.startswith('DATA2 '):
                sample=parse_data(line);steps.append((now,sample.sequence));counts['steps']+=1
                counts['stale_steps']+=sample.reported_age_ms>300
                key=(row.get('capture'),row.get('generation'),row.get('session'))
                old=previous.get(key);previous[key]=sample.sequence
                if old is not None:
                    delta=(sample.sequence-old)&0xffffffff
                    if 0<delta<2**31:counts['missing_source_sequences']+=delta-1
                    else:counts['step_discontinuities']+=1
            if line.startswith(('UWB1 ','UWB2 ','UWB3 ')):
                try:r=decode(line)
                except ValueError:counts['corrupt_uwb']+=1;continue
                counts[r['type']]+=1
                if r['type']=='tdoa_vector':
                    counts['valid_tdoa_components']+=sum(f['valid'] for f in r['followers'])
                    counts['total_tdoa_components']+=len(r['followers'])
            if line.startswith(('UWBSTAT ','[LINK]','DIAG ','DISCONNECT ','ERR ')):
                diagnostics.append(line);diagnostics=diagnostics[-20:]
    duration=times[-1]-times[0] if len(times)>1 else 0
    return dict(file=str(path),receipt_span_seconds=duration,counts=dict(counts),tcp_application_bytes=wire_bytes,
                application_bytes_per_second=wire_bytes/duration if duration else None,
                steps_hz=(len(steps)-1)/(steps[-1][0]-steps[0][0]) if len(steps)>1 and steps[-1][0]>steps[0][0] else None,
                tdoa_vectors_per_second=counts['tdoa_vector']/duration if duration else None,
                last_diagnostics=diagnostics,timing='PC receipt only; application bytes exclude TCP/Wi-Fi overhead')


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('recordings',nargs='+',type=Path);a=p.parse_args()
    print(json.dumps([summarize(path) for path in a.recordings],indent=2))

if __name__=='__main__':main()
