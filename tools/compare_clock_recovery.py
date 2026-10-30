#!/usr/bin/env python3
"""Read-only receiver counterfactual: identical raw snapshots, follower relation fixed.
No independent ground truth is inferred. No device access; old core from local Git.
"""
import argparse,io,json,subprocess,tarfile,tempfile,statistics,re,ctypes as C
from pathlib import Path
from uwb_replay import Reassembly
ROOT=Path(__file__).resolve().parents[1]
def compare(path,lps,stm,revision,alr=None):
 rows=[json.loads(l) for l in path.open()];assembly=Reassembly();snaps=[]
 for row in rows:
  if row.get('type')=='tdoa_replay_fragment':
   snaps.extend(x for x in assembly.accept(row,'comparison') if x['type']=='tdoa_replay')
 if not snaps:raise ValueError('no complete replay snapshots')
 if len({(x['node'],x['radio'],x['epoch'],x['pair_slot']) for x in snaps})>1:raise ValueError('single receiver epoch and pair required')
 data=b''.join(bytes.fromhex(x['blob']) for x in snaps)
 with tempfile.TemporaryDirectory() as tmp:
  work=Path(tmp);archive=subprocess.check_output(['git','archive',revision,'portable/mul_core'],cwd=lps)
  with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
   for member in tar.getmembers():
    if member.isfile():
     target=work/member.name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(tar.extractfile(member).read())
  results={}
  for tag,core in [('old',work/'portable/mul_core'),('new',lps/'portable/mul_core')]:
   exe=work/tag
   args=['cc','-std=c11','-O2','-DMUL_PROP_MAX_AGE_MS=6000','-I'+str(core),'-I'+str(stm/'Core/Inc'),'-I'+str(stm/'tests/phy_interop/stubs')]
   if tag=='new':args+=['-DRECOVERY_BUILD']
   args += [str(stm/'tools/clock_recovery_native.c'),str(stm/'Core/Src/mul_replay.c')]+[str(p) for p in core.glob('*.c') if p.name!='mul_multi_receiver.c']+['-lm','-o',str(exe)]
   subprocess.run(args,check=True)
   for method in (0,2):
    output=subprocess.check_output([str(exe),str(method)],input=data)
    values=[list(map(int,line.split())) for line in output.splitlines()];results[tag+str(method)]=values
  follower={}
  if alr:
   points=[];previous=None;now=10000
   for seq,tx,rx in re.findall(r'REF_RX src=0x84 seq=(\d+) tx=0x([0-9A-Fa-f]+) rx=0x([0-9A-Fa-f]+)',alr.read_text()):
    seq=int(seq);advance=(seq-previous)&0xffffffff if previous is not None else 0
    if previous is not None:now+=advance*1000 if advance<0x80000000 else 10000
    points.append((seq,int(tx,16),int(rx,16),now&0xffffffff));previous=seq
   for tag,core in [('old',work/'portable/mul_core'),('new',lps/'portable/mul_core')]:
    lib=work/(tag+'.so')
    subprocess.run(['cc','-std=c11','-shared','-fPIC','-O2','-I'+str(core),str(lps/'tools/test/clock_model_api.c'),str(core/'mul_clock_fit.c'),str(core/'mul_clock.c'),str(core/'mul_common.c'),'-lm','-o',str(lib)],check=True)
    api=C.CDLL(str(lib));api.model_new.restype=C.c_void_p;api.model_free.argtypes=[C.c_void_p]
    api.model_step.argtypes=[C.c_void_p,C.c_uint32,C.c_uint64,C.c_uint64,C.c_uint32,C.POINTER(C.c_double)]
    ptr=api.model_new();valid=0;out=(C.c_double*18)()
    for point in points:api.model_step(ptr,*point,out);valid+=out[15]==2
    follower[tag]=dict(observations=len(points),mapping_valid=valid,resets=out[16]);api.model_free(ptr)
  summary={}
  for tag,values in results.items():
   valid=[v[2] for v in values if v[1]==4]
   waits=[]
   for i in range(1,len(values)):
    if 3<=((values[i][0]-values[i-1][0])&0xffffffff)<=6:
     ready=next((v for v in values[i:] if v[7]==2),None)
     waits.append(None if ready is None else (ready[0]-values[i][0])&0xffffffff)
   summary[tag]=dict(recovery_wait_cycles_after_snapshot_gap=waits,pairs=len(values),valid=len(valid),resets=values[-1][3] if values else 0,unapplied=sum(not v[4] for v in values),mean_mm=statistics.mean(valid) if valid else None,std_mm=statistics.pstdev(valid) if valid else None)
  common=[b[2]-a[2] for a,b in zip(results['old2'],results['new2']) if a[1]==b[1]==4]
  assert results['old0']==results['new0'],'Median changed'
  rate_changes=[b[6]-a[6] for a,b in zip(results['old2'],results['new2']) if a[7]==b[7]==2]
  summary['common_kalman']=dict(common_rate_n=len(rate_changes),rms_rate_change_ppb=(statistics.mean(x*x for x in rate_changes)**.5) if rate_changes else None,n=len(common),rms_change_mm=(statistics.mean(x*x for x in common)**.5) if common else None,max_abs_change_mm=max(map(abs,common),default=None))
  print(json.dumps(dict(recording=str(path),results=summary,follower_uncompensated=follower,follower_scope='real TX/RX, inferred1Hz coarse age; no propagation compensation; serial gaps are not proof of RF loss',scope='cold-start receiver-only captured snapshots; original follower relations fixed; no independent ground truth'),indent=2))
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('recording',type=Path);p.add_argument('--old',default='4060df1');p.add_argument('--alr',type=Path);p.add_argument('--lps',type=Path,default=ROOT.parent/'lps-node-firmware');p.add_argument('--stm32',type=Path,default=ROOT.parent/'epuck-stm32');a=p.parse_args();compare(a.recording,a.lps.resolve(),a.stm32.resolve(),a.old,a.alr)
