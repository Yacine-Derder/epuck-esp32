#!/usr/bin/env python3
"""Replay captured legacy TDMA frames through the exact shared C receiver core.
Requires a host C compiler, no radio/robot access. JSONL output never calls an
inter-ALR TWR measurement an RLR range. Host reception times are not UWB time.
"""
import argparse
import ctypes
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
STATUS={0:'NO_RELATION',1:'NO_RATE',2:'STALE_RATE',3:'INCONSISTENT',4:'AVAILABLE'}
class Replay:
    def __init__(self,slot=500,reference=0x84,follower=0x85):
        if slot not in (500,50,10):raise ValueError('slot')
        if any(type(n) is not int or not 0<=n<=254 for n in (reference,follower)) or reference==follower:raise ValueError('distinct reference/follower IDs 0..254 required')
        self.reference=reference;self.follower=follower
        self.generations={};self.slot=slot;self.temp=tempfile.TemporaryDirectory(prefix='mul-hybrid-replay-');self.contexts={}
        core=ROOT/'ThirdParty/mul_core';library=Path(self.temp.name)/'hybrid.so'
        subprocess.run([sys.executable,str(core/'verify.py')],check=True,stdout=subprocess.PIPE)
        names=['mul_common','mul_clock','mul_relation','mul_observation','mul_downlink','mul_localization','mul_legacy_twr','mul_hybrid']
        subprocess.run(['cc','-std=c11','-O2','-shared','-fPIC',f'-DMUL_FOLLOWER_SLOT_MS={slot}','-DMUL_PROP_MAX_AGE_MS=6000','-I'+str(core),str(ROOT/'tools/native/hybrid_replay.c'),*[str(core/(name+'.c')) for name in names],'-o',str(library)],check=True)
        self.lib=ctypes.CDLL(str(library));self.lib.hybrid_new_network.argtypes=[ctypes.c_uint]*4;self.lib.hybrid_new_network.restype=ctypes.c_void_p
        self.lib.hybrid_free.argtypes=[ctypes.c_void_p]
        self.lib.hybrid_feed.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_uint,ctypes.c_uint64,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p,ctypes.c_uint]
    def close(self):
        for context in self.contexts.values():self.lib.hybrid_free(context)
        self.contexts.clear();self.generations.clear();self.temp.cleanup()
    def feed(self,r):
        if r.get('kind')!='telemetry' or r.get('type')!='hybrid_rx':return None
        try:
            node=r['node'];epoch=r['epoch'];radio=r['radio'];policy=r['rate_policy']
            if type(node) is not int or not 0<=node<=255 or node in (self.reference,self.follower) or radio!=2 or policy not in ('unknown','stationary') or len(epoch)!=16 or not 0<int(epoch,16)<1<<64:raise ValueError('domain/policy')
            if not r.get('flags',0)&8 or r.get('mode') not in (4,5) or r.get('role')!=2 or r.get('remote_radio')!=1 or (r.get('mode')==5)!=(policy=='stationary'):raise ValueError('timestamp validity/mode')
            if r['slot_ms']!=self.slot:raise ValueError('slot mismatch; replay each build profile separately')
            frame=bytes.fromhex(r['legacy_frame']);rx=int(r['rx'],16);age=r['capture_age_ms'];now=r['uptime_ms']
            generations=(r['reference_generation'],r['follower_generation'])
            if any(type(g) is not int or not 0<=g<1<<32 for g in generations):raise ValueError('source generation')
            if len(frame) not in (34,74,84) or len(frame)!=r['legacy_length'] or frame[13]!=r['peer'] or int.from_bytes(frame[25:29],'little')!=r['transaction'] or not 0<=rx<1<<40 or not 0<=age<=65535 or not 0<=now<1<<32:raise ValueError('legacy shape/time')
        except (KeyError,ValueError,TypeError) as error:return dict(kind='invalid',reason=str(error))
        key=(node,radio,epoch,policy)
        old_generations=self.generations.get(key)
        generation_reset=old_generations is not None and any(a and b and a!=b for a,b in zip(old_generations,generations))
        if generation_reset:self.lib.hybrid_free(self.contexts.pop(key))
        context_created=key not in self.contexts
        if context_created:
            # Bound receiver epochs, retaining no cross-boot association history.
            if len(self.contexts)>=128:
                old_key=next(iter(self.contexts));self.lib.hybrid_free(self.contexts.pop(old_key));self.generations.pop(old_key,None)
            context=self.lib.hybrid_new_network(node,policy=='stationary',self.reference,self.follower)
            if not context:raise MemoryError('receiver context')
            self.contexts[key]=context
        self.generations[key]=generations
        output=ctypes.create_string_buffer(2048);data=ctypes.create_string_buffer(frame)
        result=self.lib.hybrid_feed(self.contexts[key],data,len(frame),rx,age,now,output,len(output))
        if result<=0:return dict(kind='invalid',reason='legacy decoder/receiver rejected frame' if result==0 else 'output limit')
        row=json.loads(output.value)
        row.update(kind='hybrid_observation' if row['associated'] else 'inter_alr_twr' if row['twr_complete'] else 'legacy_rx',
                   receiver_domain=dict(node=node,radio=radio,epoch=epoch),observed_ms=now,slot_ms=self.slot,rate_policy=policy,
                   remote_boot_epoch_known=False,physical_uncertainty='unknown',stationarity_assumed=policy=='stationary')
        row['status']=STATUS.get(row['status_code'],'NOT_ASSOCIATED')
        row['replay_context_created']=context_created
        row['replay_reset_on_captured_generation']=generation_reset
        row['captured_reference_generation']=r.get('reference_generation');row['captured_follower_generation']=r.get('follower_generation')
        if row['associated']:
            row['measurement']=dict(kind='range_difference',reference=str(row['reference']),anchor=str(row['follower']),
                 value_m=row['tdoa_dtu']*299792458/63897600000,valid=True) if row['status']=='AVAILABLE' else None
        if row['twr_complete']:
            row['measurement_target']='inter_ALR_baseline_not_RLR_range'
            if row['twr_corrected_tof_dtu']==0xffffffff:row['twr_corrected_tof_dtu']=None
        else:
            row.pop('twr_raw_tof_dtu');row.pop('twr_corrected_tof_dtu')
        return row

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('logs',nargs='+',type=Path);p.add_argument('--slot',type=int,choices=(500,50,10),default=500)
    p.add_argument('--reference',type=lambda s:int(s,0),default=0x84);p.add_argument('--follower',type=lambda s:int(s,0),default=0x85)
    p.add_argument('--output',required=True,type=Path);p.add_argument('--anchors',type=Path);p.add_argument('--positions',type=Path)
    args=p.parse_args()
    if bool(args.anchors)!=bool(args.positions):p.error('--anchors and --positions must be supplied together')
    from uwb_localize import load_geometry,solve_2d
    geometry=load_geometry(args.anchors) if args.anchors else None
    try:replay=Replay(args.slot,args.reference,args.follower)
    except ValueError as error:p.error(str(error))
    positions=args.positions.open('x') if args.positions else None
    try:
        with args.output.open('x') as out:
            for file in args.logs:
                with file.open() as stream:
                    for line in stream:
                        try:r=json.loads(line);row=replay.feed(r) if isinstance(r,dict) else dict(kind='invalid',reason='JSON object required')
                        except (ValueError,TypeError):row=dict(kind='invalid',reason='malformed JSON')
                        if row is None:continue
                        out.write(json.dumps(row,sort_keys=True)+'\n')
                        if positions and row.get('associated'):
                            result=solve_2d(geometry['anchors'],[row['measurement']] if row.get('measurement') else [],bounds=geometry.get('bounds'))
                            result.update(receiver_domain=row['receiver_domain'],observed_ms=row['observed_ms'],measurement_status=row['status'],stationarity_assumed=row['stationarity_assumed'])
                            positions.write(json.dumps(result,sort_keys=True)+'\n')
    finally:
        replay.close()
        if positions:positions.close()
if __name__=='__main__':main()
