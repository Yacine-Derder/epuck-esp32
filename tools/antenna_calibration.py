#!/usr/bin/env python3
"""Read-only three-pair antenna-delay proposal; never accesses devices or writes settings.

Input JSON supplies132/133/141 current TX/RX values, three measured antenna
separations, and TWR range samples in metres under one declared power policy.
No values are inferred from archived experiments. See docs/uwb_logging.md.
"""
import argparse
import json
import math
import statistics
from pathlib import Path
NODES=(132,133,141)
DTU_HZ=63897600000
C_M_S=299792458

def number(value):
    if isinstance(value,bool) or not isinstance(value,(int,float)) or not math.isfinite(value):
        raise ValueError('finite numeric values required')
    return value

def rounded(value):
    return math.floor(value+.5) if value>=0 else math.ceil(value-.5)

def propose(data):
    if data.get('additive_correction_removed') is not True:
        raise ValueError('declare additive_correction_removed=true only after subtracting/disabling any ADS additive correction')
    devices=data['devices']
    if set(devices)!=set(map(str,NODES)):
        raise ValueError('supply current antenna registers for132,133,141')
    for values in devices.values():
        for key in ('tx','rx'):
            n=number(values[key])
            if int(n)!=n or not 0<=n<=65535:raise ValueError('register range0..65535')
    errors={};summaries=[];policies=set()
    for pair in data['pairs']:
        nodes=tuple(sorted(pair['nodes']))
        if len(nodes)!=2 or nodes[0]==nodes[1] or any(n not in NODES for n in nodes) or nodes in errors:
            raise ValueError('exactly one run for each distinct pair required')
        distance=number(pair['known_m'])
        samples=[number(n) for n in pair['ranges_m']]
        if distance<=0 or not samples or min(samples)<0:raise ValueError('positive separation/nonnegative samples required')
        policy=pair['power_policy']
        if policy not in ('applied','raw','unavailable'):raise ValueError('declare power_policy applied/raw/unavailable')
        policies.add(policy)
        median=statistics.median(samples)
        errors[nodes]=(median-distance)*DTU_HZ/C_M_S
        summaries.append(dict(nodes=nodes,n=len(samples),known_m=distance,median_m=median,bias_m=median-distance,mad_m=statistics.median(abs(n-median) for n in samples),power_policy=policy))
    if set(errors)!={(132,133),(132,141),(133,141)}:raise ValueError('all three pairs required')
    if len(policies)!=1:raise ValueError('do not mix received-power correction policies')
    output=[]
    for node in NODES:
        a,b=[n for n in NODES if n!=node]
        aggregate=rounded(errors[tuple(sorted((node,a)))]+errors[tuple(sorted((node,b)))]-errors[tuple(sorted((a,b)))])
        tx_delta=rounded(.44*aggregate);rx_delta=aggregate-tx_delta
        old=devices[str(node)];tx=old['tx']+tx_delta;rx=old['rx']+rx_delta
        if not 0<=tx<=65535 or not 0<=rx<=65535:raise ValueError('proposal outside hardware register range; inspect measurements')
        output.append(dict(node=node,current=old,aggregate_delta_dtu=aggregate,delta_tx_dtu=tx_delta,delta_rx_dtu=rx_delta,proposed_tx_dtu=tx,proposed_rx_dtu=rx))
    return dict(proposals=output,pairs=summaries,scope='aggregate delay only; assumed reciprocal links and44/56 incremental TX/RX split; not calibrated accuracy',warning='Power, multipath, geometry and oscillator residuals can masquerade as delay. Validate at another distance; never apply automatically.')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('measurements',type=Path)
    args=parser.parse_args()
    try:print(json.dumps(propose(json.loads(args.measurements.read_text())),indent=2))
    except (ValueError,KeyError,TypeError) as e:parser.error(str(e))
