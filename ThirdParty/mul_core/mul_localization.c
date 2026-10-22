/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_localization.h"
#include <string.h>

void mulPropagationMotionBound(uint32_t ageMs,uint32_t *millimetres,uint32_t *dtu)
{
  *millimetres=*dtu=UINT32_MAX;
#if MUL_RELATIVE_SPEED_MM_S >= 0
  if (ageMs>UINT16_MAX) return; // Bound arithmetic and reject full-uptime aliases.
  uint64_t mm=((uint64_t)MUL_RELATIVE_SPEED_MM_S*ageMs+999)/1000;
  *millimetres=(uint32_t)mm;
  *dtu=(uint32_t)((mm*UINT64_C(63897600000)+UINT64_C(299792457999))/UINT64_C(299792458000));
#else
  (void)ageMs;
#endif
}

void mulLocalizationObserveNetwork(const mulNetwork_t *network,mulLocalization_t *o,mulPair_t *pair,
  const mulRelation_t *r,const mulClock_t *rate,uint32_t now,uint32_t captureAge,bool stationary)
{
  memset(o,0,sizeof(*o));
  memcpy(o->reference,pair->reference.sourceClock,8); memcpy(o->follower,pair->follower.sourceClock,8);
  o->referenceSequence=pair->reference.source.sequence; o->followerSequence=pair->follower.source.sequence;
  o->referenceGeneration=pair->reference.generation; o->followerGeneration=pair->follower.generation;
  o->receiverTimestamp=pair->follower.source.rxTimestamp; o->observedMs=pair->follower.acceptedMs;
  o->followerEpoch=r->epoch; o->slotMs=MUL_FOLLOWER_SLOT_MS;
  o->tdoaDtu=o->rangeDifferenceMm=o->ratePpb=INT32_MIN;
  o->delayAgeMs=o->motionBoundMm=o->motionBoundDtu=UINT32_MAX;
  o->delayDtu=o->clockAgeMs=UINT16_MAX;
  o->unknownComponents=mulQualityAllUnknown;
  o->powerFlags=(pair->reference.power.status==mulPowerApplied ? 1 : 0) |
    (pair->follower.power.status==mulPowerApplied ? 2 : 0) | (r->powerCorrectedTwr ? 4 : 0);
  o->associationValid=pair->reference.valid && pair->follower.valid &&
    pair->reference.generation!=0 && pair->follower.generation!=0 &&
    mulNetworkRole(network,o->reference)==mulAlrReference && mulNetworkRole(network,o->follower)==mulAlrFollower &&
    memcmp(pair->reference.receiverClock,pair->follower.receiverClock,8)==0;
  o->delayState=r->delayState;
  if (r->version!=2 && r->available) o->delayState=mulPropagationFresh; // v1 cannot carry numeric estimate/state.
  uint32_t elapsed=now-pair->follower.acceptedMs;
  bool ageKnown=captureAge<=15 && elapsed<=MUL_PAIR_MAX_AGE_MS;
  if (ageKnown && (r->version==2 || r->available) && o->delayState!=mulPropagationNone && o->delayState!=mulPropagationInvalidated) {
    o->delayAgeMs=r->delayAgeMs+elapsed+captureAge;
    if (r->version==2) o->delayDtu=r->delayDtu;
    uint32_t limit=r->version==2 ? r->maxAgeMs : 1000;
    if (o->delayAgeMs>limit) o->delayState=mulPropagationExpired;
    mulPropagationMotionBound(o->delayAgeMs,&o->motionBoundMm,&o->motionBoundDtu);
    if (o->motionBoundMm!=UINT32_MAX) o->unknownComponents &= ~mulQualityMotionUnknown;
  }
  if (r->available && ageKnown && (uint32_t)r->originAgeMs+elapsed+captureAge<=UINT16_MAX)
    o->clockAgeMs=r->originAgeMs+elapsed+captureAge;
  if (stationary && rate && rate->hasBaseline && rate->status==mulClockValid) {
    o->rateState=mulRateStale;
    if ((uint32_t)(now-rate->observedMs)<=MUL_PAIR_MAX_AGE_MS &&
        rate->sequence==pair->reference.source.sequence && rate->referenceTx==pair->reference.source.txTimestamp &&
        rate->localRx==pair->reference.source.rxTimestamp && rate->skewPpb>=-MUL_CLOCK_MAX_PPB && rate->skewPpb<=MUL_CLOCK_MAX_PPB) {
      o->rateState=mulRateStationary; o->ratePpb=rate->skewPpb;
    }
  }
  mulDownlink_t d=mulDownlinkComputeNetwork(network,r,pair,rate,now,captureAge,stationary);
  o->status=o->associationValid ? d.status : mulDownlinkInconsistent;
  o->clockValid=o->associationValid && pair->clockRelationAvailable;
  if (o->status==mulDownlinkAvailable) {
    o->tdoaDtu=(int32_t)(d.correctedAvailable ? d.correctedTdoaRef : d.rawTdoaRef);
    o->rangeDifferenceMm=d.rangeDifferenceMm;
  }
}


/* Legacy API wrappers retain the default network. */
void mulLocalizationObserve(mulLocalization_t *observation,mulPair_t *pair,
  const mulRelation_t *relation,const mulClock_t *rate,uint32_t nowMs,
  uint32_t captureAgeMs,bool stationaryOptIn) { mulLocalizationObserveNetwork(NULL,observation,pair,relation,rate,nowMs,captureAgeMs,stationaryOptIn); }
