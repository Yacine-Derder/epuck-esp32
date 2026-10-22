/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_hybrid.h"
#include <string.h>
void mulHybridInit(mulHybrid_t *s,const uint8_t receiver[8],bool stationary) {
 memset(s,0,sizeof(*s));s->stationary=stationary;
 mulObservationInit(s->observations,receiver);mulClockReset(&s->rate);
}
void mulHybridExpire(mulHybrid_t *s,uint32_t now) {
 mulAssociationExpire(&s->association,s->observations,now);
 if(s->rate.hasBaseline&&(uint32_t)(now-s->rate.observedMs)>=MUL_SOURCE_TIMEOUT_MS)mulClockReset(&s->rate);
 if(s->twrActive&&(uint32_t)(now-s->twrStartedMs)>=200U){s->twrActive=false;s->twrTimeouts++;}
}
static bool twrObserve(mulHybrid_t *s,const mulTwrPacket_t *p,uint64_t rx,uint32_t now,mulHybridOutput_t *out) {
 const mulObservation_t *ref=&s->observations[mulAlrReference];
 int64_t phase=mulTimestampDifference(rx,ref->source.rxTimestamp);
 if(!mulObservationFresh(ref,now)||p->id!=ref->source.sequence||phase<600*INT64_C(63897600)||phase>800*INT64_C(63897600))goto rejected;
 if(p->type==mulTwrPoll) {
   if(s->twrSeen&&p->id==s->lastTwrId&&s->twrGeneration==ref->generation)goto rejected;
   if(p->id%5 || mulTimestampDifference(p->stamps[0],ref->source.txTimestamp)<600*INT64_C(63897600)||
      mulTimestampDifference(p->stamps[0],ref->source.txTimestamp)>700*INT64_C(63897600))goto rejected;
   s->twr=*p;s->twrStartedMs=now;s->twrGeneration=ref->generation;s->lastTwrId=p->id;s->twrSeen=s->twrActive=true;return true;
 }
 if(!s->twrActive||s->twrGeneration!=ref->generation||s->twr.id!=p->id||p->type!=s->twr.type+1)goto rejected;
 unsigned used=s->twr.type==mulTwrPoll?1:s->twr.type==mulTwrResponse?3:s->twr.type==mulTwrFinal?5:6;
 if(memcmp(p->stamps,s->twr.stamps,used*sizeof(uint64_t)))goto rejected;
 for(unsigned i=0;i<used/2;i++) {
   const mulPower_t *a=&p->power[i],*b=&s->twr.power[i];
   if(a->powerCentiDbm!=b->powerCentiDbm||a->deltaDtu!=b->deltaDtu||a->status!=b->status||a->table!=b->table)goto rejected;
 }
 if(p->type==mulTwrResult) {
   int64_t report=mulTimestampDifference(s->twr.txTimestamp,p->stamps[5]);
   int64_t result=mulTimestampDifference(p->txTimestamp,p->stamps[4]);
   if(report<15*INT64_C(63897600)||report>30*INT64_C(63897600)||result<30*INT64_C(63897600)||result>60*INT64_C(63897600))goto rejected;
   uint32_t raw,corrected;if(!mulTwrResults(p->stamps,p->power,&raw,&corrected)||p->tof!=(corrected==UINT32_MAX?raw:corrected))goto rejected;
   out->twrRaw=raw;out->twrCorrected=corrected;out->twrComplete=true;s->twrCompleted++;s->twrActive=false;
 }
 s->twr=*p;return true;
rejected:s->twrRejected++;return false;
}
bool mulHybridReceive(mulHybrid_t *s,const uint8_t *f,size_t n,uint64_t rx,uint32_t age,uint32_t now,mulHybridOutput_t *out) {
 if(!s||!f||!out||rx>MUL_TIMESTAMP_MASK)return false;
 mulHybridExpire(s,now);memset(out,0,sizeof(*out));
 if(n==MUL_TWR_FRAME_LENGTH) {
   mulTwrPacket_t p;if(!mulTwrDecode(f,n,&p)){s->rejected++;return false;}
   s->received++; /* Capture decoded frames even if a prior TWR packet was lost. */
   if(age<=15)(void)twrObserve(s,&p,rx,now,out);else s->twrRejected++;
   out->packet.sequence=p.id;memcpy(out->packet.source,f+13,8);
 } else {
   mulTestPacket_t p;mulRelation_t r;
   if(!mulBroadcastDecode(f,n,&p,&r)||mulAlrRoleForAddress(p.source)==mulAlrUnsupported){s->rejected++;return false;}
   s->received++;out->packet=p;out->relation=r;out->broadcast=true;
   mulAlrRole_t role=mulAlrRoleForAddress(p.source);
   mulPowerRx_t capture={.raw=rx,.corrected=rx,.power={.powerCentiDbm=INT16_MIN,.status=mulPowerUnsupported}};
   mulPair_t pair;bool hadFollower=s->observations[1].source.hasSequence;
   bool associated=mulAssociate(&s->association,s->observations,&p,&capture,now,&out->order,&pair);
   mulObservation_t *o=&s->observations[role];
   if(role==mulAlrFollower&&out->order!=mulSequenceDuplicate&&out->order!=mulSequenceOld) {
     if(hadFollower&&r.epoch!=s->relation.epoch) {
       associated=false;s->association.pending=false;mulClockReset(&s->rate);
       if(o->generation==UINT32_MAX)o->valid=false;else o->generation++;
     }
     s->relation=r;o->remoteEpochKnown=r.epoch!=0;
   }
   if(role==mulAlrReference&&out->order!=mulSequenceDuplicate&&out->order!=mulSequenceOld) {
     if(out->order==mulSequenceFirst)mulClockReset(&s->rate);
     mulClockObserve(&s->rate,&p,rx,now,true);
   }
   if(associated) {
     pair.follower.remoteEpochKnown=o->remoteEpochKnown;
     mulLocalizationObserve(&out->observation,&pair,&r,&s->rate,now,age,s->stationary);
     out->associated=true;s->pairs++;
   }
 }
 out->referenceGeneration=s->observations[0].generation;out->followerGeneration=s->observations[1].generation;
 return true;
}
