/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_LOCALIZATION_H
#define MUL_LOCALIZATION_H
#include "mul_downlink.h"
// Independent UNKNOWN components; a configured motion term never clears total
// physical uncertainty. None of these are calibrated statistical deviations.
enum {
  mulQualityTwrUnknown=1, mulQualityClockUnknown=2, mulQualityRateUnknown=4,
  mulQualityPowerUnknown=8, mulQualityMotionUnknown=16, mulQualityOtherUnknown=32,
  mulQualityAllUnknown=63
};
typedef enum { mulRateUnavailable, mulRateStationary, mulRateStale } mulRateState_t;
// Caller-owned, generated once per accepted association. No packet copies,
// pointers into transient RX buffers, persistent queue, or serial parsing needed.
// observedMs and receiverTimestamp are FOLLOWER RX in this RLR's uptime/radio
// domains respectively (coarse callback time vs exact 40-bit raw RX marker).
// INT32_MIN/UINT32_MAX/UINT16_MAX mean NA, never a valid unavailable measurement.
typedef struct {
  uint8_t reference[8],follower[8];
  uint64_t receiverTimestamp;
  uint64_t followerEpoch;
  uint32_t referenceSequence,followerSequence;
  uint32_t referenceGeneration,followerGeneration;
  uint32_t observedMs;
  int32_t tdoaDtu,rangeDifferenceMm,ratePpb;
  uint32_t delayAgeMs,motionBoundMm,motionBoundDtu;
  uint16_t delayDtu,slotMs,clockAgeMs;
  uint8_t delayState,status,rateState,powerFlags,unknownComponents;
  uint8_t delaySource:2;
  uint8_t associationValid:1;
  uint8_t clockValid:1;
} mulLocalization_t;
// Nominal DW1000 reference units; oscillator and all other physical errors are
// separate UNKNOWN terms. Bounds round outwards; missing v_max stays UNKNOWN.
void mulPropagationMotionBound(uint32_t ageMs,uint32_t *millimetres,uint32_t *dtu);
void mulLocalizationObserve(mulLocalization_t *observation,mulPair_t *pair,
  const mulRelation_t *relation,const mulClock_t *rate,uint32_t nowMs,
  uint32_t captureAgeMs,bool stationaryOptIn);
void mulLocalizationObserveNetwork(const mulNetwork_t *network,mulLocalization_t *observation,mulPair_t *pair,
  const mulRelation_t *relation,const mulClock_t *rate,uint32_t nowMs,
  uint32_t captureAgeMs,bool stationaryOptIn);

void mulLocalizationObserveAtSlot(const mulNetwork_t *network,uint16_t offsetMs,mulLocalization_t *observation,mulPair_t *pair,
 const mulRelation_t *relation,const mulClock_t *rate,uint32_t nowMs,uint32_t captureAgeMs,bool stationaryOptIn);
#endif
