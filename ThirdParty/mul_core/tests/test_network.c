/* Explicit rosters must reach every codec/math/association boundary, and two
 * receivers must never mutate each other's roles. No radio or wall-clock mock. */
#include "mul_hybrid.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define MS INT64_C(63897600)
static int64_t fast(int64_t t) {return t+(t*15000+500000000)/1000000000;}
static void cycle(mulHybrid_t *s,const mulNetwork_t *network,unsigned seq) {
 uint16_t slot=mulNetworkSlotMs(network);
 uint64_t reference=mulTimestampMask(MUL_TIMESTAMP_MASK-2*1000*MS+seq*1000*MS);
 uint64_t origin=mulTimestampMask(500000+seq*1000*MS);
 mulClock_t clock={.hasBaseline=true,.status=mulClockValid,.epoch=7,.sequence=seq,
  .referenceTx=reference,.localRx=mulTimestampMask(origin+120),.observedMs=seq*1000};
 mulClockDelay_t delay={.epoch=7,.oneWayDtu=120,.measuredMs=seq*1000,.referenceSequence=seq,
  .maxAgeMs=6000,.valid=true,.measurementBoundDtu=UINT64_MAX,.motionBoundDtuPerMs=UINT64_MAX};
 memcpy(delay.reference,network->addresses[0],8);memcpy(delay.follower,network->addresses[1],8);
 assert(mulClockDelayStatusNetwork(network,&clock,&delay,seq*1000)==mulClockDelayValid);
 assert(mulClockDelayStatus(&clock,&delay,seq*1000)==mulClockDelayInvalid);
 uint64_t tx=mulTimestampMask(origin+(slot*MS));
 mulRelation_t relation=mulRelationPublishNetwork(network,&clock,&delay,tx,
  mulTimestampMask(tx-10*MS),seq*1000+slot-10,true);
 assert(relation.available&&relation.followerOrigin==origin);
 uint8_t frame[84];mulHybridOutput_t out;
 if(network->slotMs)assert(mulEncodeScheduledPacket(frame,network->addresses[0],seq,reference,slot));
 else mulEncodeTestPacket(frame,network->addresses[0],seq,reference);
 uint64_t rx=mulTimestampMask(MUL_TIMESTAMP_MASK-100000+fast(seq*1000*MS+4000));
 assert(mulHybridReceive(s,frame,34,rx,1,seq*1000,&out));
 mulTestPacket_t packet={.sequence=seq+500,.txTimestamp=tx,.slotMs=network->slotMs};memcpy(packet.source,network->addresses[1],8);
 mulRelationEncode(frame,&packet,&relation);
 assert(!mulBroadcastDecode(frame,74,&packet,&relation)); /* Default roster fails closed. */
 assert(mulHybridReceive(s,frame,74,mulTimestampMask(rx+fast((slot*MS)+1000)),1,seq*1000+slot,&out));
 if(seq>=5) {
  assert(out.associated&&out.observation.clockValid);
  assert(out.observation.status==(s->stationary?mulDownlinkAvailable:mulDownlinkNoRate));
  if(s->stationary)assert(out.observation.tdoaDtu>=998&&out.observation.tdoaDtu<=1002);
  assert(!memcmp(out.observation.reference,network->addresses[0],8));
 }
}
static void ranging(mulHybrid_t *s,const mulNetwork_t *network) {
 uint64_t ref=s->observations[0].source.txTimestamp,rx=s->observations[0].source.rxTimestamp;
 uint64_t stamps[6]={mulTimestampMask(ref+650*MS),900000000000ULL,0,0,0,0};
 stamps[2]=mulTimestampMask(stamps[1]+20*MS);stamps[3]=mulTimestampMask(stamps[0]+20*MS+200);
 stamps[4]=mulTimestampMask(stamps[3]+20*MS);stamps[5]=mulTimestampMask(stamps[2]+20*MS+200);
 for(unsigned stage=0;stage<5;stage++) {
  mulTwrPacket_t packet={.type=mulTwrPoll+stage,.id=10,.tof=stage==4?100:0};
  unsigned used=stage==0?1:stage==1?3:stage==2?5:6;memcpy(packet.stamps,stamps,used*sizeof(uint64_t));
  packet.txTimestamp=stage<3?stamps[used-1]:stage==3?mulTimestampMask(stamps[5]+20*MS):mulTimestampMask(stamps[4]+40*MS+200);
  uint8_t frame[84];assert(mulTwrEncodeNetwork(network,frame,&packet));mulHybridOutput_t out;
  assert(mulHybridReceive(s,frame,84,mulTimestampMask(rx+fast((650+stage*20)*MS)),1,10650+stage*20,&out));
  assert(out.twrComplete==(stage==4));
 }
 assert(s->twrCompleted==1&&s->twrRejected==0);
}
int main(void) {
 mulNetwork_t a,b,saved;assert(mulNetworkInit(&a,0x85,0x84));assert(mulNetworkInit(&b,0x42,0x23));saved=a;
 assert(!mulNetworkInit(&a,7,7)&&!memcmp(&a,&saved,sizeof(a)));
 assert(!mulNetworkInit(&a,255,7)&&!mulNetworkInit(NULL,1,2));
 assert(mulNetworkRole(&a,mulDefaultNetwork.addresses[0])==mulAlrFollower);
 uint8_t spoof[8];memcpy(spoof,a.addresses[0],8);spoof[3]=1;
 assert(mulNetworkRole(&a,spoof)==mulAlrUnsupported);
 mulNetwork_t invalid={0};assert(!mulNetworkValid(&invalid));
 const uint8_t receiver[8]={0x8d,0,0,0,0,0,0xcf,0xbc};mulHybrid_t first,second;
 assert(!mulHybridInitNetwork(&first,&invalid,receiver,false));
 assert(!mulHybridInitNetwork(&first,&a,a.addresses[0],false));
 assert(mulHybridInitNetwork(&first,&a,receiver,true));assert(mulHybridInitNetwork(&second,&b,receiver,false));
 for(unsigned i=0;i<11;i++){cycle(&first,&a,i);cycle(&second,&b,i);}
 ranging(&first,&a);ranging(&second,&b);
 assert(!memcmp(&first.network,&a,sizeof(a))&&!memcmp(&second.network,&b,sizeof(b)));
 for(unsigned i=0;i<5;i++) {
  mulTwrPacket_t packet={.type=mulTwrPoll+i,.id=10,.tof=i==4?100:0};unsigned used=i==0?1:i==1?3:i==2?5:6;
  for(unsigned j=0;j<used;j++)packet.stamps[j]=100000+j*1000;
  packet.txTimestamp=packet.stamps[used-1];uint8_t frame[84];mulTwrPacket_t decoded;
  assert(mulTwrEncodeNetwork(&a,frame,&packet));assert(mulTwrDecodeNetwork(&a,frame,84,&decoded));
  assert(!mulTwrDecodeNetwork(&b,frame,84,&decoded)&&!mulTwrDecode(frame,84,&decoded));
  frame[5]^=1;assert(!mulTwrDecodeNetwork(&a,frame,84,&decoded));
 }
 /* Every delay in this one executable, with disjoint receiver state. */
 const uint16_t slots[]={10,50,500};
 for(unsigned j=0;j<3;j++) {
   a.slotMs=slots[j];b.slotMs=slots[(j+1)%3];
   assert(mulNetworkValid(&a)&&mulNetworkValid(&b));
   assert(mulHybridInitNetwork(&first,&a,receiver,true));
   assert(mulHybridInitNetwork(&second,&b,receiver,false));
   for(unsigned i=0;i<11;i++){cycle(&first,&a,i);cycle(&second,&b,i);}
   assert(first.pairs&&second.pairs);
   uint8_t frame[34];mulHybridOutput_t out;
   assert(mulEncodeScheduledPacket(frame,a.addresses[0],11,0,slots[(j+1)%3]));
   assert(!mulHybridReceive(&first,frame,34,0,1,11000,&out));
   assert(first.diagnostics.reason[mulHybridScheduleMismatch]==1&&!first.association.pending&&!first.rate.hasBaseline);
   mulEncodeTestPacket(frame,a.addresses[0],11,0);
   assert(!mulHybridReceive(&first,frame,34,0,1,11000,&out));
   assert(first.diagnostics.reason[mulHybridScheduleMismatch]==2);
   assert(!mulEncodeScheduledPacket(frame,a.addresses[0],11,0,20));
 }
 a.slotMs=20;assert(!mulNetworkValid(&a));
 puts("Explicit roster: swapped/arbitrary IDs, simultaneous relation, clock, signed geometry, NO_RATE, strict TWR endpoints and independent receiver contexts passed");
 return 0;
}
