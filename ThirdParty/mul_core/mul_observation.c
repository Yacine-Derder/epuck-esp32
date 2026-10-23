/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_observation.h"
#include <string.h>

void mulObservationInit(mulObservation_t observations[2], const uint8_t receiver[8])
{
  memset(observations,0,2*sizeof(*observations));
  for (unsigned int i=0;i<2;i++) memcpy(observations[i].receiverClock,receiver,8);
}

bool mulObservationFresh(const mulObservation_t *observation, uint32_t nowMs)
{
  return observation->valid && (uint32_t)(nowMs-observation->acceptedMs) < MUL_SOURCE_TIMEOUT_MS;
}

void mulObservationExpire(mulObservation_t observations[2], uint32_t nowMs)
{
  for (unsigned int i=0;i<2;i++) if (!mulObservationFresh(&observations[i],nowMs)) observations[i].valid=false;
}

mulSequenceResult_t mulObservationRecordNetwork(const mulNetwork_t *network,mulObservation_t observations[2],
  const mulTestPacket_t *packet, const mulPowerRx_t *rx, uint32_t nowMs)
{
  mulAlrRole_t role=mulNetworkRole(network,packet->source);
  if (role == mulAlrUnsupported) return mulSequenceOld;
  mulObservation_t *o=&observations[role];
  bool rebase=o->source.hasSequence && packet->sequence != o->source.sequence &&
              (uint32_t)(nowMs-o->acceptedMs) >= MUL_SOURCE_TIMEOUT_MS;
  if ((!o->source.hasSequence || rebase) && o->generation == UINT32_MAX) {
    o->valid=false; return mulSequenceOld; // Generation exhaustion fails closed.
  }
  if (rebase) o->source.hasSequence=false;
  bool first=!o->source.hasSequence;
  mulSequenceResult_t result=mulObservePacket(&o->source,packet,rx->raw);
  if (result == mulSequenceOld || result == mulSequenceDuplicate) return result;
  if (first) o->generation++;
  o->acceptedMs=nowMs;
  o->correctedRx=mulTimestampMask(rx->corrected);
  o->power=rx->power;
  memcpy(o->sourceClock,packet->source,8);
  o->valid=true;
  o->remoteEpochKnown=false;
  return result;
}

void mulAssociationExpireNetwork(const mulNetwork_t *network,mulAssociation_t *a, mulObservation_t observations[2], uint32_t nowMs)
{
  mulObservationExpire(observations,nowMs);
  if (!mulObservationFresh(&observations[0],nowMs) ||
      (uint32_t)(nowMs-observations[0].acceptedMs) > mulNetworkPairMaxAgeMs(network)) a->pending=false;
}

static bool cadence(const mulObservation_t *old, const mulObservation_t *current,
                    mulSequenceResult_t result)
{
  const int64_t ms=INT64_C(63897600);
  int64_t tx=mulTimestampDifference(current->source.txTimestamp,old->source.txTimestamp);
  int64_t rx=mulTimestampDifference(current->source.rxTimestamp,old->source.rxTimestamp);
  uint32_t elapsed=current->acceptedMs-old->acceptedMs;
  return result == mulSequenceForward && old->generation == current->generation &&
         elapsed >= 900 && elapsed <= 1100 && tx >= 900*ms && tx <= 1100*ms &&
         rx >= 900*ms && rx <= 1100*ms;
}

bool mulAssociateNetwork(const mulNetwork_t *network,mulAssociation_t *a, mulObservation_t observations[2],
  const mulTestPacket_t *packet, const mulPowerRx_t *rx, uint32_t nowMs,
  mulSequenceResult_t *sequenceResult, mulPair_t *pair)
{
  mulAlrRole_t role=mulNetworkRole(network,packet->source);
  *sequenceResult=mulSequenceOld;
  mulAssociationExpireNetwork(network,a,observations,nowMs);
  if (role == mulAlrUnsupported) return false;
  mulObservation_t previous=observations[role];
  *sequenceResult=mulObservationRecordNetwork(network,observations,packet,rx,nowMs);
  if (*sequenceResult == mulSequenceDuplicate || *sequenceResult == mulSequenceOld) return false;
  bool consecutive=cadence(&previous,&observations[role],*sequenceResult);
  mulObservation_t *ref=&observations[0], *fol=&observations[1];
  if (role == mulAlrReference) {
    a->pending=consecutive;
    a->referenceSequence=ref->source.sequence;
    a->referenceGeneration=ref->generation;
    a->followerGeneration=fol->generation;
    return false;
  }
  bool pending=a->pending;
  a->pending=false; // Any accepted follower consumes this one candidate.
  if (!pending || !consecutive || !mulObservationFresh(ref,nowMs) ||
      nowMs-ref->acceptedMs > mulNetworkPairMaxAgeMs(network) ||
      a->referenceSequence != ref->source.sequence || a->referenceGeneration != ref->generation ||
      a->followerGeneration != fol->generation ||
      mulNetworkRole(network,ref->sourceClock) != mulAlrReference ||
      mulNetworkRole(network,fol->sourceClock) != mulAlrFollower ||
      memcmp(ref->receiverClock,fol->receiverClock,8)) return false;
  int64_t difference=mulTimestampDifference(fol->source.rxTimestamp,ref->source.rxTimestamp);
  if (difference < mulSlotMinDtu(mulNetworkSlotMs(network)) || difference > mulSlotMaxDtu(mulNetworkSlotMs(network))) return false;
  *pair=(mulPair_t){.reference=*ref,.follower=*fol,.rawArrivalDifferenceDtu=difference,
    .physicalBoundDtu=UINT64_MAX};
  if (ref->power.status == mulPowerApplied && fol->power.status == mulPowerApplied) {
    pair->correctedArrivalDifferenceDtu=mulTimestampDifference(fol->correctedRx,ref->correctedRx);
    pair->correctedAvailable=true;
  }
  // Association alone never subtracts source TXs. The downlink layer must bind
  // the transported simultaneous model and rate without fitting away geometry.
  return true;
}

/* Legacy API wrappers retain the default network. */
mulSequenceResult_t mulObservationRecord(mulObservation_t observations[2],
  const mulTestPacket_t *packet, const mulPowerRx_t *rx, uint32_t nowMs) { return mulObservationRecordNetwork(NULL,observations,packet,rx,nowMs); }
bool mulAssociate(mulAssociation_t *association, mulObservation_t observations[2],
  const mulTestPacket_t *packet, const mulPowerRx_t *rx, uint32_t nowMs,
  mulSequenceResult_t *sequenceResult, mulPair_t *pair) { return mulAssociateNetwork(NULL,association,observations,packet,rx,nowMs,sequenceResult,pair); }

void mulAssociationExpire(mulAssociation_t *a,mulObservation_t observations[2],uint32_t now) {mulAssociationExpireNetwork(NULL,a,observations,now);}
