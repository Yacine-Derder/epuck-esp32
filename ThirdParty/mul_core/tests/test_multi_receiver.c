#include "mul_multi_receiver.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define MS INT64_C(63897600)
static int64_t scale(int64_t n,int32_t ppb){int64_t product=n*ppb;return n+(product>=0?(product+500000000)/1000000000:-((-product+500000000)/1000000000));}
static const int32_t differences[]={-477,0,477,2000,-2000,1,-1};
static size_t packet(const mulSchedule_t *s,unsigned slot,unsigned step,uint32_t cycle,bool available,uint8_t frame[90],uint64_t *rx) {
 int32_t folPpb=slot%2?35000:-45000,receiverPpb=15000;
 int64_t at=step*1000*MS,offset=slot*s->slotMs*MS;
 uint64_t reference=mulTimestampMask(MUL_TIMESTAMP_MASK-2000*MS+at);
 uint64_t followerOrigin=mulTimestampMask(500000+scale(at,folPpb));
 int32_t path=4000+(slot?differences[slot-1]:0);
 *rx=mulTimestampMask(MUL_TIMESTAMP_MASK-100000+scale(at+(slot?offset+120+path:path),receiverPpb));
 mulRelation_t r={.version=2,.epoch=slot+10,.referenceSequence=cycle,.maxAgeMs=6000};
 if(slot&&available)r=(mulRelation_t){.version=2,.epoch=slot+10,.available=true,.referenceSequence=cycle,.twrSequence=cycle-3,
  .referenceOrigin=reference,.followerOrigin=followerOrigin,.skewPpb=folPpb,.originAgeMs=slot*s->slotMs+1,
  .delayAgeMs=3000+slot*s->slotMs,.delayDtu=120,.delayState=mulPropagationReused,.maxAgeMs=6000};
 uint64_t tx=slot?mulTimestampMask(followerOrigin+scale(offset+120,folPpb)):reference;
 return mulMultiBroadcastEncode(frame,90,s,slot,cycle,tx,slot?&r:NULL);
}
static void scenario(unsigned count,unsigned spacing,bool stationary) {
 mulSchedule_t schedule={.profile=MUL_PROFILE_MULTI,.count=count,.slotMs=spacing,.periodMs=1000,.revision=1};
 for(unsigned i=0;i<count;i++)schedule.ids[i]=(uint8_t)(132+i);
 mulMultiReceiver_t s;const uint8_t receiver[8]={0x8d,0,0,0,0,0,0xcf,0xbc};
 assert(mulMultiReceiverInit(&s,&schedule,receiver,stationary));
 for(unsigned step=0;step<14;step++) {
  uint32_t cycle=UINT32_MAX-5+step,base=UINT32_MAX-8000+step*1000;
  for(unsigned slot=0;slot<count;slot++) {
   if(step==6&&slot==2)continue;
   if(step==7&&!slot)continue;
   uint8_t frame[90];uint64_t rx;size_t n=packet(&schedule,slot,step,cycle,!(step==6&&slot==3),frame,&rx);assert(n);
   mulMultiReceiveOutput_t out;uint32_t now=base+slot*spacing;
   assert(mulMultiReceiverReceive(&s,frame,n,rx,1,now,&out)&&!out.cycleReady);
   assert(!mulMultiReceiverReceive(&s,frame,n,rx,1,now,&out));
  }
  mulMultiCycle_t result;
  assert(!mulMultiReceiverPoll(&s,s.cycleStartMs+(count-1)*spacing+19,&result));
  assert(mulMultiReceiverPoll(&s,s.cycleStartMs+(count-1)*spacing+20,&result));
  assert(!mulMultiReceiverPoll(&s,base+900,&result));
  assert(result.cycle==cycle&&result.count==count&&result.fingerprint==mulScheduleFingerprint(&schedule));
  for(unsigned slot=1;slot<count;slot++) {
   const mulMultiValue_t *v=&result.values[slot-1];
   if(step==6&&slot==2){assert(v->reason==mulMultiMissing&&v->rangeDifferenceMm==INT32_MIN);continue;}
   if(step==6&&slot==3){assert(v->status==mulDownlinkNoRelation&&v->rangeDifferenceMm==INT32_MIN);continue;}
   if(step==7){assert(v->reason==mulMultiNoReference&&v->rangeDifferenceMm==INT32_MIN);continue;}
   if(step>=5&&stationary) {
    if(v->status!=mulDownlinkAvailable)fprintf(stderr,"N=%u spacing=%u step=%u slot=%u status=%u reason=%u rate=%u clock=%u\n",count,spacing,step,slot,v->status,v->reason,s.rate.status,v->clockValid);
    assert(v->status==mulDownlinkAvailable&&v->reason==mulMultiOk&&(result.validMask&(1u<<slot)));
    assert(v->tdoaDtu>=differences[slot-1]-3&&v->tdoaDtu<=differences[slot-1]+3);
   } else if(!stationary)assert(v->status==mulDownlinkNoRate&&v->rangeDifferenceMm==INT32_MIN);
  }
 }
 assert(s.duplicates&&s.cycles==14);
 // Schedule mismatch and late/old packets cannot mutate completed observations.
 uint8_t frame[90];uint64_t rx;size_t n=packet(&schedule,1,13,s.cycle,true,frame,&rx);mulMultiReceiveOutput_t out;
 frame[36]^=1;unsigned mismatch=s.mismatches;assert(!mulMultiReceiverReceive(&s,frame,n,rx,1,s.lastCycleMs+100,&out)&&s.mismatches==mismatch+1);
 frame[36]^=1;assert(!mulMultiReceiverReceive(&s,frame,n,rx,1,s.lastCycleMs+100,&out)&&s.late);
}
static void reordering_and_recovery(void) {
 mulSchedule_t schedule={.profile=MUL_PROFILE_MULTI,.count=4,.slotMs=10,.periodMs=1000,.revision=1,.ids={132,133,134,135}};
 mulMultiReceiver_t s;const uint8_t receiver[8]={141,0,0,0,0,0,0xcf,0xbc};assert(mulMultiReceiverInit(&s,&schedule,receiver,true));
 for(unsigned step=0;step<17;step++) {
  unsigned order[4]={0,1,2,3};
  if(step==6){order[0]=1;order[1]=0;}
  if(step==7){order[2]=3;order[3]=2;}
  for(unsigned at=0;at<4;at++) {
   unsigned slot=order[at];uint8_t frame[90];uint64_t rx;size_t n=packet(&schedule,slot,step,100+step,true,frame,&rx);
   uint32_t now=10000+step*1000+slot*10;unsigned age=1;
   if(step==6&&!slot){now=10000+step*1000+12;age=12;}
   if(step==7&&slot==2){now=10000+step*1000+32;age=12;}
   if(step==8&&slot==2)age=16;
   if(step==9&&!slot)age=16;
   if(step==16&&slot==2)frame[36]^=1;
   mulMultiReceiveOutput_t out;
   bool accepted=mulMultiReceiverReceive(&s,frame,n,rx,age,now,&out);
   assert(accepted==!(step==16&&slot==2));
  }
  mulMultiCycle_t cycle;assert(mulMultiReceiverPoll(&s,s.cycleStartMs+50,&cycle));
  if(step==6||step==7||step==15)assert(cycle.validMask==14);
  if(step==8){assert(cycle.values[1].reason==mulMultiCaptureAge);assert(cycle.validMask==10);}
  if(step==9){assert(!cycle.validMask);for(unsigned i=0;i<3;i++)assert(cycle.values[i].reason==mulMultiCaptureAge);}
  if(step==16){assert(cycle.values[1].reason==mulMultiScheduleMismatch&&cycle.validMask==10);}
 }
 uint8_t frame[90];uint64_t rx;size_t n=packet(&schedule,0,18,1,true,frame,&rx);mulMultiReceiveOutput_t out;
 assert(!mulMultiReceiverReceive(&s,frame,n,rx,1,s.lastCycleMs+100,&out));
 assert(mulMultiReceiverReceive(&s,frame,n,rx,1,s.lastCycleMs+3100,&out));assert(s.rate.status!=mulClockValid);
}
int main(void) {
 const unsigned counts[]={2,3,4,8};
 for(unsigned n=0;n<4;n++)for(unsigned spacing=10;spacing<=50;spacing+=40)for(unsigned policy=0;policy<2;policy++)scenario(counts[n],spacing,policy!=0);
 scenario(2,500,true);reordering_and_recovery();
 puts("Multi receiver: N2/3/4/8, signed/zero exact geometry and clock skews, partial vectors, missing ref/follower, invalid relation, duplicates/stale/schedule rejection and native40/cycle32/uptime32 wrap passed");return 0;
}
