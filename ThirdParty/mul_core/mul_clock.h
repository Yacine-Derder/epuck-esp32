/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_CLOCK_H
#define MUL_CLOCK_H

#include "mul_common.h"
#include "mul_propagation.h"

#define MUL_CLOCK_HISTORY 5
#define MUL_CLOCK_MAX_PPB 200000 // +/-200 ppm plausibility gate, not calibration.
#define MUL_CLOCK_STABILITY_PPB 1000 // At least three samples within 1 ppm of median.

typedef enum {
  mulClockSearching,
  mulClockWarmup,
  mulClockValid,
} mulClockStatus_t;

typedef enum {
  mulClockIgnored,
  mulClockBaseline,
  mulClockSample,
  mulClockDiscontinuity,
} mulClockResult_t;

typedef struct {
  bool hasBaseline;
  uint32_t sequence;
  uint64_t referenceTx;
  uint64_t localRx;
  uint32_t observedMs;
  int32_t history[MUL_CLOCK_HISTORY];
  unsigned int count;
  unsigned int next;
  uint32_t samples; // Accepted intervals since baseline/reset, saturating.
  int32_t skewPpb; // ratio = 1 + skewPpb/1e9; positive means faster local clock.
  mulClockStatus_t status;
  uint64_t epoch; // Process-local binding, replaced by every reset/discontinuity.
} mulClock_t;

void mulClockReset(mulClock_t *clock);
// observedMs is wrap-safe coarse monotonic uptime, used only to reject long gaps.
// valid means the packet passed wire validation and local RX capture succeeded.
// Caller must reset on reference loss/reacquisition; no epoch exists on wire.
mulClockResult_t mulClockObserve(mulClock_t *clock, const mulTestPacket_t *packet,
                                uint64_t localRx, uint32_t observedMs, bool valid);
mulClockResult_t mulClockObserveNetwork(const mulNetwork_t *network,mulClock_t *clock, const mulTestPacket_t *packet,
                                uint64_t localRx, uint32_t observedMs, bool valid);

// Arrival-aligned mapping ONLY: full reference identity 84:00:00:00:00:00:cf:bc
// to follower 85:00:00:00:00:00:cf:bc. Existing observation/slope fields are the
// mapping origin; no absolute intercept or independently duplicated state.
#define MUL_CLOCK_MAP_HORIZON_MS 2000
#define MUL_CLOCK_MAP_HORIZON_DTU (UINT64_C(63897600) * MUL_CLOCK_MAP_HORIZON_MS)
typedef enum {
  mulClockMapInvalid,
  mulClockMapWarmup,
  mulClockMapValid,
  mulClockMapStale,
} mulClockMapStatus_t;

// These physical uncertainties have no established finite bounds. They must
// not be inferred from convergence, timestamp resolution, or sample scatter.
enum {
  mulClockUnknownRate = 1u << 0,
  mulClockUnknownReferenceMeasurement = 1u << 1,
  mulClockUnknownFollowerMeasurement = 1u << 2,
  mulClockUnknownOrigin = 1u << 3,
  mulClockUnknownPropagation = 1u << 4,
  mulClockUnknownPropagationChange = 1u << 5,
  mulClockUnknownExtrapolation = 1u << 6,
  mulClockUnknownTotal = 1u << 7,
  mulClockUnknownAll = 0xff,
};

typedef struct {
  uint64_t timestamp; // 40-bit arrival-aligned coordinate, in destination clock.
  mulAlrRole_t destination;
  uint32_t ageMs; // Coarse age of origin at query time, not extrapolation distance.
  uint64_t extrapolationDtu; // Absolute displacement from origin, reference units.
  uint32_t unknownComponents; // Physical error bounds: flags above, currently ALL.
  uint32_t observedRateSpreadPpb; // Max deviation in five samples; NOT a bound.
  uint64_t numericalBoundDtu; // Rounding/rate-quantization ONLY, destination units.
  bool syncCalibrated; // Always false: neither true offset nor delay is identified.
} mulClockMapping_t;

mulClockMapStatus_t mulClockMapStatus(const mulClock_t *clock, uint32_t nowMs);
// Nearest modular displacement must be within +/- horizon in reference units;
// origin age must also be <= horizon. Caller must know the intended counter epoch:
// no 40-bit API can distinguish a full-wrap alias from a nearby coordinate.
// On failure output is unchanged. Inverse input must be an arrival-aligned local
// coordinate, NOT an arbitrary follower TX timestamp. Both inherit delay ambiguity.
bool mulClockMapRefToFollower(const mulClock_t *clock, uint64_t referenceTx,
                             uint32_t nowMs, mulClockMapping_t *mapping);
