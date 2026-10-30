/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_hybrid.h"
#include <string.h>
const char *mulHybridReasonName(mulHybridReason_t reason) {
 static const char *const names[]={"shape","address","decode","no_reference",
 "sequence","window","missing_twr","prefix","timestamp","arithmetic",
 "capture_age","source_order","epoch","missing_relation","stale_relation","missing_propagation","slot_mismatch"};
 return (unsigned)reason<mulHybridReasonCount?names[reason]:"none";
}
void mulHybridDiagnostic(mulHybridDiagnostics_t *d,mulHybridReason_t reason,
 uint8_t type,uint32_t id,uint8_t stage) {
 if((unsigned)reason>=mulHybridReasonCount)return;
 d->reason[reason]++;d->lastReason=reason;d->lastType=type;d->lastId=id;d->lastStage=stage;d->hasFailure=true;
}
bool mulHybridInitNetwork(mulHybrid_t *s,const mulNetwork_t *network,const uint8_t receiver[8],bool stationary) {
 if(!s)return false;
 memset(s,0,sizeof(*s));
 if(!receiver||!mulNetworkValid(network)||mulNetworkRole(network,receiver)!=mulAlrUnsupported)return false;
 s->network=network?*network:mulDefaultNetwork;s->stationary=stationary;
 mulObservationInit(s->observations,receiver);mulClockReset(&s->rate);mulClockFitDefaults(&s->clockConfig);mulClockFitReset(&s->fit);return true;
}
void mulHybridInit(mulHybrid_t *s,const uint8_t receiver[8],bool stationary) {
 (void)mulHybridInitNetwork(s,NULL,receiver,stationary);
}

