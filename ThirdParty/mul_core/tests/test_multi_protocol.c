#include "mul_multi_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define MS UINT64_C(63897600)
int main(void) {
 const unsigned counts[]={2,3,4,8};
 for(unsigned c=0;c<4;c++) {
  mulSchedule_t s={.revision=1,.profile=MUL_PROFILE_MULTI,.count=counts[c],.slotMs=10,.periodMs=1000};
  for(unsigned i=0;i<s.count;i++)s.ids[i]=(uint8_t)(132+i*3);
  for(unsigned slot=0;slot<s.count;slot++) {
   uint8_t frame[90];memset(frame,0xaa,sizeof(frame));
   uint64_t origin=mulTimestampMask(MUL_TIMESTAMP_MASK-100*MS),tx=mulTimestampMask(origin+slot*10*MS);
   mulRelation_t relation={.version=2,.epoch=7,.referenceSequence=UINT32_MAX,.twrSequence=UINT32_MAX,.referenceOrigin=origin,.followerOrigin=origin,.originAgeMs=slot*10,.delayAgeMs=slot*10,.delayDtu=120,.delayState=mulPropagationFresh,.maxAgeMs=6000,.available=true};
   size_t n=mulMultiBroadcastEncode(frame,sizeof(frame),&s,slot,UINT32_MAX,tx,slot?&relation:NULL);
   assert(n==(slot?80u:40u)&&n+2<=127&&frame[n]==0xaa);
   mulMultiBroadcast_t out;assert(mulMultiBroadcastDecode(&s,frame,n,&out));
   assert(out.slot==slot&&out.packet.sequence==UINT32_MAX&&out.packet.txTimestamp==tx&&out.packet.source[0]==s.ids[slot]);
   if(slot)assert(out.relation.available&&out.relation.referenceSequence==UINT32_MAX);
   mulSchedule_t mismatch=s;mismatch.revision++;
   assert(!mulMultiBroadcastDecode(&mismatch,frame,n,&out));
   const unsigned corrupt[]={0,1,2,3,5,13,21,24,34,35,36};
   for(unsigned i=0;i<sizeof(corrupt)/sizeof(*corrupt);i++) {
    frame[corrupt[i]]^=1;assert(!mulMultiBroadcastDecode(&s,frame,n,&out));frame[corrupt[i]]^=1;
   }
   for(unsigned length=0;length<90;length++)if(length!=n)assert(!mulMultiBroadcastDecode(&s,frame,length,&out));
   mulTestPacket_t legacy;mulRelation_t ignored;assert(!mulBroadcastDecode(frame,n,&legacy,&ignored));
   assert(!mulMultiBroadcastEncode(frame,n-1,&s,slot,UINT32_MAX,tx,slot?&relation:NULL));
  }
  for(unsigned window=0;window<2;window++)for(unsigned cycle=1;cycle<12;cycle++) {
   unsigned owner=mulScheduleSyncOwner(&s,cycle,window);if(!owner)continue;
   mulTwrPacket_t p={.type=mulTwrPoll,.id=cycle,.stamps={mulTimestampMask(100000+650*MS)},.txTimestamp=mulTimestampMask(100000+650*MS)};
   uint8_t frame[90];assert(mulMultiTwrEncode(frame,&s,window,&p));
   mulTwrPacket_t decoded;unsigned slot;
   assert(mulMultiTwrDecode(&s,window,frame,90,&decoded,&slot)&&slot==owner&&decoded.id==cycle);
   assert(!mulTwrDecode(frame,84,&decoded));
   frame[85]=0;assert(!mulMultiTwrDecode(&s,window,frame,90,&decoded,&slot));frame[85]=(uint8_t)owner;
   frame[86]^=1;assert(!mulMultiTwrDecode(&s,window,frame,90,&decoded,&slot));
  }
 }
 puts("Multi radio codec: N2/3/4/8, reference40/follower80/TWR90, native40/cycle32 wrap, strict schedule/version/source/window, malformed/truncated and legacy rejection passed");return 0;
}
