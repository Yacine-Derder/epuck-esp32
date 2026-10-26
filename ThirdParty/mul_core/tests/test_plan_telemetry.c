#include "mul_telemetry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void put(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
int main(int argc,char **argv) {
 (void)argv;
 for(unsigned count=2;count<=8;count++)for(unsigned policy=0;policy<2;policy++) {
  mulTelemetry_t r={.type=mulTlmPlanVector,.quality=4,.node=141,.radio=2,.role=2,.mode=policy?5:4,.ratePolicy=policy,.epoch=9,.uptimeMs=UINT32_MAX};
  mulMultiCycle_t *v=&r.vector;*v=(mulMultiCycle_t){.cycle=UINT32_MAX,.observedMs=UINT32_MAX,.fingerprint=0x34567890,.revision=7,.slotMs=3,.periodMs=200,.count=count,.referenceRx=UINT64_C(0xffffffffff)};
  for(unsigned i=0;i<count;i++){v->ids[i]=32+i;v->seenMask|=1u<<i;if(i)v->values[i-1]=(mulMultiValue_t){.rangeDifferenceMm=INT32_MIN,.tdoaDtu=INT32_MIN,.reason=policy?1:6};}
  char line[MUL_TLM_LINE];mulTelemetry_t decoded;
  assert(mulTelemetryEncode(line,sizeof(line),&r)&&mulTelemetryDecode(line,&decoded));
  assert(decoded.type==mulTlmPlanVector&&decoded.quality==4&&decoded.vector.fingerprint==v->fingerprint&&decoded.vector.periodMs==200);
  if(argc>1)puts(line);
  r.quality=3;assert(!mulTelemetryEncode(line,sizeof(line),&r));r.quality=4;
  r.type=mulTlmVector;assert(!mulTelemetryEncode(line,sizeof(line),&r));r.type=mulTlmPlanVector;
  r.type=mulTlmPlanRx;r.legacyRx=UINT64_C(0xffffffffff);r.captureAgeMs=15;r.legacySlotMs=3;r.scheduleRevision=7;r.scheduleFingerprint=v->fingerprint;
  const unsigned lengths[]={40,80,90};
  for(unsigned k=0;k<3;k++) {
   unsigned n=lengths[k];r.legacyLength=n;memset(r.legacyFrame,0,sizeof(r.legacyFrame));
   unsigned version=n==90?84:34,hash=n==90?86:36;r.legacyFrame[version]=MUL_PLAN_RADIO_VERSION;put(r.legacyFrame+hash,r.scheduleFingerprint);
   if(n==90)r.legacyFrame[85]=31;
   assert(mulTelemetryEncode(line,sizeof(line),&r)&&mulTelemetryDecode(line,&decoded));assert(decoded.type==mulTlmPlanRx&&decoded.legacyLength==n);
   if(argc>1)puts(line);
   r.legacyFrame[version]=3;assert(!mulTelemetryEncode(line,sizeof(line),&r));r.legacyFrame[version]=4;
   r.legacyFrame[hash]^=1;assert(!mulTelemetryEncode(line,sizeof(line),&r));r.legacyFrame[hash]^=1;
   if(n==90){r.legacyFrame[85]=32;assert(!mulTelemetryEncode(line,sizeof(line),&r));r.legacyFrame[85]=31;}
   r.type=mulTlmMultiRx;assert(!mulTelemetryEncode(line,sizeof(line),&r));r.type=mulTlmPlanRx;
  }
 }
 if(argc==1)puts("Plan telemetry11/12: strict profile4, numeric timing, raw40/80/90 fingerprint binding, allN2..8, policy, wrap and old-type rejection passed");
 return 0;
}
