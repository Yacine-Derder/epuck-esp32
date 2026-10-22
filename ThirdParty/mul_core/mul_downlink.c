/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_downlink.h"
#include <string.h>
bool mulDownlinkRelation(const mulRelation_t *r,mulPair_t *pair,
  uint32_t nowMs,uint32_t captureAgeMs,int64_t *txSeparation)
{
  pair->clockRelationAvailable=false; pair->geometricTdoaAvailable=false;
  if (!r->available || captureAgeMs>15 ||
      !mulObservationFresh(&pair->reference,nowMs) || !mulObservationFresh(&pair->follower,nowMs) ||
      mulAlrRoleForAddress(pair->reference.sourceClock)!=mulAlrReference ||
      mulAlrRoleForAddress(pair->follower.sourceClock)!=mulAlrFollower ||
      memcmp(pair->reference.receiverClock,pair->follower.receiverClock,8) ||
      r->referenceSequence!=pair->reference.source.sequence ||
      r->referenceOrigin!=pair->reference.source.txTimestamp) return false;
  uint32_t age=nowMs-pair->follower.acceptedMs;
  if (age>r->remainingMs || captureAgeMs>r->remainingMs-age) return false;
  uint64_t converted;
  if (!mulRelationInverse(r,pair->follower.source.txTimestamp,age+captureAgeMs,&converted)) return false;
  int64_t dt=mulTimestampDifference(converted,pair->reference.source.txTimestamp);
  if (dt<MUL_SLOT_MIN_DTU || dt>MUL_SLOT_MAX_DTU) return false;
  *txSeparation=dt; pair->clockRelationAvailable=true;
  return true;
}

static int64_t intervalToReference(int64_t d,int32_t ppb)
{
  int64_t denominator=INT64_C(1000000000)+ppb;
  int64_t product=d*ppb;
  int64_t correction=product>=0 ? (product+denominator/2)/denominator : -((-product+denominator/2)/denominator);
  return d-correction;
}

mulDownlink_t mulDownlinkCompute(const mulRelation_t *r,mulPair_t *pair,
  const mulClock_t *rate,uint32_t nowMs,uint32_t captureAgeMs,bool stationaryOptIn)
{
  mulDownlink_t result={.physicalBoundDtu=UINT64_MAX,.status=mulDownlinkNoRelation};
  if (!mulDownlinkRelation(r,pair,nowMs,captureAgeMs,&result.txSeparationRef)) return result;
  result.relationAgeMs=r->originAgeMs+(nowMs-pair->follower.acceptedMs)+captureAgeMs;
  result.status=mulDownlinkNoRate;
  if (!stationaryOptIn || !rate || !rate->hasBaseline || rate->status!=mulClockValid) return result;
  result.status=mulDownlinkStaleRate;
  if ((uint32_t)(nowMs-rate->observedMs)>MUL_PAIR_MAX_AGE_MS ||
      rate->sequence!=pair->reference.source.sequence || rate->referenceTx!=pair->reference.source.txTimestamp ||
      rate->localRx!=pair->reference.source.rxTimestamp ||
      rate->skewPpb < -MUL_CLOCK_MAX_PPB || rate->skewPpb > MUL_CLOCK_MAX_PPB) return result;
  int64_t raw=mulTimestampDifference(pair->follower.source.rxTimestamp,pair->reference.source.rxTimestamp);
  result.status=mulDownlinkInconsistent;
  if (raw!=pair->rawArrivalDifferenceDtu || raw<MUL_SLOT_MIN_DTU || raw>MUL_SLOT_MAX_DTU) return result;
  result.ratePpb=rate->skewPpb;
  result.rawRxSeparationRef=intervalToReference(raw,rate->skewPpb);
  result.rawTdoaRef=result.rawRxSeparationRef-result.txSeparationRef;
  if (pair->correctedAvailable) {
    if (pair->reference.power.status!=mulPowerApplied || pair->follower.power.status!=mulPowerApplied) return result;
    int64_t corrected=mulTimestampDifference(pair->follower.correctedRx,pair->reference.correctedRx);
    if (corrected!=pair->correctedArrivalDifferenceDtu || corrected<raw-128 || corrected>raw+128) return result;
    result.correctedRxSeparationRef=intervalToReference(corrected,rate->skewPpb);
    result.correctedTdoaRef=result.correctedRxSeparationRef-result.txSeparationRef;
    result.correctedAvailable=true;
  }
  int64_t selected=result.correctedAvailable ? result.correctedTdoaRef : result.rawTdoaRef;
  // Same prototype <=1 us inter-ALR range envelope, not a physical error bound.
  if (selected < -MUL_CLOCK_DELAY_MAX_DTU || selected > MUL_CLOCK_DELAY_MAX_DTU) return result;
  int64_t mm=selected*INT64_C(299792458000);
  result.rangeDifferenceMm=(int32_t)(mm>=0 ? (mm+INT64_C(31948800000))/INT64_C(63897600000) :
    -((-mm+INT64_C(31948800000))/INT64_C(63897600000)));
  result.status=mulDownlinkAvailable; pair->geometricTdoaAvailable=true;
  return result;
}
