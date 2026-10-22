/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_HYBRID_H
#define MUL_HYBRID_H
#include "mul_localization.h"
#include "mul_legacy_twr.h"
/* Protocol-specific receiver, not a new network scheduler. The reference wire
 * roster is defined by mulAlrRoleForAddress; higher layers/solvers use full IDs.
 * One receiver context owns its inferred generations and never transmits. */
typedef struct {
  mulObservation_t observations[MUL_ALR_COUNT];
  mulAssociation_t association;mulClock_t rate;mulRelation_t relation;
  mulTwrPacket_t twr;
  uint32_t twrStartedMs,twrGeneration,lastTwrId;
  uint32_t received,rejected,pairs,twrCompleted,twrRejected,twrTimeouts;
  bool stationary,twrActive,twrSeen;
} mulHybrid_t;
typedef struct {
  mulTestPacket_t packet;mulRelation_t relation;mulLocalization_t observation;
  uint32_t referenceGeneration,followerGeneration,twrRaw,twrCorrected;
  mulSequenceResult_t order;
  bool broadcast,associated,twrComplete;
} mulHybridOutput_t;
void mulHybridInit(mulHybrid_t *s,const uint8_t receiver[8],bool stationary);
void mulHybridExpire(mulHybrid_t *s,uint32_t nowMs);
/* All arguments captured before RX rearm. captureAgeMs is outward-rounded
 * hardware RX->service time, not a Wi-Fi or host timestamp. */
bool mulHybridReceive(mulHybrid_t *s,const uint8_t *frame,size_t n,
 uint64_t rx,uint32_t captureAgeMs,uint32_t nowMs,mulHybridOutput_t *out);
#endif
