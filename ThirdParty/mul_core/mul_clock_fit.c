/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_clock_fit.h"
#include <string.h>
#define SCALE INT64_C(65536)
#define BILLION INT64_C(1000000000)
static uint32_t sat(uint32_t n){return n==UINT32_MAX?n:n+1;}
static int64_t divide(int64_t n,int64_t d){return (n+(n<0?-d/2:d/2))/d;}
static uint64_t magnitude(int64_t v){return (uint64_t)(v<0?-v:v);}
static uint32_t root(uint64_t x) {
 uint64_t r=0,bit=UINT64_C(1)<<62;
 while(bit>x)bit>>=2;
 while(bit){if(x>=r+bit){x-=r+bit;r=(r>>1)+bit;}else r>>=1;bit>>=2;}
 return (uint32_t)r;
}
void mulClockFitDefaults(mulClockFitConfig_t *c){*c=(mulClockFitConfig_t){.periodMs=1000,.gateDtu=256,.window=24,.minSamples=8};}
bool mulClockFitConfigValid(const mulClockFitConfig_t *c) {
 return c&&c->estimator<=1&&c->periodMs>=10&&c->periodMs<=2000&&c->gateDtu>=16&&
  c->window>=8&&c->window<=32&&c->minSamples>=6&&c->minSamples<=c->window&&c->compensate<=1;
}
void mulClockFitReset(mulClockFit_t *s){memset(s,0,sizeof(*s));s->uncertaintyPpb=UINT32_MAX;}
/* Compute sxy/sxx inQ24 via bounded binary division; no128-bit type, FPU,
 * numerator*1e9 overflow, or double-library dependency on Cortex-M0. */