bool mulClockMapFollowerToRef(const mulClock_t *clock, uint64_t localArrival,
                             uint32_t nowMs, mulClockMapping_t *mapping);

// External one-way reference->follower measurement, never a raw TWR round trip.
// Records are volatile: discard on firmware restart; do not persist/replay epochs.
#define MUL_CLOCK_BOUND_UNKNOWN UINT64_MAX
#define MUL_CLOCK_DELAY_DEFAULT_MAX_AGE_MS 1000
#define MUL_CLOCK_DELAY_MAX_AGE_MS 6000
// Initial application policy: <=1 us (~300 m), rounded up to a reference DTU.
#define MUL_CLOCK_DELAY_MAX_DTU INT64_C(63898)
typedef enum {
  mulClockDelayReferenceDtu,
} mulClockDelayUnits_t;

typedef struct {
  uint8_t reference[MUL_ADDRESS_LENGTH];
  uint8_t follower[MUL_ADDRESS_LENGTH]; // Ordered identities define direction.
  uint64_t epoch; // Copy current clock.epoch when measurement is acquired.
  int64_t oneWayDtu; // Nonnegative delay in REFERENCE hardware-clock units.
  mulClockDelayUnits_t units;
  uint32_t measuredMs; // Same follower monotonic uptime domain as observedMs.
  uint32_t referenceSequence; // Latest accepted reference sequence at acquisition.
  uint32_t maxAgeMs; // Caller policy: 1..MAX_AGE, default recommendation above.
  bool valid;
  uint64_t measurementBoundDtu; // Reference units; UNKNOWN is not zero.
  uint64_t motionBoundDtuPerMs; // Bound on path-delay change per coarse ms, or UNKNOWN.
} mulClockDelay_t;

typedef enum {
  mulClockDelayMissing,
  mulClockDelayInvalid,
  mulClockDelayEpochMismatch,
  mulClockDelayStale,
  mulClockDelayValid,
} mulClockDelayStatus_t;

typedef struct {
  uint64_t timestamp; // Corrected destination coordinate (NOT arrival-aligned).
  mulClockMapping_t affine; // Underlying arrival API result; its uncertainty stays UNKNOWN.
  uint32_t delayAgeMs;
  uint32_t applicationAgeMs; // Conservative span to origin/query; governs motion/age policy.
  uint64_t measurementBoundReferenceDtu;
  uint64_t motionBoundReferenceDtu;
  uint64_t conditionalDelayBoundDtu; // Destination units, conditional on estimated slope.
  uint64_t numericalBoundDtu; // Includes affine and delay-correction rounding only.
  uint64_t totalPhysicalBoundDtu; // UNKNOWN while any required physical component is unknown.
} mulClockCorrectedMapping_t;

// Delay validity is separate from affine validity and correction availability.
mulClockDelayStatus_t mulClockDelayStatus(const mulClock_t *clock,
                                         const mulClockDelay_t *delay, uint32_t nowMs);
mulClockDelayStatus_t mulClockDelayStatusNetwork(const mulNetwork_t *network,const mulClock_t *clock,
                                         const mulClockDelay_t *delay, uint32_t nowMs);
// Separate, stateless APIs apply correction exactly once; existing arrival APIs
// never inspect a delay. Success means AVAILABLE, not fully bounded/calibrated.
// Outputs stay unchanged on failure. Caller must supply the correct coordinate
// kind/epoch and serialize clock operations in the UWB task, as for the estimator.
bool mulClockMapRefToFollowerSimultaneous(const mulClock_t *clock, uint64_t referenceTx,
  const mulClockDelay_t *delay, uint32_t nowMs, mulClockCorrectedMapping_t *mapping);
bool mulClockMapRefToFollowerSimultaneousNetwork(const mulNetwork_t *network,const mulClock_t *clock, uint64_t referenceTx,
  const mulClockDelay_t *delay, uint32_t nowMs, mulClockCorrectedMapping_t *mapping);
bool mulClockMapFollowerSimultaneousToRef(const mulClock_t *clock, uint64_t localSimultaneous,
  const mulClockDelay_t *delay, uint32_t nowMs, mulClockCorrectedMapping_t *mapping);
bool mulClockMapFollowerSimultaneousToRefNetwork(const mulNetwork_t *network,const mulClock_t *clock, uint64_t localSimultaneous,
  const mulClockDelay_t *delay, uint32_t nowMs, mulClockCorrectedMapping_t *mapping);

#endif // MUL_CLOCK_H
