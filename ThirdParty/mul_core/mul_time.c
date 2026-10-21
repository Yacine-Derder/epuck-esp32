/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_time.h"
#include <math.h>
bool mulClockSame(mulClockDomain_t a,mulClockDomain_t b){return a.node==b.node&&a.radio==b.radio&&a.epoch==b.epoch&&a.epoch;}
static bool event(mulTimeEvent_t e){return e.valid&&e.domain.epoch&&e.domain.radio>=1&&e.domain.radio<=2&&e.value<=MUL_TIMESTAMP_MASK;}
bool mulTimeDelta(mulTimeEvent_t later,mulTimeEvent_t earlier,int64_t *delta){
 if(!delta||!event(later)||!event(earlier)||!mulClockSame(later.domain,earlier.domain))return false;
 int64_t d=mulTimestampDifference(later.value,earlier.value);if(d<=0||(uint64_t)d>MUL_TIME_MAX_DELTA)return false;*delta=d;return true;
}
bool mulArrivalUpdate(mulArrivalModel_t *m,mulTimeEvent_t tx,mulTimeEvent_t rx){
 if(!m||!event(tx)||!event(rx))return false;
 if(!m->seen||!mulClockSame(m->source,tx.domain)||!mulClockSame(m->target,rx.domain)){
  *m=(mulArrivalModel_t){.source=tx.domain,.target=rx.domain,.sourceAnchor=tx.value,.targetAnchor=rx.value,.rate=1,.samples=1,.seen=true,.version=m->version+1};return true;
 }
 int64_t dt=mulTimestampDifference(tx.value,m->sourceAnchor),dr=mulTimestampDifference(rx.value,m->targetAnchor);
 if(dt<=0||dr<=0||(uint64_t)dt>MUL_TIME_MAX_DELTA||(uint64_t)dr>MUL_TIME_MAX_DELTA){m->rejected++;return false;}
 double rate=(double)dr/(double)dt,residual=(double)dr-m->rate*(double)dt;
 if(!isfinite(rate)||rate<.999||rate>1.001||(m->valid&&fabs(residual)>12780.0)){m->rejected++;return false;}
 m->rate=m->samples==1?rate:m->rate+(rate-m->rate)/8;
 m->residualDtu=residual;m->sourceAnchor=tx.value;m->targetAnchor=rx.value;m->samples++;m->valid=m->samples>=3;m->version++;return true;
}
bool mulArrivalMap(const mulArrivalModel_t *m,mulTimeEvent_t tx,uint64_t *arrival){
 if(!m||!arrival||!m->valid||!event(tx)||!mulClockSame(m->source,tx.domain))return false;
 int64_t dt=mulTimestampDifference(tx.value,m->sourceAnchor);if(dt<0||(uint64_t)dt>MUL_TIME_MAX_DELTA)return false;
 *arrival=mulTimestampMask(m->targetAnchor+(uint64_t)llround(m->rate*(double)dt));return true;
}
