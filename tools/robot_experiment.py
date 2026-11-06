#!/usr/bin/env python3
"""One TCP owner: simple motor commands plus complete raw dataset/step-rate check."""
import argparse
import datetime
import json
import socket
import time
import uuid
try:
    from wifi_client import parse_data, validate
except ModuleNotFoundError:
    from tools.wifi_client import parse_data, validate


def run(host, port, seconds, commands, output):
    capture=uuid.uuid4().hex
    samples=[];session='';buf=bytearray();motion=any(c.startswith('M ') for c in commands)
    def record(kind,line):
        row=dict(kind=kind,raw=line,capture=capture,session=session,
                 received_unix=time.time(),received_monotonic=time.monotonic())
        if output:output.write(json.dumps(row)+'\n');output.flush()
    with socket.create_connection((host,port),timeout=3) as sock:
        sock.settimeout(.05);opened=time.monotonic();deadline=opened+seconds;sent=False
        record('connected',f'{host}:{port}')
        try:
            while time.monotonic()<deadline:
                if not session and time.monotonic()-opened>1.5:raise ConnectionError('READY timeout; another client may own port 3333')
                try:data=sock.recv(4096)
                except socket.timeout:continue
                if not data:raise ConnectionError('EOF; another client may own port 3333; close ROS/collector/wifi_client first')
                buf.extend(data)
                while b'\n' in buf:
                    line,_,buf=buf.partition(b'\n');line=line.decode('ascii').rstrip('\r');record('received',line)
                    if len(line)>2048:raise ValueError('oversized record')
                    if line.startswith('DISCONNECT '):raise ConnectionError(line)
                    if line.startswith('SESSION '):
                        import re
                        match=re.fullmatch(r'SESSION ([0-9a-f]{16}) READY',line)
                        if not match:raise ConnectionError(line)
                        if session and session!=match[1]:raise ConnectionError('session changed')
                        session=match[1]
                    if session and not sent:
                        for command in commands:
                            sock.sendall((command+'\n').encode());record('motor_command',command)
                        sent=True
                    if line.startswith('DATA2 '):
                        sample=parse_data(line);samples.append((time.monotonic(),sample.sequence,sample.reported_age_ms))
                    if line.startswith(('[MOTOR]','ERR ')):print(line,flush=True)
                if len(buf)>2048:raise ValueError('oversized record')
        finally:
            if session and motion:
                try:
                    sock.sendall(b'STOP\n');record('motor_command','STOP')
                    acknowledged=False
                    until=time.monotonic()+1
                    while time.monotonic()<until:
                        try:data=sock.recv(4096)
                        except socket.timeout:continue
                        if not data:break
                        buf.extend(data)
                        while b'\n' in buf:
                            line,_,buf=buf.partition(b'\n');line=line.decode('ascii').rstrip('\r');record('received',line)
                            if line=='[MOTOR] OK STOP':print(line);acknowledged=True
                        if acknowledged:break
                        if len(buf)>2048:break
                    if not acknowledged:print('STOP not acknowledged; inspect motor/link status')
                except OSError as error:print(f'STOP transport failure: {error}')
            record('disconnected','end')
    return samples


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('host');p.add_argument('--port',type=int,default=3333)
    p.add_argument('--seconds',type=float,default=10);p.add_argument('--command',action='append',default=[])
    p.add_argument('--output',help='New raw JSONL file; all UWB, DATA2, commands and ACKs')
    a=p.parse_args()
    if not 0<a.seconds<86400:p.error('seconds must be 0..86400')
    for c in a.command:
        if not c.split():p.error('empty command')
        if c.split()[0] not in ('M','STOP','MOTOR_STATUS','STREAM','STREAM_STATUS','LINK_STATUS'):p.error('unsupported command')
        validate(c)
    f=open(a.output,'x',buffering=1) if a.output else None
    try:
        samples=run(a.host,a.port,a.seconds,a.command,f)
        if len(samples)>1:
            unique=len(set(s[1] for s in samples));hz=(len(samples)-1)/(samples[-1][0]-samples[0][0])
            print(f'DATA2 received={len(samples)} unique_sequences={unique} rate={hz:.2f} Hz max_source_age={max(s[2] for s in samples)} ms')
        else:print(f'DATA2 received={len(samples)}; no rate estimate')
    except (OSError,ValueError) as error:p.exit(1,str(error)+'\n')
    finally:
        if f:f.close()

if __name__=='__main__':main()
