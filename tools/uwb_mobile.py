#!/usr/bin/env python3
"""Conditional passive mobile analysis. No hardware access or true-clock claim.
Shared C replay must first validate association and the transported ALR relation.
A next-reference bracket is required; no stationary fit/extrapolation is reused.
"""
import argparse
import json
import math
from pathlib import Path
import sys

C=299792458.0
HZ=63897600000
MASK=(1<<40)-1
REQUIRED={'max_speed_m_s','max_acceleration_m_s2','min_reference_distance_m',
          'relative_clock_slew_ppb_s','oscillator_ppm_bound','baseline_max_m',
          'metrology_error_m'}

def assumptions(values):
    if not isinstance(values,dict) or set(values)!=REQUIRED:
        raise ValueError('all explicit motion/clock/baseline/metrology assumptions required')
    try:finite=all(type(v) in (int,float) and math.isfinite(v) for v in values.values())
    except OverflowError:finite=False
    if not finite:raise ValueError('assumptions must be finite numbers, not booleans')
    if any(v<0 for v in values.values()):raise ValueError('negative assumption')
    if not 0<values['min_reference_distance_m'] or not 0<values['baseline_max_m']:
        raise ValueError('positive minimum reference distance and maximum baseline required')
    if values['max_speed_m_s']>1000 or values['oscillator_ppm_bound']>1000:
        raise ValueError('outside prototype speed/oscillator envelope')
    return dict(values)

def unavailable(reason,status='unavailable'):
    return dict(kind='conditional_mobile_range_difference',status=status,reason=reason,
                point_measurement_valid=False,measurement=None,interval_m=None,
                oscillator_rate_identified=False)

def interpolate(tx_span,rx_span,tx_query,rx_query,bounds):
    """All integer spans in their own native DTU, from one validated bracket.
    Returned interval is conditional on EVERY supplied assumption. metrology_error
    includes propagation model/antenna/multipath/relation errors, never a residual
    fit. Representation quantization is added separately, not hardware accuracy.
    """
    if bounds is None:return unavailable('explicit assumptions required','assumptions_required')
    try:b=assumptions(bounds)
    except ValueError as error:return unavailable(str(error),'invalid_assumptions')
    if any(type(v) is not int for v in (tx_span,rx_span,tx_query,rx_query)):
        return unavailable('integer native DTU required')
    if not (.9*HZ<=tx_span<=1.1*HZ and .9*HZ<=rx_span<=1.1*HZ):
        return unavailable('reference bracket must be one observed cadence')
    if not 0<tx_query<tx_span or not 0<rx_query<rx_span:
        return unavailable('no extrapolation or unbracketed reception')
    q=rx_span/tx_span
    if not .99<q<1.01:return unavailable('implausible apparent arrival slope')
    fraction=tx_query/tx_span
    residual=rx_query-fraction*rx_span
    nominal=residual/q*C/HZ
    u=b['max_speed_m_s']/C
    scale=b['oscillator_ppm_bound']*1e-6
    alpha_min=(1-scale)/(1+scale)
    alpha_max=(1+scale)/(1-scale)
    slew=b['relative_clock_slew_ppb_s']*1e-9
    radial_accel=b['max_acceleration_m_s2']+b['max_speed_m_s']**2/b['min_reference_distance_m']
    curvature=slew/(1-u)**2+alpha_max*radial_accel/(C*(1-scale)*(1-u)**3)
    before=tx_query/HZ;after=(tx_span-tx_query)/HZ
    interpolation_m=C/q*curvature*before*after/2
    # Mean-value normalization relative to simultaneous geometry. Oscillator
    # priors apply to EACH radio vs nominal SI time, not a measured confidence.
    factor_min=(1-scale)*alpha_min/(q*(1+u))
    factor_max=(1+scale)*alpha_max/(q*(1-u))
    normalization_m=b['baseline_max_m']*max(abs(factor_min-1),abs(factor_max-1))
    quantization_m=C/HZ*(2/q+2)
    numerical_m=C/HZ/q*32*sys.float_info.epsilon*(abs(rx_query)+abs(fraction*rx_span)+abs(rx_span)+1)
    error=interpolation_m+normalization_m+quantization_m+numerical_m+b['metrology_error_m']
    if not all(math.isfinite(v) for v in (nominal,error)):
        return unavailable('assumptions/arithmetic overflow','invalid_assumptions')
    lo=math.nextafter(nominal-error,-math.inf);hi=math.nextafter(nominal+error,math.inf)
    limit=b['baseline_max_m']
    if lo>limit or hi<-limit:return unavailable('inconsistent with explicit baseline/model bounds','inconsistent')
    # Interval geometry constraint, never clip or invent a point measurement.
    lo=max(lo,-limit);hi=min(hi,limit)
    return dict(kind='conditional_mobile_range_difference',status='conditional',
                nominal_estimate_m=nominal,interval_m=[lo,hi],apparent_rate_ratio=q,
                apparent_rate_ppb=(q-1)*1e9,rate_kind='reference_arrival_not_oscillator',
                oscillator_rate_identified=False,point_measurement_valid=False,
                measurement=None,epoch='follower_reception',physical_validation=False,
                uncertainty='conditional_envelope_not_calibrated',assumptions=b,
                interpolation_error_m=interpolation_m,normalization_error_m=normalization_m,
                quantization_allowance_m=quantization_m,metrology_error_m=b['metrology_error_m'])

