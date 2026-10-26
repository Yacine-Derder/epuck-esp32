#include "mul_clock_fit.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MS INT64_C(63897600)
static int64_t scale(int64_t t,int32_t ppb){return t+(t/1000000000)*ppb+(t%1000000000)*ppb/1000000000;}
static void packet(mulTestPacket_t *p,uint32_t seq,int64_t time){*p=(mulTestPacket_t){.sequence=seq,.txTimestamp=mulTimestampMask(time)};memcpy(p->source,mulDefaultNetwork.addresses[0],8);}
static void stationary(unsigned period,int32_t ppb,unsigned window) {
 mulClock_t median,reg;mulClockReset(&median);mulClockReset(&reg);mulClockFit_t fit;mulClockFitReset(&fit);
 mulClockFitConfig_t c;mulClockFitDefaults(&c);c.estimator=1;c.periodMs=period;c.window=window;
 int64_t medianError=0,fitError=0;unsigned compared=0;uint32_t noise=1;
 for(unsigned i=0;i<2000;i++) {
  if(period<2000&&i%37==13)continue;
  int64_t t=(int64_t)i*period*MS;noise=noise*1664525u+1013904223u;
  int64_t jitter=(int)(noise%61)-30;bool outlier=i==1000;
  mulTestPacket_t p;packet(&p,UINT32_MAX-8+i,MUL_TIMESTAMP_MASK-300*MS+t);
  uint64_t rx=mulTimestampMask(900000+scale(t,ppb)+jitter+(outlier?100000:0));uint32_t now=UINT32_MAX-5000+i*period;
  mulClockObservePeriodNetwork(NULL,&median,&p,rx,now,true,period);
  mulClockResult_t result=mulClockObserveFitted(NULL,&reg,&fit,&c,&p,rx,now,true,NULL);
  if(outlier&&fit.ready){assert(result==mulClockIgnored&&fit.rejected&&reg.status!=mulClockValid);assert(reg.sequence==p.sequence);}
  if(i>100&&fit.ready&&!outlier&&reg.status==mulClockValid){medianError+=abs(median.skewPpb-ppb);fitError+=abs(reg.skewPpb-ppb);compared++;assert(abs(reg.skewPpb-ppb)<=3);}
 }
 fprintf(stderr,"case p=%u ppb=%ld count=%u rejected=%lu compared=%u ready=%u resets=%lu\n",period,(long)ppb,fit.count,(unsigned long)fit.rejected,compared,fit.ready,(unsigned long)fit.resets);
 assert(compared>1500&&fit.count==window&&fit.rejected);
 assert(fitError<=medianError);printf("period=%u skew=%ld window=%u median_abs_sum=%lld fit_abs_sum=%lld rejected=%lu rms_dtu=%lu size=%zu\n",period,(long)ppb,window,(long long)medianError,(long long)fitError,(unsigned long)fit.rejected,(unsigned long)fit.rmsDtu,sizeof(fit));
 // Invalid/wrong-source/replayed observations cannot mutate fit state.
 mulClockFit_t old=fit;mulTestPacket_t bad;packet(&bad,reg.sequence,reg.referenceTx);
 assert(mulClockObserveFitted(NULL,&reg,&fit,&c,&bad,reg.localRx,reg.observedMs,true,NULL)==mulClockIgnored);
 assert(!memcmp(&old,&fit,sizeof(fit)));
}
static void mobile(void) {
 mulClock_t raw,corrected;mulClockReset(&raw);mulClockReset(&corrected);mulClockFit_t a,b;mulClockFitReset(&a);mulClockFitReset(&b);
 mulClockFitConfig_t c;mulClockFitDefaults(&c);c.estimator=1;
 // Path grows by64DTU/s (~0.3m/s), deliberately distinguishable from oscillator.
 for(unsigned i=0;i<80;i++) {
  int64_t t=(int64_t)i*1000*MS,delay=1000+64*i;mulTestPacket_t p;packet(&p,i,100000+t);
  uint64_t rx=mulTimestampMask(800000+scale(t+delay,12000));uint32_t now=10000+i*1000;
  c.compensate=0;mulClockObserveFitted(NULL,&raw,&a,&c,&p,rx,now,true,NULL);
  mulClockDelay_t d={.epoch=corrected.epoch,.oneWayDtu=delay,.referenceSequence=i,.measuredMs=now,.maxAgeMs=1000,.valid=true};
  memcpy(d.reference,mulDefaultNetwork.addresses[0],8);memcpy(d.follower,mulDefaultNetwork.addresses[1],8);
  c.compensate=1;mulClockObserveFitted(NULL,&corrected,&b,&c,&p,rx,now,true,&d);
 }
 assert(a.ready&&b.ready&&raw.skewPpb>corrected.skewPpb&&abs(corrected.skewPpb-12000)<=1);
 assert(corrected.localRx==mulTimestampMask(800000+scale(79*1000*MS+1000+64*79,12000))); // Raw mapping origin retained.
 mulTestPacket_t p;packet(&p,80,100000+80*1000*MS);
 mulClockObserveFitted(NULL,&corrected,&b,&c,&p,mulTimestampMask(800000+scale(80*1000*MS+1000+64*80,12000)),90000,true,NULL);
 assert(!b.compensated&&!b.ready&&b.count==1); // Stale/missing provider never remains implicitly active.
}
static void transient(void) {
 mulClock_t clock;mulClockReset(&clock);mulClockFit_t fit;mulClockFitReset(&fit);mulClockFitConfig_t c;mulClockFitDefaults(&c);c.estimator=1;
 for(unsigned i=0;i<100;i++) {
  int64_t t=(int64_t)i*1000*MS,rx=scale(t,10000)+(i>=50?scale((int64_t)(i-50)*1000*MS,100)-((int64_t)(i-50)*1000*MS):0);
  mulTestPacket_t p;packet(&p,i,100000+t);mulClockObserveFitted(NULL,&clock,&fit,&c,&p,mulTimestampMask(900000+rx),10000+i*1000,true,NULL);
 }
 assert(fit.ready&&abs(clock.skewPpb-10100)<=1&&fit.rejected>=3&&fit.resets>=2);
 mulTestPacket_t p;packet(&p,1,1000);uint64_t epoch=clock.epoch;
 assert(mulClockObserveFitted(NULL,&clock,&fit,&c,&p,2000,120000,true,NULL)==mulClockIgnored);
 packet(&p,101,1000);assert(mulClockObserveFitted(NULL,&clock,&fit,&c,&p,2000,120000,true,NULL)==mulClockDiscontinuity);assert(clock.epoch!=epoch&&fit.count==1);
}
static void changing_rate_and_initial_outlier(void) {
 mulClock_t clock;mulClockReset(&clock);mulClockFit_t fit;mulClockFitReset(&fit);mulClockFitConfig_t c;mulClockFitDefaults(&c);c.estimator=1;
 unsigned fallback=0;
 for(unsigned i=0;i<150;i++) {
  int64_t t=(int64_t)i*1000*MS;
  // Instantaneous skew is15000+2*i ppb; integrated phase is15000*t + i^2 seconds*1ppb.
  int64_t rx=scale(t,15000)+(int64_t)i*i*MS/1000000;
  mulTestPacket_t p;packet(&p,i,100000+t);
  mulClockObserveFitted(NULL,&clock,&fit,&c,&p,mulTimestampMask(900000+rx),10000+i*1000,true,NULL);
  if(i>40&&fit.fallback==mulClockFitChangingRate){fallback++;assert(!fit.applied);assert(abs(clock.skewPpb-(15000+2*(int)i))<=6);}
 }
 assert(fallback>80&&fit.ready);
 mulClockReset(&clock);mulClockFitReset(&fit);
 for(unsigned i=0;i<40;i++) {
  int64_t t=(int64_t)i*1000*MS;mulTestPacket_t p;packet(&p,i,100000+t);
  mulClockObserveFitted(NULL,&clock,&fit,&c,&p,mulTimestampMask(900000+scale(t,-15000)+(i==2?20000:0)),10000+i*1000,true,NULL);
  if(i>=12)assert(fit.ready&&abs(clock.skewPpb+15000)<=1);
 }
}
int main(void) {
 const unsigned periods[]={100,500,1000,2000};
 for(unsigned i=0;i<4;i++) {
  unsigned p=periods[i];stationary(p,15000,24);stationary(p,-45000,32);stationary(p,150000,8);
 }
 mobile();transient();changing_rate_and_initial_outlier();puts("Clock fit: centered bounded integers, long-run rollover/gaps, median fallback, outliers/transients, period variants and mobile propagation compensation passed");return 0;
}
