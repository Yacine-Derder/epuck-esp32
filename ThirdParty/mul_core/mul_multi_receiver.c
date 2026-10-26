/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_multi_receiver.h"
#include <string.h>
static mulMultiValue_t invalid(mulMultiReason_t reason) {
 return (mulMultiValue_t){.rangeDifferenceMm=INT32_MIN,.tdoaDtu=INT32_MIN,.status=mulDownlinkNoRelation,.reason=(uint8_t)reason};
}
bool mulMultiReceiverInit(mulMultiReceiver_t *s,const mulSchedule_t *schedule,const uint8_t receiver[8],bool stationary) {
 if(!s||!receiver||mulScheduleValidate(schedule)||schedule->profile<MUL_PROFILE_MULTI)return false;
 mulNetwork_t first;if(!mulSchedulePair(schedule,1,&first))return false;
 if(receiver[6]!=0xcf||receiver[7]!=0xbc)return false;
 for(unsigned i=1;i<6;i++)if(receiver[i])return false;
 if(mulScheduleIndex(schedule,receiver[0])>=0)return false;
 memset(s,0,sizeof(*s));s->schedule=*schedule;s->stationary=stationary;mulClockReset(&s->rate);mulClockFitDefaults(&s->clockConfig);s->clockConfig.periodMs=schedule->periodMs;mulClockFitReset(&s->fit);
 for(unsigned i=0;i<MUL_MAX_ALRS;i++)memcpy(s->sources[i].receiverClock,receiver,8);
 return true;
}
bool mulMultiReceiverInitPlan(mulMultiReceiver_t *s,const mulRangingPlan_t *plan,const uint8_t receiver[8],bool stationary) {
 if(mulRangingPlanValidate(plan,NULL)||!mulMultiReceiverInit(s,&plan->broadcast,receiver,stationary))return false;
 s->plan=plan;return true;
}
static void begin(mulMultiReceiver_t *s,uint32_t cycle,unsigned slot,uint32_t now) {
 s->cycle=cycle;s->cycleStartMs=now-mulScheduleOffsetMs(&s->schedule,slot);s->lastCycleMs=now;
 s->seenMask=0;s->hasCycle=s->active=true;
 for(unsigned i=0;i<MUL_MAX_ALRS-1;i++)s->values[i]=invalid(mulMultiMissing);
}
static void evaluate(mulMultiReceiver_t *s,unsigned slot,uint32_t now) {
 mulMultiValue_t *v=&s->values[slot-1];
 if(!(s->seenMask&(1u<<slot))){*v=invalid(mulMultiMissing);return;}
 if(!(s->seenMask&1u)){*v=invalid(mulMultiNoReference);return;}
 if(s->captureAge[slot]>15||s->captureAge[0]>15){*v=invalid(mulMultiCaptureAge);return;}
 mulObservation_t *ref=&s->sources[0],*fol=&s->sources[slot];
 if(ref->source.sequence!=s->cycle||fol->source.sequence!=s->cycle){*v=invalid(mulMultiSequence);return;}
 mulNetwork_t pairNetwork;if(!mulSchedulePair(&s->schedule,slot,&pairNetwork)){*v=invalid(mulMultiScheduleMismatch);return;}
 uint16_t offset=mulScheduleOffsetMs(&s->schedule,slot);
 int64_t difference=mulTimestampDifference(fol->source.rxTimestamp,ref->source.rxTimestamp);
 if(difference<mulSlotMinDtu(offset)||difference>mulSlotMaxDtu(offset)){*v=invalid(mulMultiTiming);return;}
 mulPair_t pair={.reference=*ref,.follower=*fol,.rawArrivalDifferenceDtu=difference,.physicalBoundDtu=UINT64_MAX};
 mulLocalization_t observation;
 mulLocalizationObserveAtSlot(&pairNetwork,offset,&observation,&pair,&s->relations[slot-1],&s->rate,now,s->captureAge[slot],s->stationary);
 *v=(mulMultiValue_t){.rangeDifferenceMm=observation.rangeDifferenceMm,.tdoaDtu=observation.tdoaDtu,
  .status=observation.status,.clockValid=observation.clockValid,.rateState=observation.rateState};
 v->reason=observation.status==mulDownlinkAvailable?mulMultiOk:
  observation.status==mulDownlinkNoRate?mulMultiNoRate:observation.status==mulDownlinkStaleRate?mulMultiStaleRate:mulMultiClockUnavailable;
}
static void finish(mulMultiReceiver_t *s,mulMultiCycle_t *out) {
 memset(out,0,sizeof(*out));
 out->cycle=s->cycle;out->observedMs=s->cycleStartMs;out->fingerprint=s->plan?mulRangingPlanFingerprint(s->plan):mulScheduleFingerprint(&s->schedule);
 out->revision=s->schedule.revision;out->slotMs=s->schedule.slotMs;out->periodMs=s->schedule.periodMs;
 out->count=s->schedule.count;memcpy(out->ids,s->schedule.ids,MUL_MAX_ALRS);out->seenMask=s->seenMask;
 if(s->seenMask&1u)out->referenceRx=s->sources[0].source.rxTimestamp;
 for(unsigned i=1;i<s->schedule.count;i++) {
  out->values[i-1]=s->values[i-1];
  if(out->values[i-1].status==mulDownlinkAvailable)out->validMask|=(uint8_t)(1u<<i);
 }
 s->active=false;s->cycles++;
}
bool mulMultiReceiverPoll(mulMultiReceiver_t *s,uint32_t now,mulMultiCycle_t *out) {
 if(!s||!out||!s->active)return false;
 uint32_t deadline=mulScheduleOffsetMs(&s->schedule,s->schedule.count-1)+MUL_DATA_GUARD_MS;
 if((uint32_t)(now-s->cycleStartMs)<deadline)return false;
 finish(s,out);return true;
}
static mulSequenceResult_t record(mulMultiReceiver_t *s,unsigned slot,const mulTestPacket_t *p,uint64_t rx,uint32_t now) {
 mulObservation_t *o=&s->sources[slot];
 if(o->source.hasSequence&&p->sequence!=o->source.sequence&&(uint32_t)(now-o->acceptedMs)>=MUL_SOURCE_TIMEOUT_MS)o->source.hasSequence=false;
 bool first=!o->source.hasSequence;
 if(first&&o->generation==UINT32_MAX)return mulSequenceOld;
 mulSequenceResult_t result=mulObservePacket(&o->source,p,rx);
 if(result==mulSequenceOld||result==mulSequenceDuplicate)return result;
 if(first)o->generation++;
 o->acceptedMs=now;o->correctedRx=rx;o->valid=true;
 memcpy(o->sourceClock,p->source,8);o->power=(mulPower_t){.powerCentiDbm=INT16_MIN,.status=mulPowerUnsupported};
 return result;
}
bool mulMultiReceiverReceive(mulMultiReceiver_t *s,const uint8_t *frame,size_t n,uint64_t rx,uint32_t age,uint32_t now,mulMultiReceiveOutput_t *out) {
 if(!s||!frame||!out||rx>MUL_TIMESTAMP_MASK)return false;
 memset(out,0,sizeof(*out));
 if(!(s->plan?mulPlanBroadcastDecode(s->plan,frame,n,&out->broadcast):mulMultiBroadcastDecode(&s->schedule,frame,n,&out->broadcast))) {
  s->rejected++;s->mismatches++;
  if(s->active&&n==MUL_MULTI_FOLLOWER_BYTES&&frame[24]==MUL_MULTI_FOLLOWER_TYPE&&frame[34]==(s->plan?MUL_PLAN_RADIO_VERSION:(s->schedule.profile==MUL_PROFILE_EXPERIMENTAL?3:MUL_MULTI_VERSION))) {
   unsigned slot=frame[35];uint32_t cycle=0;for(unsigned i=0;i<4;i++)cycle|=(uint32_t)frame[25+i]<<(8*i);
   if(slot>0&&slot<s->schedule.count&&cycle==s->cycle&&!(s->seenMask&(1u<<slot))) {
    uint8_t id[8]={s->schedule.ids[slot],0,0,0,0,0,0xcf,0xbc};
    if(!memcmp(id,frame+13,8))s->values[slot-1]=invalid(mulMultiScheduleMismatch);
   }
  }
  return false;
 }
 unsigned slot=out->broadcast.slot;uint32_t cycle=out->broadcast.packet.sequence;
 if(s->hasCycle&&cycle!=s->cycle) {
  uint32_t delta=cycle-s->cycle;
  if(delta>=UINT32_C(0x80000000)) {
   if((uint32_t)(now-s->lastCycleMs)<MUL_SOURCE_TIMEOUT_MS){s->old++;return false;}
   // Explicit liveness rebase after reference restart; never retain an old rate.
   mulClockReset(&s->rate);for(unsigned i=0;i<s->schedule.count;i++){s->sources[i].source.hasSequence=false;s->sources[i].valid=false;}
  }
  if(s->active){finish(s,&out->completed);out->cycleReady=true;}
  begin(s,cycle,slot,now);
 } else if(!s->hasCycle)begin(s,cycle,slot,now);
 else if(!s->active){s->late++;return false;}
 if(s->seenMask&(1u<<slot)){s->duplicates++;return false;}
 mulSequenceResult_t order=record(s,slot,&out->broadcast.packet,rx,now);
 if(order==mulSequenceDuplicate||order==mulSequenceOld){s->old++;return false;}
 s->received++;s->seenMask|=(uint8_t)(1u<<slot);s->captureAge[slot]=age>UINT16_MAX?UINT16_MAX:(uint16_t)age;
 if(!slot) {
  s->cycleStartMs=now;
  mulNetwork_t pair;(void)mulSchedulePair(&s->schedule,1,&pair);
  if(order==mulSequenceFirst)mulClockReset(&s->rate);
  if(age<=15)mulClockObserveFitted(&pair,&s->rate,&s->fit,&s->clockConfig,&out->broadcast.packet,rx,now,true,NULL);
  else mulClockReset(&s->rate);
  for(unsigned i=1;i<s->schedule.count;i++)evaluate(s,i,now);
 } else {
  const mulRelation_t *r=&out->broadcast.relation;
  if(s->followerEpoch[slot-1]&&s->followerEpoch[slot-1]!=r->epoch) {
   if(s->sources[slot].generation==UINT32_MAX){s->sources[slot].valid=false;s->values[slot-1]=invalid(mulMultiSequence);return true;}
   s->sources[slot].generation++;
  }
  s->followerEpoch[slot-1]=r->epoch;s->relations[slot-1]=*r;s->sources[slot].remoteEpochKnown=r->epoch!=0;
  evaluate(s,slot,now);
 }
 return true;
}
