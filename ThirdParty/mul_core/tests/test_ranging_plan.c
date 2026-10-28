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
static void freshness(void) {
 mulRangingPlan_t p=plan(4,2,5,3,1000);p.aaMilliHz=500;p.arMilliHz=2000;p.policy=MUL_PLAN_FRESHNESS;p.aaPriority=9;p.arPriority=1;
 mulRangingBudget_t b;assert(!mulRangingPlanValidate(&p,&b)&&b.peakOperations==18);
 unsigned aa[3]={0},ar[8]={0};uint32_t last[11]={0};
 for(unsigned cycle=0;cycle<200;cycle++) {
  verify(&p,cycle);
  for(unsigned i=0;i<mulRangingPlanCount(&p,cycle);i++) {
   mulRangingOperation_t o;assert(mulRangingPlanOperation(&p,cycle,i,&o));unsigned pair;
   if(o.kind==MUL_PLAN_AA){pair=o.responder-133;aa[pair]++;}
   else {unsigned a=(o.initiator-132)*2+o.responder-141;ar[a]++;pair=3+a;}
   uint32_t at=cycle*1000+o.pollUs/1000;
   if(last[pair])assert(at-last[pair]<(pair<3?3000u:1500u));
   last[pair]=at;
  }
 }
 for(unsigned i=0;i<3;i++)assert(aa[i]==100);
 for(unsigned i=0;i<8;i++)assert(ar[i]==400);
 verify(&p,UINT32_MAX);verify(&p,0);
 uint8_t bytes[64];mulRangingPlan_t decoded;assert(mulRangingPlanEncode(bytes,&p)&&bytes[0]==3&&mulRangingPlanDecode(bytes,&decoded));
 assert(decoded.policy==1&&decoded.aaPriority==9&&decoded.arPriority==1&&mulRangingPlanFingerprint(&decoded)==mulRangingPlanFingerprint(&p));
 p=plan(2,1,5,3,1000);p.aaMilliHz=2000;p.arMilliHz=1000;p.policy=1;p.aaPriority=2;p.arPriority=1;
 mulRangingOperation_t o;assert(mulRangingPlanOperation(&p,0,0,&o)&&o.kind==MUL_PLAN_AA);
 uint32_t hash=mulRangingPlanFingerprint(&p);p.arPriority=3;assert(hash!=mulRangingPlanFingerprint(&p));
 assert(mulRangingPlanOperation(&p,0,0,&o)&&o.kind==MUL_PLAN_AR);
 p.aaMilliHz=0;verify(&p,0);p.arMilliHz=0;assert(mulRangingPlanCount(&p,0)==0);
 p.policy=0;assert(mulRangingPlanValidate(&p,NULL));p.aaPriority=p.arPriority=0;
 assert(!mulRangingPlanValidate(&p,NULL)&&mulRangingPlanEncode(bytes,&p)&&bytes[0]==2);
 for(unsigned n=2;n<=8;n++)for(unsigned r=0;r<=8;r++)for(unsigned f=0;f<4;f++) {
  p=plan(n,r,5,f,1000);p.policy=1;p.aaPriority=255;
  if(mulRangingPlanValidate(&p,NULL))continue;
  for(unsigned c=0;c<10;c++)verify(&p,c);
  verify(&p,UINT32_MAX);
 }
 puts("Freshness EDF: exact per-pair rates, bounded planned age, priority ties, conservative coincident deadlines, disabled classes, codec2 preservation/codec3 hash and cycle32 wrap passed");
}
static void startDelay(void) {
 mulRangingPlan_t p=plan(2,1,20,3,1000);
 p.broadcast.slotMs=10;p.broadcast.rxBudgetUs=5000;
 p.aaMilliHz=1000;p.arMilliHz=50;
 assert(mulRangingPlanFingerprint(&p)==0x826414cc);
 uint8_t old[64],bytes[64];assert(mulRangingPlanEncode(old,&p)&&old[0]==2);
 mulRangingOperation_t baseline,shifted,remote;
 assert(mulRangingPlanOperation(&p,1,0,&baseline));
 assert(baseline.kind==MUL_PLAN_AA&&baseline.startUs==16100&&baseline.pollUs==23100&&baseline.endUs==109200);
 p.aaStartDelayUs=50000;
 assert(!mulRangingPlanValidate(&p,NULL)&&mulRangingPlanEncode(bytes,&p)&&bytes[0]==4);
 mulRangingPlan_t follower;assert(mulRangingPlanDecode(bytes,&follower)&&follower.aaStartDelayUs==50000);
 assert(mulRangingPlanFingerprint(&p)==mulRangingPlanFingerprint(&follower)&&mulRangingPlanFingerprint(&p)!=0x826414cc);
 for(unsigned c=0;c<200;c++) {
  verify(&p,c);assert(mulRangingPlanCount(&p,c)==mulRangingPlanCount(&follower,c));
  for(unsigned i=0;i<mulRangingPlanCount(&p,c);i++) {
   mulRangingPlan_t zero=p;zero.aaStartDelayUs=0;
   assert(mulRangingPlanOperation(&zero,c,i,&baseline));
   assert(mulRangingPlanOperation(&p,c,i,&shifted)&&mulRangingPlanOperation(&follower,c,i,&remote));
   assert(!memcmp(&shifted,&remote,sizeof(remote)));
   unsigned delta=baseline.kind==MUL_PLAN_AA?50000:0;
   assert(shifted.startUs==baseline.startUs+delta&&shifted.pollUs==baseline.pollUs+delta&&shifted.endUs==baseline.endUs+delta);
  }
 }
 p.aaStartDelayUs=388850;assert(!mulRangingPlanValidate(&p,NULL));
 p.aaStartDelayUs=388851;assert(mulRangingPlanValidate(&p,NULL)); // Peak2: AA can collide with AR.
 p.aaStartDelayUs=UINT32_MAX;assert(mulRangingPlanValidate(&p,NULL));
 p.enabled=MUL_PLAN_AA;p.aaStartDelayUs=871000;assert(mulRangingPlanValidate(&p,NULL)); // Cycle end.
 p.aaStartDelayUs=0;assert(mulRangingPlanEncode(bytes,&p));
 p.enabled=3;assert(mulRangingPlanEncode(bytes,&p)&&!memcmp(old,bytes,64));
 bytes[53]=1;uint32_t crc=mulScheduleCrc(bytes,60);
 for(unsigned j=0;j<4;j++)bytes[60+j]=(uint8_t)(crc>>(8*j));
 assert(!mulRangingPlanDecode(bytes,&follower)); // Old version cannot smuggle delay.
 p.policy=MUL_PLAN_FRESHNESS;p.aaStartDelayUs=50000;
 assert(mulRangingPlanEncode(bytes,&p)&&mulRangingPlanDecode(bytes,&follower)&&follower.policy==MUL_PLAN_FRESHNESS);
 verify(&follower,UINT32_MAX);
 puts("AA delay: baseline hash/timing preserved; shared shifted windows, AR unchanged, collision/end rejection, codec4 roundtrip passed");
}
int main(void){matrix();fairness();freshness();startDelay();return 0;}
