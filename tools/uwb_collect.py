#!/usr/bin/env python3
"""Read-only multi-robot UWB collector. Host reception times are NOT UWB time.

JSONL is streamed to disk with bounded socket input. CONTROL_HB keeps the existing
bridge session alive; this tool never sends ARM, movement or configuration.
"""
import argparse
import asyncio
import binascii
import datetime
import json
from pathlib import Path
import struct
import time
import sys
try:
    from uwb_logging import CompactLog, LiveMonitor, DEFAULT_LOG_DIR, OLD_LOG_DIR, migrate_logs
except ModuleNotFoundError:
    from tools.uwb_logging import CompactLog, LiveMonitor, DEFAULT_LOG_DIR, OLD_LOG_DIR, migrate_logs

try:
    from uwb_multi import decode_multi
    from uwb_clock import decode_clock
except ModuleNotFoundError:
    from tools.uwb_multi import decode_multi
    from tools.uwb_clock import decode_clock

NAMES={1:'summary',2:'range',3:'event',4:'observation',5:'peer',6:'hybrid_rx',7:'tdoa',8:'tdoa_vector',9:'multi_rx',10:'clock'}

def decode(line):
    if not ((line.startswith('UWB1 ') and len(line)==261) or (line.startswith(('UWB2 ','UWB3 ')) and len(line)==309)):
        raise ValueError('length/prefix')
    try: data=bytes.fromhex(line[5:])
    except ValueError as error: raise ValueError('hex') from error
    if len(data) not in (128,152) or data[:2]!=b'MT' or data[2]!=int(line[3]) or binascii.crc_hqx(data[:-2],0xffff)!=int.from_bytes(data[-2:],'little'):
        raise ValueError('version/CRC')
    if data[3] not in NAMES or data[6] not in (1,2) or data[7]>5 or data[8] not in (1,2) or data[9]>31 or data[10]>2:
        raise ValueError('fields')
    epoch,exchange=struct.unpack_from('<QQ',data,16)
    if not epoch: raise ValueError('epoch')
    sequence,=struct.unpack_from('<I',data,12)
    transaction,uptime,raw,cal=struct.unpack_from('<IIii',data,32)
    out=dict(version=data[2],type=NAMES[data[3]],node=data[4],peer=data[5],radio=data[6],mode=data[7],role=data[8],
             flags=data[9],remote_radio=data[10],quality=data[11],sequence=sequence,epoch=f'{epoch:016X}',exchange=f'{exchange:016X}',
             transaction=transaction,uptime_ms=uptime,raw_mm=raw,calibrated_mm=cal,
             raw_valid=bool(data[9]&1),calibration=bool(data[9]&2),calibrated_valid=bool(data[9]&4))
    if data[3]==10:
        return decode_clock(data,out)
    if data[3] in (8,9):
        return decode_multi(data, out)
    if data[3]==7:
        ref_seq,fol_seq,dt,dd,rate=struct.unpack_from('<IIiii',data,40)
        ref,fol,status,clock,state,policy=data[60:66]
        rx=int.from_bytes(data[66:71],'little');slot=int.from_bytes(data[94:96],'little')
        available=status==4;sentinel=-2147483648
        if (data[2]!=3 or data[6]!=2 or data[8]!=2 or data[7] not in (4,5) or policy!=(data[7]==5)
            or ref==fol or 255 in (ref,fol) or data[4] in (ref,fol) or status>4 or clock>1 or state>2
            or data[112]>4 or data[113]>63 or data[114]>7 or slot not in (500,50,10)
            or (state==1)!=(rate!=sentinel) or (state and not policy)
            or (available and (not clock or state!=1))
            or (available and sentinel in (dt,dd)) or (not available and (dt!=sentinel or dd!=sentinel))
            or data[71] or any(data[96:104]) or any(data[115:118]) or any(data[130:150])):
            raise ValueError('tdoa envelope')
        for key in ('raw_mm','calibrated_mm','raw_valid','calibration','calibrated_valid'):out.pop(key)
        out.update(reference_sequence=ref_seq,follower_sequence=fol_seq,reference_node=ref,follower_node=fol,
                   status=status,status_name=('no_relation','no_rate','stale_rate','inconsistent','available')[status],
                   valid=available,clock_valid=bool(clock),rate_state=state,rate_ppb=rate if state==1 else None,
                   rate_policy='stationary' if policy else 'unknown',tdoa_dtu=dt if available else None,
                   range_diff_mm=dd if available else None,rx=f'{rx:010X}',slot_ms=slot,
                   reference_generation=int.from_bytes(data[80:84],'little'),follower_generation=int.from_bytes(data[84:88],'little'),
                   queue_depth=int.from_bytes(data[88:90],'little'),queue_high=int.from_bytes(data[90:92],'little'),
                   capture_age_ms=int.from_bytes(data[92:94],'little'),delay_age_ms=int.from_bytes(data[104:108],'little'),
                   delay_dtu=int.from_bytes(data[108:110],'little'),clock_age_ms=int.from_bytes(data[110:112],'little'),
                   delay_state=data[112],unknown_components=data[113],power_flags=data[114],
                   dropped=int.from_bytes(data[72:76],'little'),uart_errors=int.from_bytes(data[76:80],'little'),
                   radio_errors=int.from_bytes(data[118:122],'little'),timeouts=int.from_bytes(data[122:126],'little'),
                   tx_dropped=int.from_bytes(data[126:130],'little'),source='stm32_onboard',dtu_nominal_hz=63897600000,
                   local_clock_domain=dict(node=out['node'],radio=out['radio'],epoch=out['epoch']))
        return out
    if data[3]==6:
        n=data[129];slot=int.from_bytes(data[141:143],'little')
        if data[2]!=3 or n not in (34,74,84) or data[140]>1 or slot not in (500,50,10) or any(data[40+n:124]) or any(data[147:150]):raise ValueError('legacy envelope')
        for key in ('raw_mm','calibrated_mm','raw_valid','calibration','calibrated_valid'):out.pop(key)
        out.update(legacy_frame=data[40:40+n].hex().upper(),legacy_length=n,rx=f'{int.from_bytes(data[124:129],"little"):010X}',
                   capture_age_ms=int.from_bytes(data[130:132],'little'),reference_generation=int.from_bytes(data[132:136],'little'),
                   follower_generation=int.from_bytes(data[136:140],'little'),rate_policy='stationary' if data[140] else 'unknown',slot_ms=slot,
                   radio_errors=int.from_bytes(data[143:147],'little'),remote_epoch_known=False,epoch_scope='receiver boot/radio epoch; ALR epochs are inferred/acquisition-local',
                   dtu_nominal_hz=63897600000,local_clock_domain=dict(node=out['node'],radio=out['radio'],epoch=out['epoch']))
        return out
    if data[3]>5 or (data[2]<3 and data[7]>3):raise ValueError('version/mode')
    for offset,name in zip(range(48,68,4),('attempts','completed','aborted','rejected','invalid')):out[name]=struct.unpack_from('<I',data,offset)[0]
    out['queue_depth'],out['queue_high']=struct.unpack_from('<HH',data,68)
    for offset,name in zip(range(72,96,4),('dropped','uart_errors','spi_off_cycles','service_cycles','gap_cycles','cpu_hz')):out[name]=struct.unpack_from('<I',data,offset)[0]
    if len(data)==152:
        out.update(zip(('radio_errors','timeouts','tx_dropped','model_version','remote_epoch'),struct.unpack_from('<IIIIQ',data,126)))
        out['remote_epoch']=f'{out["remote_epoch"]:016X}'
    out['stamps']=[f'{int.from_bytes(data[96+5*i:101+5*i],"little"):010X}' for i in range(6)]
    out['dtu_nominal_hz']=63897600000
    out['timestamp_units']='native 1/63897600000 s DTU, modulo 2^40'
    out['local_clock_domain']=dict(node=out['node'],radio=out['radio'],epoch=out['epoch'])
    return out