static bool slope(int64_t xy,int64_t xx,int32_t *out) {
 if(xx<=0)return false;
 uint64_t n=magnitude(xy),d=(uint64_t)xx,q=n/d,r=n%d;
 if(q>32)return false;
 for(unsigned bit=0;bit<24;bit++){r*=2;q=q*2+(r>=d);if(r>=d)r-=d;}
 int64_t ppb=divide((int64_t)q*BILLION,SCALE*(INT64_C(1)<<24));
 if(ppb>MUL_CLOCK_MAX_PPB)return false;
 *out=(int32_t)(xy<0?-ppb:ppb);return true;
}
static int64_t residual(const mulClockFit_t *s,unsigned i) {
 return (int64_t)s->y[i]-s->centerY-divide(((int64_t)s->x[i]-s->centerX)*s->skewPpb*SCALE,BILLION);
}
static bool solve(mulClockFit_t *s,uint32_t mask) {
 int64_t sx=0,sy=0;unsigned n=0;
 for(unsigned i=0;i<s->count;i++)if(mask&(UINT32_C(1)<<i)){sx+=s->x[i];sy+=s->y[i];n++;}
 if(n<6)return false;
 s->centerX=(int32_t)divide(sx,n);s->centerY=(int32_t)divide(sy,n);
 int64_t xx=0,xy=0;
 for(unsigned i=0;i<s->count;i++)if(mask&(UINT32_C(1)<<i)) {
  int64_t dx=(int64_t)s->x[i]-s->centerX,dy=(int64_t)s->y[i]-s->centerY;xx+=dx*dx;xy+=dx*dy;
 }
 if(!slope(xy,xx,&s->skewPpb))return false;
 uint64_t squares=0;
 for(unsigned i=0;i<s->count;i++)if(mask&(UINT32_C(1)<<i)) {
  uint64_t r=magnitude(residual(s,i));if(r>(UINT64_C(1)<<28))return false;squares+=r*r;
 }
 s->inliers=(uint8_t)n;s->rmsDtu=root(squares/n);
 uint32_t spread=root((uint64_t)xx);
 /* Conditional fit-scatter diagnostic, NOT total oscillator/geometry uncertainty. */
 s->uncertaintyPpb=spread?(uint32_t)(((uint64_t)s->rmsDtu*1000000000+spread*SCALE-1)/(spread*SCALE)):UINT32_MAX;
 return true;
}
static bool fit(mulClockFit_t *s,const mulClockFitConfig_t *c) {
 uint32_t all=s->count==32?UINT32_MAX:(UINT32_C(1)<<s->count)-1;
 if(s->count<c->minSamples||!solve(s,all))return false;
 uint32_t distances[32];
 for(unsigned i=0;i<s->count;i++){uint64_t r=magnitude(residual(s,i));distances[i]=r>UINT32_MAX?UINT32_MAX:(uint32_t)r;}
 for(unsigned i=1;i<s->count;i++){uint32_t v=distances[i];unsigned j=i;while(j&&distances[j-1]>v){distances[j]=distances[j-1];j--;}distances[j]=v;}
 uint64_t gate=6*(uint64_t)distances[s->count/2];if(gate<c->gateDtu)gate=c->gateDtu;
 uint32_t keep=0;
 for(unsigned i=0;i<s->count;i++)if(magnitude(residual(s,i))<=gate)keep|=UINT32_C(1)<<i;
 return solve(s,keep)&&s->inliers>=c->minSamples;
}
static void seed(mulClockFit_t *s,uint64_t epoch,uint64_t tx,uint64_t rx,bool corrected) {
 uint32_t rejected=s->rejected,accepted=s->accepted,resets=s->resets;
 mulClockFitReset(s);s->rejected=rejected;s->accepted=accepted;s->resets=sat(resets);
 s->epoch=epoch;s->lastTx=tx;s->lastRx=rx;s->count=1;s->next=1;s->compensated=corrected;
}
mulClockResult_t mulClockObserveFitted(const mulNetwork_t *network,mulClock_t *clock,
 mulClockFit_t *s,const mulClockFitConfig_t *c,const mulTestPacket_t *p,uint64_t rx,
 uint32_t now,bool valid,const mulClockDelay_t *delay) {
 if(!clock||!s||!mulClockFitConfigValid(c))return mulClockIgnored;
 mulClock_t previous=*clock;
 mulClockResult_t result=mulClockObservePeriodNetwork(network,clock,p,rx,now,valid,c->periodMs);
 if(result==mulClockIgnored||c->estimator==mulClockEstimatorMedian)return result;
 bool corrected=c->compensate&&mulClockDelayStatusNetwork(network,clock,delay,now)==mulClockDelayValid;
 uint64_t fittedRx=rx;
 if(corrected)fittedRx=mulTimestampMask(rx-delay->oneWayDtu-divide(delay->oneWayDtu*previous.skewPpb,BILLION));
 if(result==mulClockDiscontinuity||s->epoch!=clock->epoch||!s->count||s->compensated!=corrected||s->count>c->window) {
  seed(s,clock->epoch,p->txTimestamp,fittedRx,corrected);return result;
 }
 int64_t dt=mulTimestampDifference(p->txTimestamp,s->lastTx),dr=mulTimestampDifference(fittedRx,s->lastRx);
 if(dt<=0||dr<=0||dt>INT64_C(198082560000)) {seed(s,clock->epoch,p->txTimestamp,fittedRx,corrected);return result;}
 int64_t phase=s->phase+dr-dt;uint64_t elapsed=s->elapsed+(uint64_t)dt;
 if(elapsed>UINT64_C(7667712000000)||magnitude(phase)>INT32_MAX) {seed(s,clock->epoch,p->txTimestamp,fittedRx,corrected);return result;}
 int64_t x=(int64_t)(elapsed/SCALE);
 if(s->ready) {
  int64_t error=phase-s->centerY-divide((x-s->centerX)*s->skewPpb*SCALE,BILLION);
  uint64_t gate=6*(uint64_t)s->rmsDtu;if(gate<c->gateDtu)gate=c->gateDtu;
  if(magnitude(error)>gate) {
   s->rejected=sat(s->rejected);s->consecutive++;s->applied=0;s->fallback=mulClockFitOutlier;
   if(s->consecutive<3){clock->skewPpb=previous.skewPpb;clock->status=mulClockWarmup;return mulClockIgnored;}
   // A persistent oscillator/propagation transient must recover rather than lock out.
   seed(s,clock->epoch,p->txTimestamp,fittedRx,corrected);return result;
  }
 }
 s->consecutive=0;
 if(s->count==c->window) {
  unsigned oldest=(s->next+1)%c->window;int32_t bx=s->x[oldest],by=s->y[oldest];
  for(unsigned i=0;i<s->count;i++){s->x[i]-=bx;s->y[i]-=by;}
  elapsed-=(uint64_t)bx*SCALE;phase-=by;x-=bx;
 }else s->count++;
 s->x[s->next]=(int32_t)x;s->y[s->next]=(int32_t)phase;s->next=(s->next+1)%c->window;
 s->elapsed=elapsed;s->phase=phase;s->lastTx=p->txTimestamp;s->lastRx=fittedRx;s->accepted=sat(s->accepted);
 s->ready=fit(s,c);
 s->applied=0;s->fallback=mulClockFitWarmup;
 if(s->ready) {
  // A long linear window estimates its center frequency. Detect disagreement
  // with the robust recent interval rate before applying it to a drifting clock.
  uint32_t deviations[MUL_CLOCK_HISTORY];
  for(unsigned i=0;i<clock->count;i++)deviations[i]=(uint32_t)magnitude((int64_t)clock->history[i]-clock->skewPpb);
  for(unsigned i=1;i<clock->count;i++){uint32_t v=deviations[i];unsigned j=i;while(j&&deviations[j-1]>v){deviations[j]=deviations[j-1];j--;}deviations[j]=v;}
  uint64_t scatter=clock->count?deviations[clock->count/2]:UINT32_MAX;
  if(scatter<s->uncertaintyPpb)scatter=s->uncertaintyPpb;
  uint64_t threshold=6*scatter;if(threshold<10)threshold=10;
  if(clock->status==mulClockValid&&magnitude((int64_t)clock->skewPpb-s->skewPpb)>threshold)s->fallback=mulClockFitChangingRate;
  else {clock->skewPpb=s->skewPpb;clock->status=mulClockValid;s->applied=1;s->fallback=mulClockFitUsed;}
 }
 return result;
}

bool mulClockFitConfigEncode(uint8_t out[MUL_CLOCK_CONFIG_BYTES],const mulClockFitConfig_t *c) {
 if(!out||!mulClockFitConfigValid(c))return false;
 uint8_t b[MUL_CLOCK_CONFIG_BYTES]={1,c->estimator,c->window,c->minSamples,c->periodMs&255,c->periodMs>>8,c->gateDtu&255,c->gateDtu>>8,c->compensate,0,0,0};
 memcpy(out,b,sizeof(b));return true;
}
bool mulClockFitConfigDecode(const uint8_t in[MUL_CLOCK_CONFIG_BYTES],mulClockFitConfig_t *c) {
 if(!in||!c||in[0]!=1||in[9]||in[10]||in[11])return false;
 mulClockFitConfig_t candidate={.estimator=in[1],.window=in[2],.minSamples=in[3],.periodMs=(uint16_t)(in[4]|in[5]<<8),.gateDtu=(uint16_t)(in[6]|in[7]<<8),.compensate=in[8]};
 if(!mulClockFitConfigValid(&candidate))return false;
 *c=candidate;return true;
}
