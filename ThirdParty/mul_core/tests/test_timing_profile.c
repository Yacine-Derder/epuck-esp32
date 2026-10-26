#include "mul_multi_receiver.h"
#include "mul_telemetry.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MS INT64_C(63897600)
static int64_t scale(int64_t t,int32_t ppb){return t+(t/1000000000)*ppb+(t%1000000000)*ppb/1000000000;}
static void synthetic(unsigned count,unsigned spacing,unsigned period) {
 mulSchedule_t s;mulScheduleLegacy(&s,132,133,10);s.count=count;
 for(unsigned i=0;i<count;i++)s.ids[i]=132+i;
 mulScheduleExperimental(&s);s.txLeadUs=500;s.rxBudgetUs=500;s.airtimeUs=400;s.slotMs=spacing;s.periodMs=period;s.rangingFlags=0;
 if(mulScheduleValidate(&s))return;
 uint8_t encoded[32];assert(mulScheduleEncode(encoded,&s)&&encoded[0]==2&&encoded[1]==3);
 mulSchedule_t decoded;assert(mulScheduleDecode(encoded,&decoded)&&mulScheduleFingerprint(&decoded)==mulScheduleFingerprint(&s));
 assert(!mulScheduleSyncOwner(&s,5,0)&&!mulScheduleSyncOwner(&s,5,1));
 mulMultiReceiver_t receiver;const uint8_t local[8]={141,0,0,0,0,0,0xcf,0xbc};assert(mulMultiReceiverInit(&receiver,&s,local,true));
 assert(receiver.clockConfig.periodMs==period);
 for(unsigned step=0;step<15;step++) {
  uint32_t cycle=100+step,now=10000+step*period;int64_t t=(int64_t)step*period*MS;
  uint64_t reference=mulTimestampMask(MUL_TIMESTAMP_MASK-200*MS+t);
  for(unsigned slot=0;slot<count;slot++) {
   int64_t offset=(int64_t)slot*spacing*MS,folOrigin=500000+scale(t,35000);
   unsigned ageCycles=2000/period;
   mulRelation_t relation={.version=2,.epoch=slot+1,.referenceSequence=cycle,.twrSequence=cycle-ageCycles,
    .referenceOrigin=reference,.followerOrigin=mulTimestampMask(folOrigin),.skewPpb=35000,
    .originAgeMs=slot*spacing+1,.delayAgeMs=ageCycles*period+slot*spacing,.delayDtu=120,.maxAgeMs=6000,
    .delayState=ageCycles<=1?mulPropagationFresh:mulPropagationReused,.available=true,.period10ms=period/10};
   uint64_t tx=slot?mulTimestampMask(folOrigin+scale(offset+120,35000)):reference;
   int64_t difference=slot%2?477:-477;
   uint64_t rx=mulTimestampMask(800000+scale(t+(slot?offset+120+difference:0)+4000,15000));
   uint8_t frame[90];size_t n=mulMultiBroadcastEncode(frame,sizeof(frame),&s,slot,cycle,tx,slot?&relation:NULL);assert(n);
   assert(frame[34]==3);mulMultiReceiveOutput_t out;
   assert(mulMultiReceiverReceive(&receiver,frame,n,rx,1,now+slot*spacing,&out));
   mulSchedule_t old=s;old.profile=MUL_PROFILE_MULTI;old.txLeadUs=old.rxBudgetUs=old.airtimeUs=old.guard100us=old.rangingFlags=0;old.slotMs=10;old.periodMs=1000;
   mulMultiBroadcast_t ignored;assert(!mulMultiBroadcastDecode(&old,frame,n,&ignored));
  }
  mulMultiCycle_t result;assert(mulMultiReceiverPoll(&receiver,now+(count-1)*spacing+20,&result));
  mulTelemetry_t telemetry={.type=mulTlmVector,.node=141,.radio=2,.role=2,.mode=5,.epoch=7,.quality=3,.ratePolicy=1,.vector=result};
  char line[MUL_TLM_LINE];mulTelemetry_t recovered;
  assert(mulTelemetryEncode(line,sizeof(line),&telemetry)&&mulTelemetryDecode(line,&recovered));
  assert(recovered.quality==3&&recovered.vector.slotMs==spacing&&recovered.vector.periodMs==period);
  if(spacing!=10&&spacing!=50&&spacing!=500){telemetry.quality=0;assert(!mulTelemetryEncode(line,sizeof(line),&telemetry));}
  if(step>=5)for(unsigned slot=1;slot<count;slot++) {
   if(result.values[slot-1].status!=mulDownlinkAvailable)fprintf(stderr,"timing N=%u spacing=%u period=%u step=%u slot=%u status=%u reason=%u rate=%u\n",count,spacing,period,step,slot,result.values[slot-1].status,result.values[slot-1].reason,receiver.rate.status);
   assert(result.values[slot-1].status==mulDownlinkAvailable);
   assert(abs(result.values[slot-1].tdoaDtu-(slot%2?477:-477))<=3);
  }
 }
}
int main(void) {
 const unsigned counts[]={2,3,4,8},spacings[]={2,3,5,7,10,17,50,200,251},periods[]={100,200,500,1000,2000};
 for(unsigned i=0;i<4;i++)for(unsigned j=0;j<9;j++)for(unsigned k=0;k<5;k++)synthetic(counts[i],spacings[j],periods[k]);
 mulSchedule_t s;mulScheduleLegacy(&s,132,133,10);mulScheduleExperimental(&s);assert(!mulScheduleValidate(&s));
 s.slotMs=3;assert(mulScheduleValidate(&s));s.txLeadUs=1000;s.rxBudgetUs=500;assert(!mulScheduleValidate(&s));
 s.slotMs=2;assert(mulScheduleValidate(&s));s.txLeadUs=500;s.airtimeUs=400;assert(!mulScheduleValidate(&s));
 s.slotMs=1;assert(mulScheduleValidate(&s));s.slotMs=10;s.txLeadUs=499;assert(mulScheduleValidate(&s));
 s.txLeadUs=500;s.periodMs=200;assert(mulScheduleValidate(&s));s.rangingFlags=0;assert(!mulScheduleValidate(&s));
 s.periodMs=201;assert(mulScheduleValidate(&s));s.periodMs=200;s.rangingFlags=8;assert(mulScheduleValidate(&s));
 assert(sizeof(mulClock_t)==80&&sizeof(mulRelation_t)==48);
 puts("Experimental timing core: schema2/profile3, numerical2/3/5/7/10/17/50ms and100..2000ms cycles, guards/disabled AA, strict legacy rejection, period-aware geometry and fixed clock/relation size passed");return 0;
}