class Ordering:
    def __init__(self):self.last={}
    def accept(self,r):
        key=(r['node'],r['radio'],r['epoch']);old=self.last.get(key);seq=r['sequence']
        if old is None:
            self.last[key]=seq
            if len(self.last)>128:del self.last[next(iter(self.last))]
            return dict(sequence_status='first',missing=0)
        delta=(seq-old)&0xffffffff
        if not delta:return dict(sequence_status='duplicate',missing=0)
        if delta>=0x80000000:return dict(sequence_status='old',missing=0)
        self.last[key]=seq
        return dict(sequence_status='forward' if delta==1 else 'gap',missing=delta-1)

async def collect(endpoint,output,stop=None, *, raw=False,health_seconds=30,monitor=None,metadata=None):
    host,sep,port=endpoint.rpartition(':')
    if not sep:host,port=endpoint,'3333'
    order=Ordering();reconnects=0;corrupt=0;missing=0
    compact=None if raw else CompactLog(output,endpoint,health_seconds,metadata=metadata)
    def emit(kind,**fields):
        record=dict(kind=kind,endpoint=endpoint,host_received_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),host_monotonic_ns=time.monotonic_ns(),**fields)
        if monitor:monitor.accept(endpoint,record)
        if compact:compact.accept(record)
        else:output.write(json.dumps(record,sort_keys=True,allow_nan=False)+'\n');output.flush()
    if raw and metadata is not None:emit('experiment',metadata=metadata)
    while stop is None or not stop.is_set():
        writer=None
        try:
            reader,writer=await asyncio.wait_for(asyncio.open_connection(host,int(port),limit=2048),5)
            reconnects+=1;emit('connected',connections=reconnects)
            last=time.monotonic()
            while stop is None or not stop.is_set():
                # Read-only heartbeat; stalled workstation I/O may safely expire the session.
                if time.monotonic()-last>=.1:
                    writer.write(b'CONTROL_HB\n');await asyncio.wait_for(writer.drain(),.2);last=time.monotonic()
                try: raw=await asyncio.wait_for(reader.readline(),.05)
                except asyncio.TimeoutError:continue
                if not raw:raise ConnectionError('eof')
                line=raw.decode('ascii',errors='replace').rstrip('\r\n')
                if line.startswith(('UWB1 ','UWB2 ','UWB3 ')):
                    try:r=decode(line)
                    except ValueError as error:corrupt+=1;emit('corrupt',reason=str(error),count=corrupt);continue
                    ordering=order.accept(r);missing+=ordering['missing'];emit('telemetry',**r,**ordering,missing_total=missing)
                else:emit('bridge',line=line)
        except (OSError,ValueError,ConnectionError,asyncio.TimeoutError) as error:
            emit('disconnected',reason=str(error),connections=reconnects,corrupt=corrupt,missing=missing)
        finally:
            if writer:
                writer.close()
                try:await asyncio.wait_for(writer.wait_closed(),.2)
                except (OSError,asyncio.TimeoutError):pass
        if stop is None or not stop.is_set():await asyncio.sleep(1)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--robot',action='append',help='HOST[:PORT], repeat for multiple robots')
    parser.add_argument('--output',type=Path,default=DEFAULT_LOG_DIR,help='JSONL directory (default: ~/Documents/uwb_logs)')
    parser.add_argument('--raw','--debug',dest='raw',action='store_true',help='Complete historical diagnostic JSONL; retain native frames/epochs')
    parser.add_argument('--health-seconds',type=float,default=30,help='Compact health interval; 0 disables periodic health')
    parser.add_argument('--metadata',type=Path,help='Optional experiment JSON object: ID, ALR positions and ground truth; copied verbatim')
    parser.add_argument('--no-live',action='store_true',help='Disable the compact live TDoA status display')
    parser.add_argument('--migrate-logs',action='store_true',help='Copy/verify recognized old logs, retain originals, then exit')
    parser.add_argument('--old-log-dir',type=Path,default=OLD_LOG_DIR,help='Migration source; used only with --migrate-logs')
    args=parser.parse_args()
    if args.migrate_logs:
        print(json.dumps(migrate_logs(args.old_log_dir,args.output),indent=2));return
    if not args.robot:parser.error('--robot is required unless --migrate-logs is selected')
    import math
    if not math.isfinite(args.health_seconds) or args.health_seconds<0:parser.error('--health-seconds must be finite and nonnegative')
    metadata=None
    if args.metadata:
        if args.metadata.stat().st_size>65536:parser.error('--metadata exceeds 64KiB')
        metadata=json.loads(args.metadata.read_text())
        if not isinstance(metadata,dict):parser.error('--metadata must contain a JSON object')
        json.dumps(metadata,allow_nan=False)
    args.output.mkdir(parents=True,exist_ok=True)
    if len(set(args.robot))!=len(args.robot):parser.error('duplicate endpoint')
    stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    monitor=LiveMonitor(not args.no_live)
    async def run():
        streams=[]
        try:
            tasks=[]
            for i,robot in enumerate(args.robot):
                suffix='raw' if args.raw else 'compact'
                f=(args.output/f'{stamp}-robot-{i}-{suffix}.jsonl').open('x');streams.append(f)
                print(f'Recording {suffix}: {f.name}',file=sys.stderr)
                tasks.append(collect(robot,f,raw=args.raw,health_seconds=args.health_seconds,monitor=monitor,metadata=metadata))
            await asyncio.gather(*tasks)
        finally:
            for f in streams:f.close()
    try:asyncio.run(run())
    except KeyboardInterrupt:pass
    finally:monitor.close()
if __name__=='__main__':main()
