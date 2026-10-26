#include "mul_telemetry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv) {
 (void)argv;const unsigned counts[]={2,3,4,8};
 for(unsigned c=0;c<4;c++)for(unsigned stationary=0;stationary<2;stationary++) {
  mulTelemetry_t r={.type=mulTlmVector,.node=240,.radio=2,.role=2,.mode=stationary?5:4,.ratePolicy=stationary,
    .epoch=8,.uptimeMs=UINT32_MAX,.sequence=UINT32_MAX,.dropped=7,.depth=3,.highWater=9};
  mulMultiCycle_t *v=&r.vector;
  *v=(mulMultiCycle_t){.cycle=UINT32_MAX,.observedMs=UINT32_MAX,.fingerprint=0x12345678,.revision=9,
    .referenceRx=UINT64_C(0xffffffffff),.slotMs=10,.periodMs=1000,.count=counts[c]};
  for(unsigned i=0;i<v->count;i++) {
   v->ids[i]=132+3*i;v->seenMask|=1u<<i;if(!i)continue;
   mulMultiValue_t *x=&v->values[i-1];bool available=stationary&&i%2;
   *x=(mulMultiValue_t){.rangeDifferenceMm=available?(i==1?-512:512):INT32_MIN,
    .tdoaDtu=available?(i==1?-109:109):INT32_MIN,.status=available?4:0,
    .reason=available?0:stationary?1:6,.clockValid=available,.rateState=available};
   if(available)v->validMask|=1u<<i;
   else if(stationary)v->seenMask&=~(1u<<i);
  }
  char line[MUL_TLM_LINE];mulTelemetry_t decoded;
  assert(mulTelemetryEncode(line,sizeof(line),&r)&&strlen(line)==309&&!strncmp(line,"UWB3 ",5));
  assert(!mulTelemetryEncode(line,sizeof(line)-1,&r));assert(mulTelemetryDecode(line,&decoded));
  assert(decoded.vector.count==v->count&&decoded.vector.cycle==UINT32_MAX&&decoded.vector.validMask==v->validMask);
  assert(!memcmp(decoded.vector.ids,v->ids,8));
  for(unsigned i=0;i+1<v->count;i++)assert(!memcmp(&decoded.vector.values[i],&v->values[i],sizeof(mulMultiValue_t)));
  if(argc>1)puts(line);
  mulTelemetry_t bad=r;bad.vector.validMask^=1;assert(!mulTelemetryEncode(line,sizeof(line),&bad));
  bad=r;bad.vector.ids[1]=bad.vector.ids[0];assert(!mulTelemetryEncode(line,sizeof(line),&bad));
  bad=r;bad.vector.values[0].reason=10;assert(!mulTelemetryEncode(line,sizeof(line),&bad));
  bad=r;bad.vector.count=9;assert(!mulTelemetryEncode(line,sizeof(line),&bad));
  // New raw trace type carries all90 bytes without reinterpreting legacy type6.
  r.type=mulTlmMultiRx;r.legacyLength=90;r.legacyRx=UINT64_C(0xffffffffff);r.captureAgeMs=15;
  r.legacySlotMs=10;r.scheduleFingerprint=v->fingerprint;r.scheduleRevision=9;
  for(unsigned i=0;i<90;i++)r.legacyFrame[i]=(uint8_t)i;
  assert(mulTelemetryEncode(line,sizeof(line),&r)&&mulTelemetryDecode(line,&decoded));
  assert(decoded.legacyLength==90&&decoded.scheduleRevision==9&&!memcmp(decoded.legacyFrame,r.legacyFrame,90));
  r.type=mulTlmHybridRx;assert(!mulTelemetryEncode(line,sizeof(line),&r));
 }
 mulTelemetry_t clock={.type=mulTlmClock,.node=141,.radio=2,.role=2,.mode=5,.epoch=7,
 .clock={.reference=132,.referenceTx=UINT64_C(0xffffffffff),.rawRx=1,.referenceSequence=UINT32_MAX,
  .appliedPpb=-15,.fittedPpb=-30,.rmsDtu=40,.fitSigmaPpb=UINT32_MAX,.method=1,.window=24,.count=24,.inliers=22,.status=2,.ready=1,.fallback=2}};
 char line[MUL_TLM_LINE];mulTelemetry_t decoded;
 assert(mulTelemetryEncode(line,sizeof(line),&clock)&&mulTelemetryDecode(line,&decoded));
 assert(decoded.clock.referenceTx==clock.clock.referenceTx&&decoded.clock.appliedPpb==-15&&decoded.clock.fitSigmaPpb==UINT32_MAX);
 assert(decoded.clock.version==2&&decoded.captureAgeMs==0);
 clock.captureAgeMs=16;clock.transaction=200;
 assert(mulTelemetryEncode(line,sizeof(line),&clock)&&mulTelemetryDecode(line,&decoded));
 assert(decoded.captureAgeMs==16&&decoded.transaction==200&&decoded.clock.version==2);
 clock.transaction=2001;assert(!mulTelemetryEncode(line,sizeof(line),&clock));
 clock.clock.version=1; // Old producer has no capture age or sample-period semantics.
 assert(mulTelemetryEncode(line,sizeof(line),&clock)&&mulTelemetryDecode(line,&decoded));
 assert(decoded.clock.version==1&&decoded.captureAgeMs==UINT16_MAX);
 clock.clock.version=2;clock.transaction=1000;
 clock.clock.applied=1;assert(!mulTelemetryEncode(line,sizeof(line),&clock));clock.clock.fallback=0;
 assert(mulTelemetryEncode(line,sizeof(line),&clock));clock.clock.count=25;assert(!mulTelemetryEncode(line,sizeof(line),&clock));
 if(argc==1)puts("Multi telemetry: bounded152-byte vector/raw90, N2/3/4/8, mixed validity, no-rate, legacy rejection and32/40-bit wrap passed");
 return 0;
}
