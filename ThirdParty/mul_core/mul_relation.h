/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_RELATION_H
#define MUL_RELATION_H
#include "mul_clock.h"
#define MUL_RELATION_FRAME_LENGTH 74
#define MUL_PACKET_CLOCK_BROADCAST 2
// Ordered full reference/follower identities come from the explicit roster.
// F(t)=followerOrigin+(1+skewPpb/1e9)*diff40(t,referenceOrigin).
// Epoch is follower acquisition-local, NOT a globally unique persistent boot ID.
typedef struct {
  uint64_t referenceOrigin;
  uint64_t followerOrigin;
  uint64_t epoch;
  int32_t skewPpb;
  uint32_t referenceSequence;
  uint32_t twrSequence;
  uint16_t originAgeMs; // Ages at the hardware follower TX, not callback time.
  uint16_t delayAgeMs;
  uint16_t remainingMs; // Conservative remaining existing contract lifetime.
  uint16_t delayDtu; // v2: actual reference-unit propagation estimate; v1 unknown.
  uint16_t maxAgeMs; // v2 policy on wire; v1 implicitly 1000.
  uint8_t available:1;
  uint8_t powerCorrectedTwr:1;
  uint8_t version:2; // 0 in legacy API fixtures means v1.
  uint8_t delayState:3;

} mulRelation_t;

mulRelation_t mulRelationPublish(const mulClock_t *clock,const mulClockDelay_t *delay,
  uint64_t followerTx,uint64_t hardwareNow,uint32_t nowMs,bool powerCorrectedTwr);
mulRelation_t mulRelationPublishNetwork(const mulNetwork_t *network,const mulClock_t *clock,const mulClockDelay_t *delay,
  uint64_t followerTx,uint64_t hardwareNow,uint32_t nowMs,bool powerCorrectedTwr);
void mulRelationEncode(uint8_t frame[MUL_RELATION_FRAME_LENGTH],
  const mulTestPacket_t *packet,const mulRelation_t *relation);
// Strict type-1/34 OR type-2/74/version-1/2; outputs unchanged on failure.
// A valid old packet represents unavailable metadata, never a stale cache hit.
bool mulBroadcastDecode(const uint8_t *frame,size_t length,
  mulTestPacket_t *packet,mulRelation_t *relation);
bool mulBroadcastDecodeNetwork(const mulNetwork_t *network,const uint8_t *frame,size_t length,
  mulTestPacket_t *packet,mulRelation_t *relation);
bool mulRelationInverse(const mulRelation_t *relation,uint64_t followerTx,
  uint32_t receiverAgeMs,uint64_t *referenceTx);
#endif
