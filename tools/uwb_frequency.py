#!/usr/bin/env python3
"""Decode optional STM32 raw frequency snapshots or calculate explicit scenarios.
Conversions label bundled API algebra, NOT validated oscillator/Doppler estimates.
"""
import argparse
import json
import math
from pathlib import Path
import re

C=299792458.0
CHANNEL5_HZ=6489600000.0
CI_API_HZ_PER_COUNT=998.4e6/2/1024/131072
COE_API_RATIO_PER_COUNT=2.0**-26
FIELDS={'slot','peer','type','seq','rx','observed_ms','age_ms','ci_raw','coe_raw','coe_present','status','metric_rate'}
SCENARIO={'radial_velocity_error_m_s','frequency_error_hz','calibration_error_ppb',
          'clock_slew_ppb_s','hold_seconds','speed_m_s'}

def decode(line):
    pos=line.find('MULH FREQ ')
    if pos<0:return None
    line=line[pos+10:].strip()
    if line.startswith('disabled;'):
        return dict(kind='frequency_unavailable',reason='diagnostic build disabled',metric_rate_valid=False)
    absent=re.fullmatch(r'slot=([012]) absent',line)
    if absent:return dict(kind='frequency_unavailable',slot=int(absent[1]),reason='no captured frame',metric_rate_valid=False)
    parts=line.split();fields={}
    for part in parts:
        if part.count('=')!=1:raise ValueError('malformed frequency snapshot')
        key,value=part.split('=')
        if key in fields:raise ValueError('duplicate field')
        fields[key]=value
    if set(fields)!=FIELDS or fields['metric_rate']!='UNKNOWN':raise ValueError('unrecognized raw-only frequency schema')
    try:
        slot=int(fields['slot']);peer=int(fields['peer'],16);packet_type=int(fields['type'],16)
        seq=int(fields['seq']);rx=int(fields['rx'],16);now=int(fields['observed_ms']);age=int(fields['age_ms'])
        ci=int(fields['ci_raw']);coe=int(fields['coe_raw']);present=int(fields['coe_present']);status=int(fields['status'],16)
    except ValueError as error:raise ValueError('invalid integer field') from error
    if not (0<=slot<3 and 0<=peer<256 and 0<=packet_type<256 and 0<=seq<1<<32
            and 0<=rx<1<<40 and 0<=now<1<<32 and 0<=age<=65535 and 0<=status<1<<32
            and -(1<<20)<=ci<1<<20 and -4096<=coe<=4095 and present in (0,1)):
        raise ValueError('frequency field width/range')
    if not present and coe:raise ValueError('unavailable CIA offset must not masquerade as a sample')
    return dict(kind='hybrid_frequency_raw',slot=slot,peer=peer,packet_type=packet_type,transaction=seq,
                rx=f'{rx:010X}',observed_ms=now,capture_age_ms=age,status=f'{status:08X}',
                ci_raw=ci,coe_raw=coe if present else None,coe_present=bool(present),
                ci_api_offset_hz=ci*CI_API_HZ_PER_COUNT,
                ci_api_offset_ppb=-ci*CI_API_HZ_PER_COUNT/CHANNEL5_HZ*1e9,
                coe_api_scaled_ppb=coe*COE_API_RATIO_PER_COUNT*1e9 if present else None,
                sign_convention='raw_and_bundled_api_algebra_not_hardware_verified',
                nominal_channel_hz=CHANNEL5_HZ,oscillator_rate_identified=False,
                metric_rate_valid=False,measurement=None)

def sensitivity(scenario,slot_ms):
    if not isinstance(scenario,dict) or set(scenario)!=SCENARIO:
        raise ValueError('all six explicit scenario parameters required')
    try:valid=all(type(v) in (int,float) and math.isfinite(v) and v>=0 for v in scenario.values())
    except OverflowError:valid=False
    if not valid or slot_ms not in (500,50,10):raise ValueError('finite nonnegative scenario and supported slot required')
    scenario={key:float(value) for key,value in scenario.items()}
    s=slot_ms/1000
    doppler_ppb=scenario['radial_velocity_error_m_s']/C*1e9
    frequency_ppb=scenario['frequency_error_hz']/CHANNEL5_HZ*1e9
    hold_ppb=scenario['calibration_error_ppb']+scenario['clock_slew_ppb_s']*scenario['hold_seconds']
    out=dict(kind='frequency_sensitivity_scenario',status='synthetic_or_user_assumed_not_hardware_bound',
             slot_ms=slot_ms,assumptions=scenario,metric_rate_valid=False,measurement=None,
             doppler_residual_ppb=doppler_ppb,doppler_equivalent_range_error_m=C*s*doppler_ppb*1e-9,
             frequency_error_ppb=frequency_ppb,frequency_equivalent_range_error_m=C*s*frequency_ppb*1e-9,
             stationary_hold_rate_error_ppb=hold_ppb,stationary_hold_equivalent_range_error_m=C*s*hold_ppb*1e-9,
             cross_time_motion_bound_m=scenario['speed_m_s']*s,
             ci_api_one_count_ppb=CI_API_HZ_PER_COUNT/CHANNEL5_HZ*1e9,
             ci_api_one_count_equivalent_range_m=s*C*CI_API_HZ_PER_COUNT/CHANNEL5_HZ,
             coe_api_one_count_ppb=COE_API_RATIO_PER_COUNT*1e9,
             coe_api_one_count_equivalent_range_m=s*C*COE_API_RATIO_PER_COUNT)
    if any(isinstance(v,float) and not math.isfinite(v) for v in out.values()):raise ValueError('scenario arithmetic overflow')
    return out

def main():
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='action',required=True)
    d=sub.add_parser('decode');d.add_argument('logs',nargs='+',type=Path);d.add_argument('--output',required=True,type=Path)
    s=sub.add_parser('sensitivity');s.add_argument('--scenario',required=True,type=Path)
    s.add_argument('--slot-ms',type=int,choices=(500,50,10),action='append')
    a=p.parse_args()
    if a.action=='sensitivity':
        try:
            scenario=json.loads(a.scenario.read_text())
            for slot in a.slot_ms or (500,50,10):print(json.dumps(sensitivity(scenario,slot),allow_nan=False,sort_keys=True))
        except (OSError,ValueError) as error:p.error(str(error))
        return
    with a.output.open('x') as output:
        for path in a.logs:
            for line in path.read_text().splitlines():
                try:row=decode(line)
                except ValueError as error:row=dict(kind='invalid_frequency_snapshot',reason=str(error),metric_rate_valid=False)
                if row is not None:output.write(json.dumps(row,allow_nan=False,sort_keys=True)+'\n')
if __name__=='__main__':main()
