#include "mul_plan_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
 mulRangingPlan_t p={.enabled=3,.replyMs=5,.maxUtilization=80,.aaMilliHz=10000,.arMilliHz=5500,.rlrCount=2,.rlrs={141,142}};
 mulScheduleLegacy(&p.broadcast,132,133,2);mulScheduleExperimental(&p.broadcast);
 p.broadcast.txLeadUs=500;p.broadcast.rxBudgetUs=100;p.broadcast.airtimeUs=400;p.broadcast.rangingFlags=0;
 assert(!mulRangingPlanValidate(&p,NULL));assert(mulRangingPlanCount(&p,0)==32);
 uint8_t frame[90];mulMultiBroadcast_t decoded;
 size_t n=mulPlanBroadcastEncode(frame,sizeof(frame),&p,0,UINT32_MAX,UINT64_C(0xffffffffff),NULL);
 assert(n==40&&frame[34]==4&&mulPlanBroadcastDecode(&p,frame,n,&decoded));
 assert(decoded.packet.sequence==UINT32_MAX&&decoded.packet.txTimestamp==UINT64_C(0xffffffffff));
 assert(!mulMultiBroadcastDecode(&p.broadcast,frame,n,&decoded));
 mulRangingPlan_t different=p;different.rlrs[1]=143;
 assert(mulScheduleFingerprint(&different.broadcast)==mulScheduleFingerprint(&p.broadcast));
 assert(!mulPlanBroadcastDecode(&different,frame,n,&decoded));
 for(unsigned length=0;length<40;length++)assert(!mulPlanBroadcastDecode(&p,frame,length,&decoded));
 mulRelation_t r={.version=2,.epoch=8,.referenceSequence=10,.maxAgeMs=MUL_PROP_MAX_AGE_MS};
 assert(mulPlanBroadcastEncode(frame,sizeof(frame),&p,1,10,123,&r)==80);
 assert(mulPlanBroadcastDecode(&p,frame,80,&decoded)&&decoded.slot==1);
 bool aa=false,ar=false;
 for(unsigned index=0;index<32;index++) {
  mulRangingOperation_t op;assert(mulRangingPlanOperation(&p,0,index,&op));aa|=op.kind==1;ar|=op.kind==2;
  mulTwrPacket_t packet={.type=mulTwrPoll,.id=0,.stamps={123},.txTimestamp=123},out;
  assert(mulPlanTwrEncode(frame,&p,index,&packet));assert(frame[84]==4&&frame[85]==index);
  assert(mulPlanTwrDecode(&p,0,index,frame,90,&out)&&out.id==0);
  assert(frame[13]==op.initiator&&frame[5]==op.responder);
  assert(!mulPlanTwrDecode(&different,0,index,frame,90,&out));
  assert(!mulPlanTwrDecode(&p,1,index,frame,90,&out));
  unsigned ignored;assert(!mulMultiTwrDecode(&p.broadcast,0,frame,90,&out,&ignored));
  frame[85]^=1;assert(!mulPlanTwrDecode(&p,0,index,frame,90,&out));frame[85]^=1;
  frame[13]^=1;assert(!mulPlanTwrDecode(&p,0,index,frame,90,&out));
 }
 assert(aa&&ar);
 puts("Plan radio v4: full-plan/RLR-roster binding, strict legacy rejection, broadcast40/80, TWR90, all32 operation indices, generic AA/AR identities and cycle/source/index/length rejection passed");
 return 0;
}