void mulHybridExpire(mulHybrid_t *s,uint32_t now) {
 mulAssociationExpireNetwork(&s->network,&s->association,s->observations,now);
 if(s->rate.hasBaseline&&(uint32_t)(now-s->rate.observedMs)>=MUL_SOURCE_TIMEOUT_MS&&!mulClockFitHoldover(&s->rate,&s->fit,&s->clockConfig,now))mulClockReset(&s->rate);
 if(s->twrActive&&(uint32_t)(now-s->twrStartedMs)>=200U){s->twrActive=false;s->twrTimeouts++;}
}
static bool twrObserve(mulHybrid_t *s,const mulTwrPacket_t *p,uint64_t rx,uint32_t now,mulHybridOutput_t *out) {
 mulHybridReason_t reason=mulHybridNoReference;
 const mulObservation_t *ref=&s->observations[mulAlrReference];
 int64_t phase=mulTimestampDifference(rx,ref->source.rxTimestamp);
 if(!mulObservationFresh(ref,now))goto rejected;
 reason=mulHybridSequence;if(p->id!=ref->source.sequence)goto rejected;
 reason=mulHybridWindow;if(phase<600*INT64_C(63897600)||phase>800*INT64_C(63897600))goto rejected;
 if(p->type==mulTwrPoll) {
   reason=mulHybridSequence;
   if(s->twrSeen&&p->id==s->lastTwrId&&s->twrGeneration==ref->generation)goto rejected;
   reason=mulHybridTimestamp;
   if(p->id%5 || mulTimestampDifference(p->stamps[0],ref->source.txTimestamp)<600*INT64_C(63897600)||
      mulTimestampDifference(p->stamps[0],ref->source.txTimestamp)>700*INT64_C(63897600))goto rejected;
   s->twr=*p;s->twrStartedMs=now;s->twrGeneration=ref->generation;s->lastTwrId=p->id;s->twrSeen=s->twrActive=true;return true;
 }
 reason=mulHybridMissingTwr;if(!s->twrActive)goto rejected;
 reason=mulHybridSequence;if(s->twrGeneration!=ref->generation||s->twr.id!=p->id)goto rejected;
 reason=mulHybridMissingTwr;if(p->type!=s->twr.type+1)goto rejected;
 reason=mulHybridPrefix;
 unsigned used=s->twr.type==mulTwrPoll?1:s->twr.type==mulTwrResponse?3:s->twr.type==mulTwrFinal?5:6;
 if(memcmp(p->stamps,s->twr.stamps,used*sizeof(uint64_t)))goto rejected;
 for(unsigned i=0;i<used/2;i++) {
   const mulPower_t *a=&p->power[i],*b=&s->twr.power[i];
   if(a->powerCentiDbm!=b->powerCentiDbm||a->deltaDtu!=b->deltaDtu||a->status!=b->status||a->table!=b->table)goto rejected;
 }
 if(p->type==mulTwrResult) {
   int64_t report=mulTimestampDifference(s->twr.txTimestamp,p->stamps[5]);
   int64_t result=mulTimestampDifference(p->txTimestamp,p->stamps[4]);
   reason=mulHybridTimestamp;
   if(report<15*INT64_C(63897600)||report>30*INT64_C(63897600)||result<30*INT64_C(63897600)||result>60*INT64_C(63897600))goto rejected;
   reason=mulHybridArithmetic;
   uint32_t raw,corrected;if(!mulTwrResults(p->stamps,p->power,&raw,&corrected)||p->tof!=(corrected==UINT32_MAX?raw:corrected))goto rejected;
   out->twrRaw=raw;out->twrCorrected=corrected;out->twrComplete=true;s->twrCompleted++;s->twrActive=false;
 }
 s->twr=*p;return true;
rejected:s->twrRejected++;mulHybridDiagnostic(&s->diagnostics,reason,p->type,p->id,s->twrActive?s->twr.type:0);return false;
}
bool mulHybridReceive(mulHybrid_t *s,const uint8_t *f,size_t n,uint64_t rx,uint32_t age,uint32_t now,mulHybridOutput_t *out) {
 if(!s||!f||!out||rx>MUL_TIMESTAMP_MASK||!mulNetworkValid(&s->network))return false;
 const mulNetwork_t *network=&s->network;
 mulHybridExpire(s,now);memset(out,0,sizeof(*out));
 if(n==MUL_TWR_FRAME_LENGTH) {
   mulTwrPacket_t p;if(!mulTwrDecodeNetwork(network,f,n,&p)){
     mulAlrRole_t src=mulNetworkRole(network,f+13),dst=mulNetworkRole(network,f+5);
     bool known=f[24]>=mulTwrPoll&&f[24]<=mulTwrResult;
     mulAlrRole_t expected=f[24]==mulTwrResponse||f[24]==mulTwrReport?mulAlrFollower:mulAlrReference;
     mulHybridDiagnostic(&s->diagnostics,src==mulAlrUnsupported||dst==mulAlrUnsupported||src==dst||(known&&src!=expected)?mulHybridAddress:mulHybridDecode,f[24],0,0);
     s->rejected++;return false;
   }
   s->diagnostics.twrFrames[p.type-mulTwrPoll]++;
   s->received++; /* Capture decoded frames even if a prior TWR packet was lost. */
   if(age<=15)(void)twrObserve(s,&p,rx,now,out);else {s->twrRejected++;mulHybridDiagnostic(&s->diagnostics,mulHybridCaptureAge,p.type,p.id,0);}
   out->packet.sequence=p.id;memcpy(out->packet.source,f+13,8);
 } else {
   mulTestPacket_t p;mulRelation_t r;
   if(n!=MUL_TEST_FRAME_LENGTH&&n!=MUL_RELATION_FRAME_LENGTH) {
     s->rejected++;mulHybridDiagnostic(&s->diagnostics,mulHybridShape,0,0,0);return false;
   }
   if(!mulBroadcastDecodeNetwork(network,f,n,&p,&r)||mulNetworkRole(network,p.source)==mulAlrUnsupported){s->rejected++;
     mulHybridDiagnostic(&s->diagnostics,mulNetworkRole(network,f+13)==mulAlrUnsupported?mulHybridAddress:mulHybridDecode,f[24],0,0);return false;}
   if(!mulNetworkScheduleMatches(network,p.slotMs)) {
     s->rejected++;s->association.pending=false;mulClockReset(&s->rate);
     mulHybridDiagnostic(&s->diagnostics,mulHybridScheduleMismatch,f[24],p.sequence,0);return false;
   }
   if(n==MUL_RELATION_FRAME_LENGTH){s->diagnostics.relations++;if(r.available)s->diagnostics.availableRelations++;}
   s->received++;out->packet=p;out->relation=r;out->broadcast=true;
   mulAlrRole_t role=mulNetworkRole(network,p.source);
   mulPowerRx_t capture={.raw=rx,.corrected=rx,.power={.powerCentiDbm=INT16_MIN,.status=mulPowerUnsupported}};
   mulPair_t pair;bool hadFollower=s->observations[1].source.hasSequence;
   bool associated=mulAssociateNetwork(network,&s->association,s->observations,&p,&capture,now,&out->order,&pair);
   if(out->order==mulSequenceDuplicate||out->order==mulSequenceOld)
     mulHybridDiagnostic(&s->diagnostics,mulHybridSourceOrder,f[24],p.sequence,0);
   mulObservation_t *o=&s->observations[role];
   if(role==mulAlrFollower&&out->order!=mulSequenceDuplicate&&out->order!=mulSequenceOld) {
     if(hadFollower&&r.epoch!=s->relation.epoch) {
       mulHybridDiagnostic(&s->diagnostics,mulHybridEpoch,f[24],p.sequence,0);
       associated=false;s->association.pending=false;mulClockReset(&s->rate);
       if(o->generation==UINT32_MAX)o->valid=false;else o->generation++;
     }
     s->relation=r;o->remoteEpochKnown=r.epoch!=0;
   }
   if(role==mulAlrReference&&out->order!=mulSequenceDuplicate&&out->order!=mulSequenceOld) {
     if(out->order==mulSequenceFirst&&!mulClockFitHoldover(&s->rate,&s->fit,&s->clockConfig,now))mulClockReset(&s->rate);
     mulClockObserveFitted(network,&s->rate,&s->fit,&s->clockConfig,&p,rx,now,true,NULL);
   }
   if(associated) {
     pair.follower.remoteEpochKnown=o->remoteEpochKnown;
     mulLocalizationObserveNetwork(network,&out->observation,&pair,&r,&s->rate,now,age,s->stationary);
     if(!r.available)mulHybridDiagnostic(&s->diagnostics,mulHybridMissingRelation,f[24],p.sequence,0);
     else if(!out->observation.clockValid)mulHybridDiagnostic(&s->diagnostics,mulHybridStaleRelation,f[24],p.sequence,0);
     if(r.delayState==mulPropagationNone||r.delayState==mulPropagationExpired||r.delayState==mulPropagationInvalidated)
       mulHybridDiagnostic(&s->diagnostics,mulHybridMissingPropagation,f[24],p.sequence,0);
     out->associated=true;s->pairs++;
   }
 }
 out->referenceGeneration=s->observations[0].generation;out->followerGeneration=s->observations[1].generation;
 return true;
}
