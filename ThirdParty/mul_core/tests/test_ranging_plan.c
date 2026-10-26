#include "mul_ranging_plan.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static mulRangingPlan_t plan(unsigned alrs,unsigned rlrs,unsigned reply,unsigned flags,unsigned period) {
 mulRangingPlan_t p={.replyMs=reply,.enabled=flags,.maxUtilization=70,.rlrCount=rlrs,.aaMilliHz=100,.arMilliHz=100};
 mulScheduleLegacy(&p.broadcast,132,133,10);mulScheduleExperimental(&p.broadcast);
 p.broadcast.count=alrs;p.broadcast.slotMs=3;p.broadcast.periodMs=period;
 p.broadcast.txLeadUs=1000;p.broadcast.rxBudgetUs=500;p.broadcast.rangingFlags=0;
 for(unsigned i=0;i<alrs;i++)p.broadcast.ids[i]=132+i;
 for(unsigned i=0;i<rlrs;i++)p.rlrs[i]=141+i;
 return p;
}
static void verify(const mulRangingPlan_t *p,uint32_t cycle) {
 mulRangingBudget_t b;assert(!mulRangingPlanValidate(p,&b));
 unsigned count=mulRangingPlanCount(p,cycle);assert(count<=b.capacity&&count<=b.peakOperations);
 uint32_t last=b.dataEndUs;
 for(unsigned i=0;i<count;i++) {
  mulRangingOperation_t o,a;assert(mulRangingPlanOperation(p,cycle,i,&o));
  assert(mulRangingPlanOperation(p,cycle,i,&a)&&!memcmp(&o,&a,sizeof(o)));
  assert(o.startUs>=last&&o.pollUs>=o.startUs+b.prepareUs&&o.endUs==o.startUs+b.reservationUs&&o.endUs<=b.usableEndUs);
  assert(mulScheduleIndex(&p->broadcast,o.initiator)>=0&&o.initiator!=o.responder);
  if(o.kind==MUL_PLAN_AA)assert((p->enabled&MUL_PLAN_AA)&&o.initiator==p->broadcast.ids[0]&&mulScheduleIndex(&p->broadcast,o.responder)>0);
  else {assert(o.kind==MUL_PLAN_AR&&(p->enabled&MUL_PLAN_AR));bool found=false;for(unsigned j=0;j<p->rlrCount;j++)if(p->rlrs[j]==o.responder)found=true;assert(found);}
  last=o.endUs;
 }
 assert(b.dataEndUs+(uint64_t)count*b.reservationUs<=(uint64_t)p->broadcast.periodMs*1000*p->maxUtilization/100);
 mulRangingOperation_t untouched={.startUs=42};assert(!mulRangingPlanOperation(p,cycle,count,&untouched)&&untouched.startUs==42);
}
static void matrix(void) {
 unsigned feasible=0,rejected=0;
 const unsigned counts[]={2,3,4,5,8},rlrs[]={0,1,2,8},replies[]={5,10,20},periods[]={100,200,500,1000,2000};
 for(unsigned a=0;a<5;a++)for(unsigned r=0;r<4;r++)for(unsigned d=0;d<3;d++)for(unsigned t=0;t<5;t++)for(unsigned flags=0;flags<4;flags++) {
  mulRangingPlan_t p=plan(counts[a],rlrs[r],replies[d],flags,periods[t]);
  if(mulRangingPlanValidate(&p,NULL)){rejected++;continue;}feasible++;
  uint8_t bytes[MUL_PLAN_BYTES];mulRangingPlan_t decoded;
  assert(mulRangingPlanEncode(bytes,&p)&&mulRangingPlanDecode(bytes,&decoded));
  assert(mulRangingPlanFingerprint(&p)==mulRangingPlanFingerprint(&decoded));
  for(unsigned cycle=0;cycle<25;cycle++)verify(&p,cycle);
  verify(&p,UINT32_MAX);verify(&p,0);
  for(unsigned i=0;i<MUL_PLAN_BYTES;i++){bytes[i]^=1;assert(!mulRangingPlanDecode(bytes,&decoded));bytes[i]^=1;}
 }
 assert(feasible>100&&rejected>100);
 printf("Ranging planner matrix: %u feasible and %u rejected profiles; no overlap, exact owners, explicit timing/utilization limits, bounded codec and cycle32 wrap passed\n",feasible,rejected);
}
static void fairness(void) {
 mulRangingPlan_t p=plan(4,2,5,3,1000);p.aaMilliHz=500;p.arMilliHz=2000;
 mulRangingBudget_t b;assert(!mulRangingPlanValidate(&p,&b));
 unsigned aa[3]={0},ar[8]={0};uint32_t lastAa[3]={0},lastAr[8]={0},maxGap=0;
 for(unsigned cycle=0;cycle<2000;cycle++) {
  verify(&p,cycle);
  for(unsigned i=0;i<mulRangingPlanCount(&p,cycle);i++) {
   mulRangingOperation_t o;assert(mulRangingPlanOperation(&p,cycle,i,&o));
   uint32_t at=cycle*1000+o.pollUs/1000;
   if(o.kind==MUL_PLAN_AA){unsigned pair=o.responder-133;if(aa[pair]){uint32_t gap=at-lastAa[pair];if(gap>maxGap)maxGap=gap;}aa[pair]++;lastAa[pair]=at;}
   else {unsigned pair=(o.initiator-132)*2+o.responder-141;ar[pair]++;lastAr[pair]=at;}
  }
 }
 for(unsigned i=0;i<3;i++)assert(aa[i]==1000);
 for(unsigned i=0;i<8;i++)assert(ar[i]==4000&&lastAr[i]>1999000);
 assert(maxGap<=2200); // Requested AA freshness2s plus cycle placement quantization.
 p.enabled=0;for(unsigned i=0;i<100;i++)assert(!mulRangingPlanCount(&p,i));
 p.enabled=3;p.replyMs=20;assert(mulRangingPlanValidate(&p,NULL)); // Cannot promise17.5 exchanges/s at20ms replies.
 p=plan(4,2,5,3,1000);p.broadcast.txLeadUs=5000;assert(mulRangingPlanValidate(&p,NULL));
 p.broadcast.txLeadUs=1000;p.rlrs[1]=p.rlrs[0];assert(mulRangingPlanValidate(&p,NULL));
 p.rlrs[1]=132;assert(mulRangingPlanValidate(&p,NULL));
 p=plan(4,2,5,3,1000);p.broadcast.rangingFlags=1;assert(mulRangingPlanValidate(&p,NULL));
 p=plan(4,2,5,3,1000);uint8_t bytes[MUL_PLAN_BYTES];assert(mulRangingPlanEncode(bytes,&p));
 const unsigned offsets[]={0,1,2,36,38,45,50};
 const uint8_t invalid[]={3,4,4,9,141,1,1};
 for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
  uint8_t bad[MUL_PLAN_BYTES];memcpy(bad,bytes,sizeof(bad));bad[offsets[i]]=invalid[i];
  uint32_t crc=mulScheduleCrc(bad,60);for(unsigned j=0;j<4;j++)bad[60+j]=(uint8_t)(crc>>(8*j));
  mulRangingPlan_t unchanged=p;assert(!mulRangingPlanDecode(bad,&unchanged));
  assert(!memcmp(&unchanged,&p,sizeof(p)));
 }
 puts("Ranging planner fairness: AA0.5Hz and AR2Hz per pair, all8 AR links, deterministic fractional service, bounded freshness and infeasible20ms rejection passed");
}
int main(void){matrix();fairness();return 0;}
