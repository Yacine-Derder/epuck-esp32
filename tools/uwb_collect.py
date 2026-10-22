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

NAMES={1:'summary',2:'range',3:'event',4:'observation',5:'peer',6:'hybrid_rx'}

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

async def collect(endpoint,output,stop=None):
    host,sep,port=endpoint.rpartition(':')
    if not sep:host,port=endpoint,'3333'
    order=Ordering();reconnects=0;corrupt=0;missing=0
    def emit(kind,**fields):
        record=dict(kind=kind,endpoint=endpoint,host_received_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),host_monotonic_ns=time.monotonic_ns(),**fields)
        output.write(json.dumps(record,sort_keys=True)+'\n');output.flush()
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
    parser.add_argument('--robot',action='append',required=True,help='HOST[:PORT], repeat for multiple robots')
    parser.add_argument('--output',type=Path,required=True,help='JSONL output directory')
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    if len(set(args.robot))!=len(args.robot):parser.error('duplicate endpoint')
    stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    async def run():
        streams=[]
        try:
            tasks=[]
            for i,robot in enumerate(args.robot):
                f=(args.output/f'{stamp}-robot-{i}.jsonl').open('x');streams.append(f);tasks.append(collect(robot,f))
            await asyncio.gather(*tasks)
        finally:
            for f in streams:f.close()
    try:asyncio.run(run())
    except KeyboardInterrupt:pass
if __name__=='__main__':main()