def delta(later,earlier):
    d=(later-earlier)&MASK
    return d-(1<<40) if d&(1<<39) else d

class MobileReplay:
    """Bounded per-receiver state; C Replay owns all on-air validation/mapping."""
    def __init__(self,bounds,slot=500):
        from uwb_hybrid import Replay
        self.bounds=assumptions(bounds);self.shared=Replay(slot);self.states={}
    def close(self):self.shared.close();self.states.clear()
    def feed(self,record):
        row=self.shared.feed(record)
        if row is None:return None
        if row.get('kind')=='invalid':return unavailable(row['reason'])
        domain=row['receiver_domain'];key=(domain['node'],domain['radio'],domain['epoch'],record['rate_policy'])
        if key not in self.states:
            if len(self.states)>=128:self.states.pop(next(iter(self.states)))
            self.states[key]={}
        state=self.states[key]
        if row['replay_reset_on_captured_generation']:state.clear()
        if not row['broadcast']:return None
        if row['source_order'] in (3,4):return None # Duplicate/old: no refresh or pending-pair loss.
        if record['capture_age_ms']>15:state.clear();return unavailable('late/invalid capture')
        # Shapes/roles below have already passed actual C codecs and association.
        tx=int(row['broadcast_tx'],16);rx=int(record['rx'],16);seq=record['transaction']
        generation=row['reference_generation'];now=record['uptime_ms']
        if row['source_role']==0: # Actual shared role resolver, not a guessed ID.
            old=state.get('reference');pending=state.get('pair')
            if old:
                advance=(seq-old['seq'])&0xffffffff
                if advance==0 or advance>=1<<31:return None # No duplicate age refresh.
            result=None
            if old and pending:
                elapsed=(now-old['now'])&0xffffffff
                if (seq-old['seq'])&0xffffffff!=1 or generation!=old['generation'] or not 900<=elapsed<=1100:
                    result=unavailable('missing/stale reference bracket or generation')
                elif pending['reference_tx']!=old['tx'] or pending['reference_sequence']!=old['seq']:
                    result=unavailable('relation belongs to a different reference')
                else:
                    result=interpolate(delta(tx,old['tx']),delta(rx,old['rx']),
                                       pending['tx_separation_ref_dtu'],pending['rx_separation_local_dtu'],self.bounds)
                result.update(receiver_domain=domain,reference_sequence=old['seq'],
                              next_reference_sequence=seq,follower_sequence=pending['follower_sequence'],
                              reference_generation=generation,follower_generation=pending['follower_generation'],
                              follower_acquisition_epoch=pending['follower_acquisition_epoch'],
                              receiver_timestamp=pending['receiver_timestamp'],observed_ms=pending['observed_ms'],
                              bracket_closed_ms=now,slot_ms=row['slot_ms'],output_delayed_until_next_reference=True)
            state.clear();state['reference']=dict(tx=tx,rx=rx,seq=seq,generation=generation,now=now)
            return result
        if row['associated'] and row['clock_valid']:
            row['receiver_timestamp']=record['rx'];row['reference_tx']=int(row['reference_tx'],16)
            state['pair']=row
        else:state.pop('pair',None)
        return None

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('logs',nargs='+',type=Path);p.add_argument('--bounds',required=True,type=Path)
    p.add_argument('--slot',type=int,choices=(500,50,10),default=500)
    p.add_argument('--output',required=True,type=Path)
    args=p.parse_args()
    try:bounds=assumptions(json.loads(args.bounds.read_text()))
    except (ValueError,OSError) as error:p.error(str(error))
    replay=MobileReplay(bounds,args.slot)
    try:
        with args.output.open('x') as output:
            for path in args.logs:
                with path.open() as stream:
                    for line in stream:
                        try:record=json.loads(line);row=replay.feed(record) if isinstance(record,dict) else unavailable('JSON object required')
                        except (ValueError,TypeError,KeyError):row=unavailable('malformed capture record')
                        if row is not None:output.write(json.dumps(row,sort_keys=True,allow_nan=False)+'\n')
    finally:replay.close()
if __name__=='__main__':main()
