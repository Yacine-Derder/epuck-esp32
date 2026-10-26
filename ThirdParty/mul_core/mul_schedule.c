/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_schedule.h"
#include <string.h>
static void put(uint8_t *p,uint32_t v,unsigned n){for(unsigned i=0;i<n;i++)p[i]=(uint8_t)(v>>(8*i));}
static uint32_t get(const uint8_t *p,unsigned n){uint32_t v=0;for(unsigned i=0;i<n;i++)v|=(uint32_t)p[i]<<(8*i);return v;}
uint32_t mulScheduleCrc(const uint8_t *p,size_t n) {
 uint32_t crc=UINT32_MAX;
 for(size_t i=0;i<n;i++){crc^=p[i];for(unsigned j=0;j<8;j++)crc=(crc>>1)^((0u-(crc&1u))&UINT32_C(0xedb88320));}
 return ~crc;
}
void mulScheduleLegacy(mulSchedule_t *s,uint8_t ref,uint8_t fol,uint16_t slot) {
 *s=(mulSchedule_t){.revision=1,.slotMs=slot,.periodMs=1000,.count=2,.profile=MUL_PROFILE_LEGACY,.ids={ref,fol}};
}
void mulScheduleExperimental(mulSchedule_t *s) {
 s->profile=MUL_PROFILE_EXPERIMENTAL;s->txLeadUs=5000;s->rxBudgetUs=2000;s->airtimeUs=1000;s->guard100us=1;s->rangingFlags=MUL_RANGING_ALR_ALR;
}
uint16_t mulScheduleReplyMs(const mulSchedule_t *s) {
 if(!s||s->profile!=MUL_PROFILE_EXPERIMENTAL)return 20;
 switch(s->rangingFlags&MUL_RANGING_REPLY_MASK){case 0:return 20;case 2:return 10;case 4:return 5;default:return 0;}
}
uint16_t mulScheduleLeadUs(const mulSchedule_t *s){return s&&s->profile==MUL_PROFILE_EXPERIMENTAL?s->txLeadUs:5000;}
bool mulScheduleAlrRanging(const mulSchedule_t *s){return s&&s->profile!=MUL_PROFILE_EXPERIMENTAL?true:s&&(s->rangingFlags&MUL_RANGING_ALR_ALR);}
const char *mulScheduleValidate(const mulSchedule_t *s) {
 if(!s||s->count<2||s->count>MUL_MAX_ALRS)return "ALR count: 2..8";
 if(s->profile!=MUL_PROFILE_LEGACY&&s->profile!=MUL_PROFILE_MULTI&&s->profile!=MUL_PROFILE_EXPERIMENTAL)return "schedule profile/version";
 if(s->profile==MUL_PROFILE_LEGACY&&s->count!=2)return "legacy profile requires two ALRs";
 if(!s->revision)return "schedule revision: nonzero";
 if(!mulSlotValid(s->slotMs))return "slot_ms: 1..65535";
 if(s->profile!=MUL_PROFILE_EXPERIMENTAL) {
  if(!mulScheduleTag(s->slotMs))return "compat slot_ms: 10, 50 or 500";
  if(s->periodMs!=MUL_SCHEDULE_CYCLE_MS)return "compat cycle_ms: 1000";
  if(s->txLeadUs||s->rxBudgetUs||s->airtimeUs||s->guard100us||s->rangingFlags)return "unexpected compatibility timing fields";
 }else {
  if(s->periodMs<100||s->periodMs>2000||s->periodMs%10)return "cycle_ms: 100..2000, step10";
  if(s->txLeadUs<500||s->txLeadUs>20000)return "tx_lead_us: 500..20000 experimental";
  if(s->rxBudgetUs<100||s->rxBudgetUs>20000)return "rx_budget_us: 100..20000";
  if(s->airtimeUs<400||s->airtimeUs>20000)return "airtime_us: 400..20000";
  if(!s->guard100us||s->guard100us>100)return "guard_us: 100..10000, step100";
  if((s->rangingFlags&~(MUL_RANGING_ALR_ALR|MUL_RANGING_REPLY_MASK))||!mulScheduleReplyMs(s))return "unsupported ranging/reply flags";
  if(mulScheduleAlrRanging(s)&&(uint32_t)mulScheduleReplyMs(s)*1000<s->txLeadUs+s->rxBudgetUs+s->airtimeUs+(uint32_t)s->guard100us*100)return "reply below lead+processing+airtime+guard";
  if((uint32_t)s->slotMs*1000<s->txLeadUs+s->rxBudgetUs+s->airtimeUs+(uint32_t)s->guard100us*100)return "spacing below lead+processing+airtime+guard";
  if(mulScheduleAlrRanging(s)&&(uint32_t)(s->count-1)*s->slotMs*1000+s->airtimeUs+(uint32_t)s->guard100us*100+s->rxBudgetUs>630000)return "data airtime overlaps AA preparation";
  if(mulScheduleAlrRanging(s)&&s->periodMs<1000)return "AA compatibility windows require cycle>=1000";
  if((uint32_t)(s->count-1)*s->slotMs+MUL_DATA_GUARD_MS+20>s->periodMs)return "broadcasts overlap next reference preparation";
 }
 for(unsigned i=0;i<MUL_MAX_ALRS;i++) {
  if(i>=s->count){if(s->ids[i])return "unused roster entries must be zero";continue;}
  if(s->ids[i]==255)return "ALR ID: 0..254";
  for(unsigned j=0;j<i;j++)if(s->ids[i]==s->ids[j])return "duplicate ALR identity";
 }
 if(mulScheduleAlrRanging(s)&&(uint32_t)(s->count-1)*s->slotMs+MUL_DATA_GUARD_MS>630)return "data slots overlap synchronization guard";
 return NULL;
}
int mulScheduleIndex(const mulSchedule_t *s,uint8_t id) {
 if(mulScheduleValidate(s))return -1;
 for(unsigned i=0;i<s->count;i++)if(s->ids[i]==id)return (int)i;
 return -1;
}
bool mulSchedulePair(const mulSchedule_t *s,unsigned follower,mulNetwork_t *pair) {
 if(!pair||mulScheduleValidate(s)||!follower||follower>=s->count)return false;
 mulNetwork_t candidate;if(!mulNetworkInit(&candidate,s->ids[0],s->ids[follower]))return false;
 /* Pair timing can exceed a base spacing; multi reconstruction uses its own
  * slot-index windows and passes a base-spacing context into legacy clock math. */
 candidate.slotMs=s->slotMs;*pair=candidate;return true;
}
bool mulScheduleEncode(uint8_t out[MUL_SCHEDULE_BYTES],const mulSchedule_t *s) {
 if(!out||mulScheduleValidate(s))return false;
 uint8_t b[MUL_SCHEDULE_BYTES]={0};b[0]=s->profile==MUL_PROFILE_EXPERIMENTAL?2:MUL_SCHEDULE_VERSION;b[1]=s->profile;b[2]=s->count;
 put(b+4,s->slotMs,2);put(b+6,s->periodMs,2);put(b+8,s->revision,4);memcpy(b+12,s->ids,MUL_MAX_ALRS);
 if(s->profile==MUL_PROFILE_EXPERIMENTAL){put(b+20,s->txLeadUs,2);put(b+22,s->rxBudgetUs,2);put(b+24,s->airtimeUs,2);b[26]=s->guard100us;b[27]=s->rangingFlags;}
 put(b+28,mulScheduleCrc(b,28),4);memcpy(out,b,sizeof(b));return true;
}
bool mulScheduleDecode(const uint8_t in[MUL_SCHEDULE_BYTES],mulSchedule_t *s) {
 if(!in||!s||(in[0]!=MUL_SCHEDULE_VERSION&&in[0]!=2)||in[3]||get(in+28,4)!=mulScheduleCrc(in,28))return false;
 if((in[0]==2)!=(in[1]==MUL_PROFILE_EXPERIMENTAL))return false;
 if(in[0]==1)for(unsigned i=20;i<28;i++)if(in[i])return false;
 mulSchedule_t candidate={.profile=in[1],.count=in[2],.slotMs=(uint16_t)get(in+4,2),.periodMs=(uint16_t)get(in+6,2),.revision=get(in+8,4)};
 if(in[0]==2){candidate.txLeadUs=(uint16_t)get(in+20,2);candidate.rxBudgetUs=(uint16_t)get(in+22,2);candidate.airtimeUs=(uint16_t)get(in+24,2);candidate.guard100us=in[26];candidate.rangingFlags=in[27];}
 memcpy(candidate.ids,in+12,MUL_MAX_ALRS);if(mulScheduleValidate(&candidate))return false;*s=candidate;return true;
}
uint32_t mulScheduleFingerprint(const mulSchedule_t *s) {
 uint8_t b[MUL_SCHEDULE_BYTES];return mulScheduleEncode(b,s)?get(b+28,4):0;
}
uint16_t mulScheduleOffsetMs(const mulSchedule_t *s,unsigned slot) {
 return mulScheduleValidate(s)||slot>=s->count?UINT16_MAX:(uint16_t)(slot*s->slotMs);
}
unsigned mulScheduleSyncOwner(const mulSchedule_t *s,uint32_t cycle,unsigned window) {
 if(mulScheduleValidate(s)||!mulScheduleAlrRanging(s)||!cycle||window>=MUL_SYNC_WINDOWS)return 0;
 if(s->profile==MUL_PROFILE_LEGACY)return window==0&&cycle%5==0?1:0;
 /* Compute modulo before multiply, including uint32 cycle rollover. */
 unsigned followers=s->count-1,first=((cycle%followers+followers-1)%followers)*2%followers;
 if(window>=followers)return 0;
 return 1+(first+window)%followers;
}
uint16_t mulScheduleSyncPollMs(unsigned window){return window<MUL_SYNC_WINDOWS?(uint16_t)(650+150*window):UINT16_MAX;}
uint16_t mulScheduleSyncEndMs(unsigned window){return window<MUL_SYNC_WINDOWS?(uint16_t)(800+150*window):UINT16_MAX;}
