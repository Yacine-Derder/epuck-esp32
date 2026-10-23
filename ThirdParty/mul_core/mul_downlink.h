/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_DOWNLINK_H
#define MUL_DOWNLINK_H
#include "mul_relation.h"
#include "mul_observation.h"
// Current-frame relation only. captureAgeMs is hardware RX-to-callback age,
// rounded outward. No cached relation may be substituted for unavailable metadata.
bool mulDownlinkRelation(const mulRelation_t *relation,mulPair_t *pair,
  uint32_t nowMs,uint32_t captureAgeMs,int64_t *txSeparationReferenceDtu);
bool mulDownlinkRelationNetwork(const mulNetwork_t *network,const mulRelation_t *relation,mulPair_t *pair,
  uint32_t nowMs,uint32_t captureAgeMs,int64_t *txSeparationReferenceDtu);

// Explicit opt-in for stationary bench experiments ONLY. Arrival cadence cannot
// distinguish constant radial motion from clock skew. No mobile-mode default.
#ifndef MUL_EXPERIMENTAL_STATIONARY_RLR_RATE
#define MUL_EXPERIMENTAL_STATIONARY_RLR_RATE 0
#endif
typedef enum {
  mulDownlinkNoRelation,
  mulDownlinkNoRate,
  mulDownlinkStaleRate,
  mulDownlinkInconsistent,
  mulDownlinkAvailable,
} mulDownlinkStatus_t;
typedef struct {
  int64_t txSeparationRef;
  int64_t rawRxSeparationRef;
  int64_t correctedRxSeparationRef;
  int64_t rawTdoaRef;
  int64_t correctedTdoaRef;
  uint64_t physicalBoundDtu;
  int32_t rangeDifferenceMm;
  int32_t ratePpb;
  uint32_t relationAgeMs;
  mulDownlinkStatus_t status;
  bool correctedAvailable;
} mulDownlink_t;
// Positive = follower->RLR propagation minus reference->RLR propagation.
// The experimental estimate is conditional on constant propagation/RX bias.
// Without that explicit assumption, no physically justified RLR rate is present.
mulDownlink_t mulDownlinkCompute(const mulRelation_t *relation,mulPair_t *pair,
  const mulClock_t *rlrRate,uint32_t nowMs,uint32_t captureAgeMs,bool stationaryOptIn);
mulDownlink_t mulDownlinkComputeNetwork(const mulNetwork_t *network,const mulRelation_t *relation,mulPair_t *pair,
  const mulClock_t *rlrRate,uint32_t nowMs,uint32_t captureAgeMs,bool stationaryOptIn);
/* Expected offset comes from a validated multi schedule, never another clock. */
bool mulDownlinkRelationAtSlot(const mulNetwork_t *network,uint16_t offsetMs,const mulRelation_t *relation,mulPair_t *pair,
 uint32_t nowMs,uint32_t captureAgeMs,int64_t *txSeparationReferenceDtu);
mulDownlink_t mulDownlinkComputeAtSlot(const mulNetwork_t *network,uint16_t offsetMs,const mulRelation_t *relation,mulPair_t *pair,
 const mulClock_t *rate,uint32_t nowMs,uint32_t captureAgeMs,bool stationaryOptIn);
#endif
