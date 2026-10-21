#!/usr/bin/env python3
"""Offline native-40-bit beacon acquisition audit; never infer global synchronization.

Input: uwb_collect.py JSONL files. Output: observed inputs, apparent arrival-clock
models (offset includes propagation), and TDoA ONLY with explicit external maps.
"""
import argparse
from collections import OrderedDict
from fractions import Fraction
import json
from pathlib import Path

MOD=1<<40
HZ=63897600000
MAX=5*HZ

def delta(a,b):
    return (a-b+MOD//2)%MOD-MOD//2

def stamp(value):
    if not isinstance(value,str) or len(value)!=10 or any(c not in '0123456789abcdefABCDEF' for c in value):raise ValueError('40-bit hex timestamp required')
    n=int(value,16)
    if not 0<=n<MOD:raise ValueError('timestamp width')
    return n

def domain(node,radio,epoch):
    if type(node) is not int or not 0<=node<=255 or type(radio) is not int or radio not in (1,2):raise ValueError('clock identity')
    if not isinstance(epoch,str) or len(epoch)!=16 or any(c not in '0123456789abcdefABCDEF' for c in epoch) or not int(epoch,16):raise ValueError('nonzero 64-bit epoch required')
    return node,radio,epoch.upper()

def as_domain(d):return domain(d['node'],d['radio'],d['epoch'])
def named(d):return dict(node=d[0],radio=d[1],epoch=d[2])

class Arrival:
    """Remote TX -> receiver arrival; propagation is inseparable from offset."""
    def __init__(self):self.last=None;self.samples=0;self.rejected=0;self.version=0;self.rate=Fraction(1);self.valid=False
    def update(self,tx,rx):
        if self.last is None:
            self.last=(tx,rx);self.samples=1;self.version+=1;return True
        dt,dr=delta(tx,self.last[0]),delta(rx,self.last[1])
        if dt<=0 or dr<=0 or dt>MAX or dr>MAX:
            self.rejected+=1
            if dt>MAX and dr>MAX: # Reacquire after bounded horizon; never bridge ambiguous wraps.
                self.last=None;self.samples=0;self.valid=False;self.version+=1
            return False
        ratio=Fraction(dr,dt);residual=dr-self.rate*dt
        if not Fraction(999,1000)<=ratio<=Fraction(1001,1000) or (self.valid and abs(residual)>12780):
            self.rejected+=1;return False
        self.rate=ratio if self.samples==1 else ((7*self.rate+ratio)/8).limit_denominator(1<<48)
        self.last=(tx,rx);self.samples+=1;self.valid=self.samples>=3;self.version+=1;return True

class Maps:
    def __init__(self,spec=None):
        self.entries={};self.target=None
        if spec is None:return
        self.target=as_domain(spec['target_domain'])
        for m in spec['mappings']:
            key=as_domain(m['domain'])
            if key in self.entries:raise ValueError('duplicate clock map')
            rate=Fraction(str(m['rate']));uncertainty=Fraction(str(m['uncertainty_dtu']));horizon=int(m['max_delta_dtu'])
            if not Fraction(999,1000)<=rate<=Fraction(1001,1000) or not 0<=uncertainty<=MAX or not 0<horizon<=MAX or not m['provenance']:
                raise ValueError('mapping envelope/provenance')
            self.entries[key]=(stamp(m['local_anchor']),stamp(m['reference_anchor']),rate,uncertainty,horizon,m['provenance'])
    def apply(self,key,rx):
        m=self.entries.get(key)
        if m is None:return None
        local,reference,rate,uncertainty,horizon,provenance=m;d=delta(rx,local)
        if abs(d)>horizon:return None
        return reference+rate*d,uncertainty,provenance

def analyze(records,maps=None,max_groups=100000):
    maps=maps or Maps();groups=OrderedDict();models=OrderedDict()
    def group_rows(key,observations,evicted=False):
        remote,seq=key;values=list(observations.items())
        if len(values)<2:
            yield dict(kind='tdoa',status='missing_receiver',transmitter=named(remote),transaction=seq,evicted=evicted);return
        first,one=values[0]
        for second,two in values[1:]:
            row=dict(kind='tdoa',transmitter=named(remote),transaction=seq,receiver_a=named(first),receiver_b=named(second),
                     rx_a=f'{one[0]:010X}',rx_b=f'{two[0]:010X}',evicted=evicted)
            a,b=maps.apply(first,one[0]),maps.apply(second,two[0])
            if a is None or b is None:
                row.update(status='missing_sync',reason='independent RX clocks; matching TX metadata does not separate offset from propagation')
            else:
                difference=delta(b[0],a[0])
                if abs(difference)>MAX:row.update(status='mapping_inconsistent')
                else:
                    row.update(status='externally_mapped',reference_domain=named(maps.target),tdoa_b_minus_a_dtu=float(difference),
                               tdoa_ns=float(difference)*1e9/HZ,distance_difference_m=float(difference)*299792458/HZ,
                               uncertainty_bound_dtu=float(a[1]+b[1]),mapping_provenance=[a[2],b[2]])
            yield row
    for r in records:
        if not isinstance(r,dict):
            yield dict(kind='invalid',reason='JSON record must be an object');continue
        if r.get('kind')!='telemetry' or r.get('mode')!=3 or r.get('type') not in ('event','observation') or r.get('flags',0)&16:continue
        try:
            if not r.get('flags',0)&8:raise ValueError('invalid timestamp')
            local=domain(r['node'],r['radio'],r['epoch']);remote=domain(r['peer'],r['remote_radio'],r['remote_epoch'])
            seq=r['transaction']
            if type(seq) is not int or not 0<=seq<1<<32:raise ValueError('sequence width')
            rx=stamp(r['stamps'][0]);paired=r['type']=='observation'
            tx=stamp(r['stamps'][1]) if paired else None
        except (KeyError,ValueError,TypeError,IndexError) as error:
            yield dict(kind='invalid',reason=str(error));continue
        key=(remote,seq);g=groups.setdefault(key,{})
        # Later metadata supplements the same RX; contradictory duplicates stay explicit.
        if local in g and g[local][0]!=rx:
            yield dict(kind='invalid',reason='conflicting RX for one domain/session/sequence',transmitter=named(remote),transaction=seq);continue
        if paired and any(v[2] is not None and v[2]!=tx for v in g.values()):
            yield dict(kind='invalid',reason='conflicting completed TX for transmitter/session/sequence',transmitter=named(remote),transaction=seq);continue
        if local in g and g[local][1]:continue
        g[local]=(rx,paired,tx)
        yield dict(kind='observation',receiver=named(local),transmitter=named(remote),transaction=seq,rx=f'{rx:010X}',
                   advertised_completed_tx=f'{tx:010X}' if paired else None,online_quality=r.get('quality'),online_model_version=r.get('model_version'))
        if paired:
            model=models.setdefault((remote,local),Arrival());accepted=model.update(tx,rx)
            yield dict(kind='clock_model',source=named(remote),target=named(local),accepted=accepted,valid_apparent=model.valid,
                       samples=model.samples,rejected=model.rejected,model_version=model.version,apparent_rate=float(model.rate),
                       apparent_skew_ppm=float(model.rate-1)*1e6,absolute_offset_identifiable=False,propagation_identifiable=False,
                       assumption='stable propagation during fit; motion can bias apparent clock rate')
            models.move_to_end((remote,local))
            if len(models)>512:
                models.popitem(last=False);yield dict(kind='limit',reason='clock model cache evicted at 512 domains')
        if len(groups)>max_groups:
            old_key,old=groups.popitem(last=False);yield from group_rows(old_key,old,True)
    for key,g in groups.items():yield from group_rows(key,g)

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('logs',nargs='+',type=Path)
    parser.add_argument('--maps',type=Path,help='external calibrated common-clock mapping JSON, never fitted one-way offsets')
    parser.add_argument('--output',type=Path,required=True);parser.add_argument('--max-groups',type=int,default=100000)
    args=parser.parse_args()
    if args.max_groups<1:parser.error('max-groups must be positive')
    spec=json.loads(args.maps.read_text()) if args.maps else None;maps=Maps(spec)
    def records():
        for path in args.logs:
            with path.open() as stream:
                for line in stream:
                    try:yield json.loads(line)
                    except (ValueError,TypeError):yield {'kind':'telemetry','mode':3,'type':'observation','flags':0}
    with args.output.open('x') as out:
        for row in analyze(records(),maps,args.max_groups):out.write(json.dumps(row,sort_keys=True)+'\n')
if __name__=='__main__':main()
