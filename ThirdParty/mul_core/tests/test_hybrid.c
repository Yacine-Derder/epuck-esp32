#include "mul_hybrid.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define MS UINT64_C(63897600)
static uint8_t receiver[8]={0x21,0,0,0,0,0,0xcf,0xbc};
static uint8_t ref[8]={0x84,0,0,0,0,0,0xcf,0xbc},fol[8]={0x85,0,0,0,0,0,0xcf,0xbc};
static mulHybrid_t s;static mulHybridOutput_t out;
static uint64_t refTx,refRx;
static void broadcasts(unsigned seq,bool stationary,bool broken) {
 (void)stationary;
 refTx=mulTimestampMask(MUL_TIMESTAMP_MASK-2*1000*MS+seq*1000*MS);
 refRx=mulTimestampMask(100000+seq*1000*MS);
 mulTestPacket_t p={.sequence=seq,.txTimestamp=refTx};memcpy(p.source,ref,8);
 uint8_t frame[84];mulEncodeTestPacket(frame,p.source,p.sequence,p.txTimestamp);
 assert(mulHybridReceive(&s,frame,34,refRx,1,seq*1000,&out));
 p.sequence=seq+500;p.txTimestamp=mulTimestampMask(500000+seq*1000*MS+MUL_FOLLOWER_SLOT_DTU);memcpy(p.source,fol,8);
 mulRelation_t r={.version=1,.epoch=1,.referenceSequence=seq+(broken?1:0),.twrSequence=seq,
 .referenceOrigin=refTx,.followerOrigin=mulTimestampMask(500000+seq*1000*MS),.originAgeMs=MUL_FOLLOWER_SLOT_MS,.delayAgeMs=MUL_FOLLOWER_SLOT_MS,.remainingMs=100,.available=true};
 mulRelationEncode(frame,&p,&r);
 assert(mulHybridReceive(&s,frame,74,mulTimestampMask(refRx+MUL_FOLLOWER_SLOT_DTU+1000),1,seq*1000+MUL_FOLLOWER_SLOT_MS,&out));
}
int main(void) {
 /* Use the ALR's real publisher, not only hand-built v1 test relations.
  * A currently valid v2 mapping must survive the exact radio codec. */
 mulClock_t clock={.hasBaseline=true,.status=mulClockValid,.epoch=3,
 .sequence=11,.referenceTx=MUL_TIMESTAMP_MASK-1000,.localRx=500000,
 .observedMs=11000};
 mulClockDelay_t delay={.epoch=3,.oneWayDtu=120,.measuredMs=10650,
 .referenceSequence=10,.maxAgeMs=6000,.valid=true,
 .measurementBoundDtu=UINT64_MAX,.motionBoundDtuPerMs=UINT64_MAX};
 memcpy(delay.reference,ref,8);memcpy(delay.follower,fol,8);
 mulTestPacket_t packet={.sequence=500,.txTimestamp=mulTimestampMask(clock.localRx+MUL_FOLLOWER_SLOT_DTU)};
 memcpy(packet.source,fol,8);
 mulRelation_t published=mulRelationPublish(&clock,&delay,packet.txTimestamp,
 mulTimestampMask(packet.txTimestamp-10*MS),11000+MUL_FOLLOWER_SLOT_MS-10,true);
 assert(published.available&&published.version==2);
 uint8_t actual[74];mulRelationEncode(actual,&packet,&published);
 mulTestPacket_t decoded;mulRelation_t relation;
 assert(mulBroadcastDecode(actual,74,&decoded,&relation)&&relation.available);
 uint64_t converted;assert(mulRelationInverse(&relation,decoded.txTimestamp,1,&converted));
 assert(mulTimestampDifference(converted,clock.referenceTx)==(int64_t)MUL_FOLLOWER_SLOT_DTU+120);
 for(unsigned policy=0;policy<2;policy++) {
   mulHybridInit(&s,receiver,policy);
   for(unsigned i=0;i<=10;i++)broadcasts(i,policy,false);
   assert(out.associated&&out.observation.clockValid);
   assert(out.observation.status==(policy?mulDownlinkAvailable:mulDownlinkNoRate));
   if(policy)assert(out.observation.tdoaDtu==1000);
   assert(out.observation.powerFlags==0); /* Never apply DW1000 power tables to DWM. */
 }
 broadcasts(11,true,true);assert(out.associated&&!out.observation.clockValid&&out.observation.status==mulDownlinkNoRelation);
 broadcasts(15,true,false); /* Missing frames suppress immediate association. */
 assert(!out.associated);broadcasts(16,true,false);assert(out.associated);
 uint8_t bad[34];mulEncodeTestPacket(bad,ref,16,refTx);bad[14]=1;
 assert(!mulHybridReceive(&s,bad,34,refRx,1,16000,&out));
 mulHybridExpire(&s,20000);assert(!s.rate.hasBaseline&&!s.association.pending);
 /* Fresh receiver epoch for observer TWR; preserve exact reference protocol. */
 mulHybridInit(&s,receiver,false);broadcasts(10,false,false);
 uint64_t stamps[6];stamps[0]=mulTimestampMask(refTx+650*MS);stamps[1]=900000000000ULL;
 stamps[2]=mulTimestampMask(stamps[1]+20*MS);stamps[3]=mulTimestampMask(stamps[0]+20*MS+200);
 stamps[4]=mulTimestampMask(stamps[3]+20*MS);stamps[5]=mulTimestampMask(stamps[2]+20*MS+200);
 uint32_t tof;assert(mulTwrCalculate(stamps,&tof)&&tof==100);
 for(unsigned stage=0;stage<5;stage++) {
   mulTwrPacket_t p={.type=mulTwrPoll+stage,.id=10};unsigned used=stage==0?1:stage==1?3:stage==2?5:6;
   memcpy(p.stamps,stamps,used*sizeof(uint64_t));p.txTimestamp=stage<3?p.stamps[used-1]:stage==3?mulTimestampMask(stamps[5]+20*MS):mulTimestampMask(stamps[4]+40*MS+200);
   if(stage==4)p.tof=100;
   uint8_t frame[84];
   if(stage==4) {
     mulTwrPacket_t wrong=p;wrong.txTimestamp=wrong.stamps[4];mulTwrEncode(frame,&wrong);
     assert(mulHybridReceive(&s,frame,sizeof(frame),mulTimestampMask(refRx+730*MS),1,10730,&out)&&!out.twrComplete);
     assert(s.twr.type==mulTwrReport); /* Failed RESULT does not advance state. */
   }
   mulTwrEncode(frame,&p);
   assert(mulHybridReceive(&s,frame,sizeof(frame),mulTimestampMask(refRx+(650+stage*20)*MS),1,10650+stage*20,&out));
   assert(out.twrComplete==(stage==4));
   if(stage==4)assert(out.twrRaw==100&&out.twrCorrected==UINT32_MAX);
 }
 assert(s.twrCompleted==1&&!s.twrActive);
 assert(s.diagnostics.reason[mulHybridTimestamp]==1);
 for(unsigned i=0;i<5;i++)assert(s.diagnostics.twrFrames[i]==(i==4?2u:1u));
 /* A radio listener need not receive all directed packets. Missing/reordered
  * components are observable failures, not malformed packets or a valid ToF. */
 mulTwrPacket_t p={.type=mulTwrResult,.id=10,.tof=100,
 .txTimestamp=mulTimestampMask(stamps[4]+40*MS+200)};
 memcpy(p.stamps,stamps,sizeof(stamps));uint8_t frame[84];mulTwrEncode(frame,&p);
 assert(mulHybridReceive(&s,frame,84,mulTimestampMask(refRx+730*MS),1,10730,&out)&&!out.twrComplete);
 assert(s.diagnostics.lastReason==mulHybridMissingTwr);
 mulHybridInit(&s,receiver,false);
 assert(mulHybridReceive(&s,frame,84,refRx,1,10730,&out)&&!out.twrComplete);
 assert(s.diagnostics.lastReason==mulHybridNoReference);
 broadcasts(10,false,false);
 assert(mulHybridReceive(&s,frame,84,mulTimestampMask(refRx+730*MS),16,10730,&out)&&!out.twrComplete);
 assert(s.diagnostics.lastReason==mulHybridCaptureAge);
 assert(mulHybridReceive(&s,frame,84,mulTimestampMask(refRx+730*MS),1,10730,&out)&&!out.twrComplete);
 assert(s.diagnostics.lastReason==mulHybridMissingTwr);
 p.type=mulTwrPoll;p.tof=0;memset(p.stamps,0,sizeof(p.stamps));p.stamps[0]=stamps[0];p.txTimestamp=stamps[0];mulTwrEncode(frame,&p);
 assert(mulHybridReceive(&s,frame,84,mulTimestampMask(refRx+650*MS),1,10650,&out));
 p.type=mulTwrFinal;memcpy(p.stamps,stamps,5*sizeof(uint64_t));p.txTimestamp=stamps[4];mulTwrEncode(frame,&p);
 assert(mulHybridReceive(&s,frame,84,mulTimestampMask(refRx+690*MS),1,10690,&out)&&!out.twrComplete);
 assert(s.diagnostics.lastReason==mulHybridMissingTwr);
 p.type=mulTwrResponse;p.stamps[0]=mulTimestampMask(stamps[0]+1);p.stamps[3]=p.stamps[4]=0;p.txTimestamp=stamps[2];mulTwrEncode(frame,&p);
 assert(mulHybridReceive(&s,frame,84,mulTimestampMask(refRx+670*MS),1,10670,&out));
 assert(s.diagnostics.lastReason==mulHybridPrefix);
 mulHybridExpire(&s,10900);assert(s.twrTimeouts==1&&!s.twrActive);
 frame[13]=0x22;assert(!mulHybridReceive(&s,frame,84,refRx,1,10900,&out));assert(s.diagnostics.lastReason==mulHybridAddress);
 assert(!mulHybridReceive(&s,frame,33,refRx,1,10900,&out));assert(s.diagnostics.lastReason==mulHybridShape);
 broadcasts(11,false,true);assert(s.diagnostics.reason[mulHybridStaleRelation]);
 assert(!strcmp(mulHybridReasonName(mulHybridMissingTwr),"missing_twr"));
 assert(!strcmp(mulHybridReasonName(mulHybridReasonCount),"none"));
 puts("Hybrid shared receiver: exact relation/association, stationary opt-in, wrap/loss/domain guards and passive inter-ALR TWR verification passed");return 0;
}
