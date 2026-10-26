/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_ranging_plan.h"
#include <string.h>
static void put(uint8_t *p,uint32_t n,unsigned bytes){for(unsigned i=0;i<bytes;i++)p[i]=(uint8_t)(n>>(8*i));}
static uint32_t get(const uint8_t *p,unsigned bytes){uint32_t n=0;for(unsigned i=0;i<bytes;i++)n|=(uint32_t)p[i]<<(8*i);return n;}
const char *mulRangingPlanValidate(const mulRangingPlan_t *p,mulRangingBudget_t *out) {
 if(!p)return "missing ranging plan";
 const char *error=mulScheduleValidate(&p->broadcast);if(error)return error;
 const mulSchedule_t *s=&p->broadcast;
 if(s->profile!=MUL_PROFILE_EXPERIMENTAL)return "ranging planner requires explicit experimental timing";
 if(p->enabled&~(MUL_PLAN_AA|MUL_PLAN_AR))return "unknown ranging flags";
 if(p->rlrCount>MUL_MAX_RLRS)return "RLR count: 0..8";
 for(unsigned i=0;i<MUL_MAX_RLRS;i++) {
  if(i>=p->rlrCount){if(p->rlrs[i])return "unused RLR entries must be zero";continue;}
  if(p->rlrs[i]==255||mulScheduleIndex(s,p->rlrs[i])>=0)return "RLR ID invalid or in ALR roster";
  for(unsigned j=0;j<i;j++)if(p->rlrs[j]==p->rlrs[i])return "duplicate RLR identity";
 }
 if((p->enabled&MUL_PLAN_AR)&&!p->rlrCount)return "ALR-RLR ranging requires an RLR";
 if(p->aaMilliHz>10000||p->arMilliHz>10000)return "per-pair target: 0..10000 milliHz";
 if(p->replyMs!=5&&p->replyMs!=10&&p->replyMs!=20)return "reply_ms: 5, 10 or 20";
 if(p->maxUtilization<10||p->maxUtilization>80)return "utilization percent: 10..80";
 unsigned guard=(unsigned)s->guard100us*100;
 if((uint32_t)p->replyMs*1000<s->txLeadUs+s->rxBudgetUs+s->airtimeUs+guard)return "reply below lead+processing+airtime+guard";
 mulRangingBudget_t b={0};
 b.aaPairs=(p->enabled&MUL_PLAN_AA)?s->count-1:0;
 b.arPairs=(p->enabled&MUL_PLAN_AR)?s->count*p->rlrCount:0;
 b.requestedMilliHz=(uint32_t)b.aaPairs*p->aaMilliHz+(uint32_t)b.arPairs*p->arMilliHz;
 b.prepareUs=(uint32_t)s->txLeadUs+s->rxBudgetUs;
 /* RX timestamps are marker referenced: reserve four complete reply intervals,
  * the final RESULT airtime, processing, guard, and initial delayed-TX setup. */
 b.reservationUs=b.prepareUs+4u*p->replyMs*1000+s->airtimeUs+s->rxBudgetUs+guard;
 b.dataEndUs=(uint32_t)(s->count-1)*s->slotMs*1000+s->airtimeUs+s->rxBudgetUs+guard;
 b.usableEndUs=(uint32_t)s->periodMs*1000-20000; // Retain next-reference preparation.
 if(b.dataEndUs>b.usableEndUs)return "broadcasts overlap reference preparation";
 uint32_t available=b.usableEndUs-b.dataEndUs;
 uint32_t limit=(uint32_t)s->periodMs*1000*p->maxUtilization/100;
 if(b.dataEndUs>limit)return "broadcast reservation exceeds utilization limit";
 if(available>limit-b.dataEndUs)available=limit-b.dataEndUs;
 b.capacity=(uint16_t)(available/b.reservationUs);
 if(b.capacity>MUL_PLAN_MAX_OPERATIONS)b.capacity=MUL_PLAN_MAX_OPERATIONS;
 b.peakOperations=(uint16_t)(((uint64_t)b.requestedMilliHz*s->periodMs+999999)/1000000);
 if(b.peakOperations>b.capacity)return "requested per-pair rates exceed guarded airtime/capacity";
 if(out)*out=b;
 return NULL;
}
static uint64_t before(const mulRangingPlan_t *p,const mulRangingBudget_t *b,uint64_t cycle) {
 return cycle*b->requestedMilliHz*p->broadcast.periodMs/1000000;
}
unsigned mulRangingPlanCount(const mulRangingPlan_t *p,uint32_t cycle) {
 mulRangingBudget_t b;if(mulRangingPlanValidate(p,&b))return 0;
 return (unsigned)(before(p,&b,(uint64_t)cycle+1)-before(p,&b,cycle));
}
bool mulRangingPlanOperation(const mulRangingPlan_t *p,uint32_t cycle,unsigned index,mulRangingOperation_t *out) {
 mulRangingBudget_t b;if(!out||mulRangingPlanValidate(p,&b)||!b.requestedMilliHz)return false;
 uint64_t first=before(p,&b,cycle),last=before(p,&b,(uint64_t)cycle+1);
 unsigned count=(unsigned)(last-first);if(index>=count)return false;
 uint64_t ordinal=first+index;
 uint32_t aaWeight=(uint32_t)b.aaPairs*p->aaMilliHz;
 /* Split aggregate opportunities by requested class weight, then rotate pairs.
  * Differences between any two pairs in a class are bounded by one operation.
  * Divide before multiply to bound the uint64 intermediate at long uptimes. */
 uint64_t aaBefore=(ordinal/b.requestedMilliHz)*aaWeight+(ordinal%b.requestedMilliHz)*aaWeight/b.requestedMilliHz;
 uint64_t next=ordinal+1;
 uint64_t aaAfter=(next/b.requestedMilliHz)*aaWeight+(next%b.requestedMilliHz)*aaWeight/b.requestedMilliHz;
 mulRangingOperation_t o={0};
 if(aaAfter!=aaBefore) {
  unsigned follower=1+(unsigned)(aaBefore%b.aaPairs);
  o.kind=MUL_PLAN_AA;o.initiator=p->broadcast.ids[0];o.responder=p->broadcast.ids[follower];
 }else {
  unsigned pair=(unsigned)((ordinal-aaBefore)%b.arPairs);
  o.kind=MUL_PLAN_AR;o.initiator=p->broadcast.ids[pair/p->rlrCount];o.responder=p->rlrs[pair%p->rlrCount];
 }
 uint32_t span=b.usableEndUs-b.dataEndUs;
 o.startUs=b.dataEndUs+(uint32_t)((uint64_t)index*span/count);
 o.pollUs=o.startUs+b.prepareUs;o.endUs=o.startUs+b.reservationUs;
 if(o.endUs>b.usableEndUs)return false;
 *out=o;return true;
}
bool mulRangingPlanEncode(uint8_t out[MUL_PLAN_BYTES],const mulRangingPlan_t *p) {
 if(!out||mulRangingPlanValidate(p,NULL))return false;
 uint8_t b[MUL_PLAN_BYTES]={0};b[0]=1;b[1]=p->enabled;b[2]=p->replyMs;b[3]=p->maxUtilization;
 if(!mulScheduleEncode(b+4,&p->broadcast))return false;
 b[36]=p->rlrCount;memcpy(b+37,p->rlrs,MUL_MAX_RLRS);
 put(b+46,p->aaMilliHz,2);put(b+48,p->arMilliHz,2);put(b+60,mulScheduleCrc(b,60),4);
 memcpy(out,b,sizeof(b));return true;
}
const char *mulRangingPlanDecodeError(const uint8_t in[MUL_PLAN_BYTES],mulRangingPlan_t *out) {
 if(!in||!out||in[0]!=1||in[45]||get(in+60,4)!=mulScheduleCrc(in,60))return "ranging plan version/CRC/header";
 for(unsigned i=50;i<60;i++)if(in[i])return "ranging plan reserved bytes";
 mulRangingPlan_t p={.enabled=in[1],.replyMs=in[2],.maxUtilization=in[3],.rlrCount=in[36],
 .aaMilliHz=(uint16_t)get(in+46,2),.arMilliHz=(uint16_t)get(in+48,2)};
 if(!mulScheduleDecode(in+4,&p.broadcast))return "invalid broadcast timing/roster";
 memcpy(p.rlrs,in+37,MUL_MAX_RLRS);const char *error=mulRangingPlanValidate(&p,NULL);if(error)return error;
 *out=p;return NULL;
}
bool mulRangingPlanDecode(const uint8_t in[MUL_PLAN_BYTES],mulRangingPlan_t *out){return !mulRangingPlanDecodeError(in,out);}
uint32_t mulRangingPlanFingerprint(const mulRangingPlan_t *p) {
 uint8_t b[MUL_PLAN_BYTES];return mulRangingPlanEncode(b,p)?get(b+60,4):0;
}
