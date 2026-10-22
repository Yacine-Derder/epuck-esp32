#!/usr/bin/env python3
"""Radio-independent fixed-anchor 2D multilateration, standard Python only.

Inputs are explicit batches of valid metre ranges/range differences, never raw
independent-clock timestamps or inter-anchor baselines. Uncertainty is not inferred
from residual scatter. Non-identifiable, ambiguous or inconsistent batches fail closed.
"""
import argparse
import json
import math
from pathlib import Path

def identity(value):
    try:return str(int(str(value),0))
    except ValueError:return str(value)

def load_geometry(path):
    spec=json.loads(Path(path).read_text());spec['anchors']={identity(k):v for k,v in spec['anchors'].items()};return spec

def solve_2d(anchors,measurements,bounds=None,max_residual_m=.25):
    failure=lambda reason:dict(status=reason,position=None,physical_uncertainty='unknown')
    try:
        positions={identity(k):(float(v[0]),float(v[1])) for k,v in anchors.items()}
        if any(not math.isfinite(c) for v in positions.values() for c in v):return failure('invalid_geometry')
        if bounds is not None:
            bounds=tuple(float(v) for v in bounds)
            if len(bounds)!=4 or not all(math.isfinite(v) for v in bounds) or bounds[0]>=bounds[1] or bounds[2]>=bounds[3]:return failure('invalid_bounds')
        equations=[];used=set();seen={}
        for m in measurements:
            if not m.get('valid',False):continue
            kind=m['kind'];anchor=identity(m['anchor']);value=float(m['value_m']);sigma=float(m.get('sigma_m',1))
            if kind not in ('range','range_difference') or anchor not in positions or not math.isfinite(value) or not math.isfinite(sigma) or sigma<=0:return failure('invalid_measurement')
            ref=identity(m['reference']) if kind=='range_difference' else None
            if ref is not None and (ref not in positions or ref==anchor):return failure('invalid_measurement')
            if kind=='range' and value<0:return failure('invalid_measurement')
            if ref is not None and abs(value)>math.dist(positions[anchor],positions[ref])+max_residual_m:return failure('impossible_range_difference')
            if ref is not None and anchor<ref:anchor,ref,value=ref,anchor,-value
            key=(kind,anchor,ref)
            if key in seen:
                if abs(seen[key]-value)>1e-9:return failure('conflicting_measurements')
                continue
            seen[key]=value
            equations.append((kind,anchor,ref,value,sigma));used.add(anchor)
            if ref is not None:used.add(ref)
        if len(equations)<2 or len(used)<3:return failure('insufficient_geometry')
        # Rank of constraints on per-anchor distances. A closed TDoA triangle
        # adds no information beyond two differences; duplicates are not geometry.
        parent={k:k for k in used}
        def find(k):
            while parent[k]!=k:k=parent[k]
            return k
        for kind,anchor,ref,_,_ in equations:
            if ref is not None:parent[find(anchor)]=find(ref)
        roots={find(k) for k in used};absolute={find(anchor) for kind,anchor,ref,_,_ in equations if kind=='range'}
        independent=len(used)-len(roots-absolute)
        if independent<3 and bounds is None:return failure('insufficient_constraints_without_bounds')
        points=[positions[k] for k in used];cx=sum(p[0] for p in points)/len(points);cy=sum(p[1] for p in points)/len(points)
        xx=sum((x-cx)**2 for x,y in points);yy=sum((y-cy)**2 for x,y in points);xy=sum((x-cx)*(y-cy) for x,y in points)
        if xx+yy<=1e-12 or xx*yy-xy*xy<=1e-9*(xx+yy)**2:return failure('degenerate_geometry')
    except (KeyError,TypeError,ValueError,OverflowError):return failure('invalid_input')
    def evaluate(x,y):
        residual=[];a=b=c=g=h=cost=0.
        for kind,anchor,ref,value,sigma in equations:
            px,py=positions[anchor];dx=x-px;dy=y-py;distance=math.hypot(dx,dy);prediction=distance
            jx=dx/max(distance,1e-12);jy=dy/max(distance,1e-12)
            if ref is not None:
                px,py=positions[ref];dx=x-px;dy=y-py;distance=math.hypot(dx,dy);prediction-=distance
                jx-=dx/max(distance,1e-12);jy-=dy/max(distance,1e-12)
            r=prediction-value;residual.append(r);r/=sigma;jx/=sigma;jy/=sigma
            a+=jx*jx;b+=jx*jy;c+=jy*jy;g+=jx*r;h+=jy*r;cost+=r*r
        return cost,residual,a,b,c,g,h
    span=max(math.sqrt(xx+yy),1.)
    search=bounds or (cx-3*span,cx+3*span,cy-3*span,cy+3*span)
    def clamp(x,y):
        if bounds:return max(bounds[0],min(bounds[1],x)),max(bounds[2],min(bounds[3],y))
        return x,y
    solutions=[]
    for ix in range(5):
        for iy in range(5):
            x=search[0]+(search[1]-search[0])*(ix+.5)/5;y=search[2]+(search[3]-search[2])*(iy+.5)/5;damping=1e-3
            for iteration in range(120):
                cost,_,a,b,c,g,h=evaluate(x,y);aa=a+damping;cc=c+damping;det=aa*cc-b*b
                if det<=0 or not all(math.isfinite(v) for v in (cost,a,b,c,g,h,det)):break
                dx=(-cc*g+b*h)/det;dy=(b*g-aa*h)/det
                # Bound each numerical step; no finite anchor uncertainty model is implied.
                size=math.hypot(dx,dy)
                if size>span:dx*=span/size;dy*=span/size
                tx,ty=clamp(x+dx,y+dy);candidate=evaluate(tx,ty)[0]
                if candidate<cost:
                    x,y=tx,ty;damping=max(1e-12,damping/3)
                    if size<1e-8:break
                else:
                    damping*=10
                    if damping>1e12:break
            result=evaluate(x,y);cost,residual,a,b,c,_,_=result;det=a*c-b*b
            if not all(math.isfinite(v) for v in (cost,x,y,a,b,c,det)) or det<=1e-9*(a+c)**2:continue
            if not any(math.hypot(x-s[1],y-s[2])<1e-4 for s in solutions):solutions.append((cost,x,y,residual,a,b,c))
    if not solutions:return failure('singular_solution')
    solutions.sort();cost,x,y,residual,a,b,c=solutions[0];rms=math.sqrt(sum(r*r for r in residual)/len(residual))
    if rms>max_residual_m:return dict(**failure('inconsistent_measurements'),rms_m=rms,residuals_m=residual)
    similar=[s for s in solutions[1:] if s[0]<=cost+max(1e-8,cost*.01) and math.hypot(x-s[1],y-s[2])>1e-3]
    if similar:return dict(**failure('ambiguous_geometry'),candidate_count=len(similar)+1)
    trace=a+c;root=math.sqrt((a-c)**2+4*b*b);condition=(trace+root)/(trace-root)
    return dict(status='available',position=dict(x=x,y=y),rms_m=rms,residuals_m=residual,normal_condition=condition,
                measurement_count=len(equations),independent_constraint_count=independent,anchor_count=len(used),physical_uncertainty='unknown',
                weighting='configured sigma or unit weights; residuals are not calibrated uncertainty')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('measurements',type=Path);p.add_argument('--anchors',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path);p.add_argument('--max-residual-m',type=float,default=.25);args=p.parse_args()
    if not math.isfinite(args.max_residual_m) or args.max_residual_m<=0:p.error('positive finite residual threshold required')
    geometry=load_geometry(args.anchors)
    with args.measurements.open() as stream,args.output.open('x') as out:
        for line in stream:
            try:
                batch=json.loads(line);result=solve_2d(geometry['anchors'],batch['measurements'],geometry.get('bounds'),args.max_residual_m)
                result['batch_id']=batch.get('batch_id')
            except (ValueError,KeyError,TypeError):result=dict(status='invalid_input',position=None)
            out.write(json.dumps(result,sort_keys=True)+'\n')
if __name__=='__main__':main()
