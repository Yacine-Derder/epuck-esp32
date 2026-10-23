#include "mul_schedule.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void schedules(void) {
 const unsigned counts[]={2,3,4,8},slots[]={10,50,500};
 for(unsigned n=0;n<4;n++)for(unsigned k=0;k<3;k++) {
  mulSchedule_t s={.count=counts[n],.slotMs=slots[k],.periodMs=1000,.revision=1,.profile=MUL_PROFILE_MULTI};
  for(unsigned i=0;i<s.count;i++)s.ids[i]=(uint8_t)(0x91-i*7);
  bool feasible=(s.count-1)*s.slotMs+20<=630;
  assert((mulScheduleValidate(&s)==NULL)==feasible);if(!feasible)continue;
  uint8_t bytes[32];assert(mulScheduleEncode(bytes,&s));mulSchedule_t decoded;
  assert(mulScheduleDecode(bytes,&decoded)&&!memcmp(&s,&decoded,sizeof(s)));
  uint32_t fingerprint=mulScheduleFingerprint(&s);assert(fingerprint);
  for(unsigned i=0;i<s.count;i++) {
   assert(mulScheduleIndex(&s,s.ids[i])==(int)i&&mulScheduleOffsetMs(&s,i)==i*s.slotMs);
   if(i) {mulNetwork_t pair;assert(mulSchedulePair(&s,i,&pair));assert(pair.addresses[0][0]==s.ids[0]&&pair.addresses[1][0]==s.ids[i]);}
  }
  for(unsigned cycle=1;cycle<50;cycle++) {
   unsigned seen=0;
   for(unsigned w=0;w<2;w++) {
    unsigned owner=mulScheduleSyncOwner(&s,cycle,w);if(!owner)continue;
    assert(owner<s.count&&!(seen&(1u<<owner)));seen|=1u<<owner;
    assert(mulScheduleSyncEndMs(w)<=s.periodMs);
    assert(mulScheduleOffsetMs(&s,s.count-1)+20<=mulScheduleSyncPollMs(w)-20);
    if(w)assert(mulScheduleSyncPollMs(w)>=mulScheduleSyncEndMs(w-1));
   }
  }
  for(unsigned start=1;start<15;start++) {
   unsigned serviced=0;
   for(unsigned c=start;c<start+4;c++)for(unsigned w=0;w<2;w++)serviced|=1u<<mulScheduleSyncOwner(&s,c,w);
   assert((serviced&((1u<<s.count)-2))==((1u<<s.count)-2));
  }
  assert(!mulScheduleSyncOwner(&s,0,0));
  for(unsigned w=0;w<2;w++)assert(mulScheduleSyncOwner(&s,UINT32_MAX,w)<s.count);
  for(unsigned i=0;i<sizeof(bytes);i++) {
   bytes[i]^=1;mulSchedule_t original=decoded;assert(!mulScheduleDecode(bytes,&decoded)&&!memcmp(&decoded,&original,sizeof(decoded)));bytes[i]^=1;
  }
  decoded=s;decoded.revision++;assert(mulScheduleFingerprint(&decoded)!=fingerprint);
  decoded=s;decoded.ids[1]=decoded.ids[0];assert(mulScheduleValidate(&decoded));
  decoded=s;decoded.ids[0]=255;assert(mulScheduleValidate(&decoded));
  decoded=s;decoded.periodMs=999;assert(mulScheduleValidate(&decoded));
  decoded=s;decoded.slotMs=20;assert(mulScheduleValidate(&decoded));
 }
}
int main(void) {
 schedules();mulSchedule_t s;mulScheduleLegacy(&s,0x84,0x85,10);assert(!mulScheduleValidate(&s));
 for(unsigned c=0;c<30;c++)assert(mulScheduleSyncOwner(&s,c,0)==(c&&c%5==0?1u:0u));
 s.count=1;assert(mulScheduleValidate(&s));s.count=9;assert(mulScheduleValidate(&s));
 s.count=3;assert(mulScheduleValidate(&s));
 puts("Shared schedule: N2/3/4/8, compact CRC persistence, malformed/mismatch, slot guards, two-window TWR fairness and legacy cadence passed");return 0;
}
