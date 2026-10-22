/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_clock.h"

#include <string.h>

#define DTU_PER_MS UINT64_C(63897600)
#define PERIOD_DTU (1000 * DTU_PER_MS)
#define PERIOD_TOLERANCE (100 * DTU_PER_MS)

// UWB-task owned; no wire epoch exists. Never reuse a token within this boot.
static uint64_t lastEpoch;

void mulClockReset(mulClock_t *clock)
{
  memset(clock, 0, sizeof(*clock));
  if (lastEpoch != UINT64_MAX) {
    clock->epoch = ++lastEpoch;
  } // On exhaustion epoch=0 permanently disables corrected mapping, fail closed.
}

static void baseline(mulClock_t *clock, const mulTestPacket_t *packet,
                     uint64_t localRx, uint32_t observedMs)
{
  clock->hasBaseline = true;
  clock->sequence = packet->sequence;
  clock->referenceTx = mulTimestampMask(packet->txTimestamp);
  clock->localRx = mulTimestampMask(localRx);
  clock->observedMs = observedMs;
  if (clock->count == 0) {
    clock->status = mulClockWarmup;
  }
}

mulClockResult_t mulClockObserve(mulClock_t *clock, const mulTestPacket_t *packet,
                                uint64_t localRx, uint32_t observedMs, bool valid)
{
  if (!valid || packet == NULL || mulAlrRoleForAddress(packet->source) != mulAlrReference) {
    return mulClockIgnored;
  }
  if (!clock->hasBaseline) {
    baseline(clock, packet, localRx, observedMs);
    return mulClockBaseline;
  }
  uint32_t advance = packet->sequence - clock->sequence;
  if (advance == 0 || advance >= UINT32_C(0x80000000)) {
    return mulClockIgnored; // No baseline/history/age refresh from replay or reordering.
  }

  // Subtract integers in their own clock domains before computing the rate.
  int64_t remote = mulTimestampDifference(packet->txTimestamp, clock->referenceTx);
  int64_t local = mulTimestampDifference(localRx, clock->localRx);
  uint32_t elapsed = observedMs - clock->observedMs;
  // Bound the elapsed *real* time as well as modular differences, to reject an
  // apparently small interval after one or more complete hardware-counter wraps.
  // The existing 1 Hz reference advances sequence once per TX attempt. Missing
  // packets use their measured multi-period intervals, never an assumed second.
  bool plausible = elapsed > 0 && elapsed < MUL_SOURCE_TIMEOUT_MS &&
    advance <= 3 && remote > 0 && local > 0 &&
    remote <= (int64_t)(3 * PERIOD_DTU + PERIOD_TOLERANCE) &&
    local <= (int64_t)(3 * PERIOD_DTU + PERIOD_TOLERANCE);
  if (plausible) {
    int64_t expected = (int64_t)advance * PERIOD_DTU;
    plausible = remote >= expected - (int64_t)PERIOD_TOLERANCE &&
                remote <= expected + (int64_t)PERIOD_TOLERANCE;
  }
  int32_t skew = 0;
  if (plausible) {
    int64_t difference = local - remote;
    // Gate before multiplication: bounds ensure difference * 1e9 fits int64.
    int64_t limit = remote * MUL_CLOCK_MAX_PPB / INT64_C(1000000000);
    plausible = difference >= -limit && difference <= limit;
    if (plausible) {
      // Round to nearest ppb; avoid subtracting nearly equal floats near ratio=1.
      int64_t scaled = difference * INT64_C(1000000000);
      skew = (int32_t)((scaled + (scaled < 0 ? -remote/2 : remote/2)) / remote);
    }
  }
  if (!plausible) {
    mulClockReset(clock);
    baseline(clock, packet, localRx, observedMs);
    return mulClockDiscontinuity;
  }

  clock->history[clock->next] = skew;
  clock->next = (clock->next + 1) % MUL_CLOCK_HISTORY;
  if (clock->count < MUL_CLOCK_HISTORY) {
    clock->count++;
  }
  if (clock->samples != UINT32_MAX) {
    clock->samples++;
  }
  // Five bounded values: median rejects up to two isolated plausible outliers
  // without an accumulating sum or a persistent bias from an erroneous interval.
  int32_t sorted[MUL_CLOCK_HISTORY];
  memcpy(sorted, clock->history, clock->count * sizeof(sorted[0]));
  for (unsigned int i = 1; i < clock->count; i++) {
    int32_t value = sorted[i];
    unsigned int j = i;
    while (j > 0 && sorted[j - 1] > value) {
      sorted[j] = sorted[j - 1];
      j--;
    }
    sorted[j] = value;
  }
  clock->skewPpb = sorted[clock->count / 2];
  unsigned int inliers = 0;
  for (unsigned int i = 0; i < clock->count; i++) {
    int32_t distance = sorted[i] - clock->skewPpb;
    if (distance >= -MUL_CLOCK_STABILITY_PPB && distance <= MUL_CLOCK_STABILITY_PPB) {
      inliers++;
    }
  }
  clock->status = clock->count == MUL_CLOCK_HISTORY && inliers >= 3 ?
    mulClockValid : mulClockWarmup;
  baseline(clock, packet, localRx, observedMs);
  return mulClockSample;
}

mulClockMapStatus_t mulClockMapStatus(const mulClock_t *clock, uint32_t nowMs)
{
  if (clock == NULL || !clock->hasBaseline || clock->status == mulClockSearching ||
      clock->skewPpb < -MUL_CLOCK_MAX_PPB || clock->skewPpb > MUL_CLOCK_MAX_PPB) {
    return mulClockMapInvalid;
  }
  if ((uint32_t)(nowMs - clock->observedMs) > MUL_CLOCK_MAP_HORIZON_MS) {
    return mulClockMapStale;
  }
  return clock->status == mulClockValid ? mulClockMapValid : mulClockMapWarmup;
}

static int64_t roundedDivide(int64_t numerator, int64_t denominator)
{
  // Inputs are bounded before multiplication/division; denominator is positive.
  return (numerator + (numerator < 0 ? -denominator/2 : denominator/2)) / denominator;
}

static bool mapCoordinate(const mulClock_t *clock, uint64_t input, uint32_t nowMs,
                          bool inverse, mulClockMapping_t *mapping)
{
  if (mapping == NULL || mulClockMapStatus(clock, nowMs) != mulClockMapValid) {
    return false;
  }
  int64_t delta = mulTimestampDifference(input, inverse ? clock->localRx : clock->referenceTx);
  // Bound before multiplying. Inverse local displacement can be slightly larger
  // than reference horizon for a fast follower, so admit this guarded envelope.
  int64_t horizon = MUL_CLOCK_MAP_HORIZON_DTU;
  int64_t envelope = inverse ? horizon + horizon * MUL_CLOCK_MAX_PPB / INT64_C(1000000000) + 1 : horizon;
  if (delta < -envelope || delta > envelope) {
    return false; // Includes exactly-half-period ambiguity and negative long gaps.
  }
  int64_t correction = roundedDivide(delta * clock->skewPpb,
    INT64_C(1000000000) + (inverse ? clock->skewPpb : 0));
  // Inverse: d_ref = d_local - d_local*s/(1+s), avoiding d_local*1e9 overflow.
  int64_t referenceDelta = inverse ? delta - correction : delta;
  if (referenceDelta < -horizon || referenceDelta > horizon) {
    return false;
  }
  uint64_t distance = (uint64_t)(referenceDelta < 0 ? -referenceDelta : referenceDelta);
  uint32_t spread = 0;
  for (unsigned int i = 0; i < clock->count; i++) {
    int64_t deviation = (int64_t)clock->history[i] - clock->skewPpb;
    uint32_t magnitude = (uint32_t)(deviation < 0 ? -deviation : deviation);
    if (magnitude > spread) {
      spread = magnitude;
    }
  }
  mulClockMapping_t result = {
    .timestamp = mulTimestampMask((inverse ? clock->referenceTx : clock->localRx) +
                                 (inverse ? referenceDelta : delta + correction)),
    .destination = inverse ? mulAlrReference : mulAlrFollower,
    .ageMs = nowMs - clock->observedMs,
    .extrapolationDtu = distance,
    .unknownComponents = mulClockUnknownAll,
    .observedRateSpreadPpb = spread,
    // Conservative <=1 ppb representation allowance + one conversion tick.
    // True rate/measurement/propagation errors remain UNKNOWN, not this number.
    .numericalBoundDtu = 1 + (distance + UINT64_C(999999999)) / UINT64_C(1000000000),
    .syncCalibrated = false,
  };
  *mapping = result;
  return true;
}

bool mulClockMapRefToFollower(const mulClock_t *clock, uint64_t referenceTx,
                             uint32_t nowMs, mulClockMapping_t *mapping)
{
  return mapCoordinate(clock, referenceTx, nowMs, false, mapping);
}

bool mulClockMapFollowerToRef(const mulClock_t *clock, uint64_t localArrival,
                             uint32_t nowMs, mulClockMapping_t *mapping)
{
  return mapCoordinate(clock, localArrival, nowMs, true, mapping);
}

mulClockDelayStatus_t mulClockDelayStatus(const mulClock_t *clock,
                                         const mulClockDelay_t *delay, uint32_t nowMs)
{
  if (delay == NULL) {
    return mulClockDelayMissing;
  }
  if (!delay->valid ||
      mulAlrRoleForAddress(delay->reference) != mulAlrReference ||
      mulAlrRoleForAddress(delay->follower) != mulAlrFollower ||
      delay->units != mulClockDelayReferenceDtu || delay->oneWayDtu < 0 ||
      delay->oneWayDtu > MUL_CLOCK_DELAY_MAX_DTU ||
      delay->maxAgeMs == 0 || delay->maxAgeMs > MUL_CLOCK_DELAY_MAX_AGE_MS) {
    return mulClockDelayInvalid;
  }
  if (clock == NULL || clock->epoch == 0 || delay->epoch != clock->epoch ||
      (uint32_t)(clock->sequence - delay->referenceSequence) >
        (delay->maxAgeMs <= 2000 ? 3u : (delay->maxAgeMs+999)/1000+2)) {
    // Bound sequence distance as well as the original measurement age. This additional
    // modular guard also rejects uptime full-wrap aliases and future sequences.
    return mulClockDelayEpochMismatch;
  }
  if ((uint32_t)(nowMs - delay->measuredMs) > delay->maxAgeMs) {
    return mulClockDelayStale; // Also rejects a measurement from the future.
  }
  return mulClockDelayValid;
}

static uint64_t boundAdd(uint64_t a, uint64_t b)
{
  return a >= UINT64_MAX - b ? MUL_CLOCK_BOUND_UNKNOWN : a + b;
}

static uint64_t boundMultiply(uint64_t rate, uint32_t elapsed)
{
  if (rate == MUL_CLOCK_BOUND_UNKNOWN) {
    return MUL_CLOCK_BOUND_UNKNOWN; // Even age=0 does not imply calibrated motion.
  }
  return elapsed != 0 && rate >= UINT64_MAX / elapsed ?
    MUL_CLOCK_BOUND_UNKNOWN : rate * elapsed;
}

// Scale an uncertainty conservatively upwards, conditional on the current slope.
static uint64_t scaleDelayBound(uint64_t bound, int32_t skew, bool inverse)
{
  if (bound == MUL_CLOCK_BOUND_UNKNOWN ||
      bound > (uint64_t)INT64_MAX / (UINT64_C(1000000000) + MUL_CLOCK_MAX_PPB)) {
    return MUL_CLOCK_BOUND_UNKNOWN;
  }
  // Forward delay contribution is a*bound, inverse contribution is bound.
  return inverse ? bound : (bound * (uint64_t)(INT64_C(1000000000) + skew) +
                            UINT64_C(999999999)) / UINT64_C(1000000000);
}

static bool correctedCoordinate(const mulClock_t *clock, uint64_t input,
  const mulClockDelay_t *delay, uint32_t nowMs, bool inverse, mulClockCorrectedMapping_t *mapping)
{
  if (mapping == NULL || mulClockDelayStatus(clock, delay, nowMs) != mulClockDelayValid ||
      mulClockMapStatus(clock, nowMs) != mulClockMapValid) {
    return false;
  }
  int64_t localDelay = delay->oneWayDtu +
    roundedDivide(delay->oneWayDtu * clock->skewPpb, INT64_C(1000000000));
  mulClockMapping_t affine;
  bool valid = inverse ?
    mulClockMapFollowerToRef(clock, mulTimestampMask(input + localDelay), nowMs, &affine) :
    mulClockMapRefToFollower(clock, input, nowMs, &affine);
  if (!valid) {
    return false;
  }
  uint32_t age = nowMs - delay->measuredMs;
  // Cover both mapping origin and requested instant, forwards or backwards.
  // The triangle bound is deliberately conservative, not a motion measurement.
  uint32_t originSeparation = age >= affine.ageMs ? age - affine.ageMs : affine.ageMs - age;
  uint32_t span = originSeparation +
    (uint32_t)((affine.extrapolationDtu + DTU_PER_MS - 1) / DTU_PER_MS);
  if (age > span) {
    span = age;
  }
  if (span > delay->maxAgeMs) {
    return false;
  }
  uint64_t motion = boundMultiply(delay->motionBoundDtuPerMs, span);
  uint64_t delayBound = boundAdd(delay->measurementBoundDtu, motion);
  mulClockCorrectedMapping_t result = {
    .timestamp = inverse ? affine.timestamp : mulTimestampMask(affine.timestamp - localDelay),
    .affine = affine,
    .delayAgeMs = age,
    .applicationAgeMs = span,
    .measurementBoundReferenceDtu = delay->measurementBoundDtu,
    .motionBoundReferenceDtu = motion,
    .conditionalDelayBoundDtu = scaleDelayBound(delayBound, clock->skewPpb, inverse),
    // Additional correction rounding + slope representation over tau. Inverse
    // has one extra rounding tick allowance for the local-delay input shift.
    .numericalBoundDtu = affine.numericalBoundDtu + 2 +
      ((uint64_t)delay->oneWayDtu + UINT64_C(999999999)) / UINT64_C(1000000000),
    // Arrival origin/rate/measurement errors have no established finite bounds.
    .totalPhysicalBoundDtu = MUL_CLOCK_BOUND_UNKNOWN,
  };
  *mapping = result;
  return true;
}

bool mulClockMapRefToFollowerSimultaneous(const mulClock_t *clock, uint64_t referenceTx,
  const mulClockDelay_t *delay, uint32_t nowMs, mulClockCorrectedMapping_t *mapping)
{
  return correctedCoordinate(clock, referenceTx, delay, nowMs, false, mapping);
}

bool mulClockMapFollowerSimultaneousToRef(const mulClock_t *clock, uint64_t localSimultaneous,
  const mulClockDelay_t *delay, uint32_t nowMs, mulClockCorrectedMapping_t *mapping)
{
  return correctedCoordinate(clock, localSimultaneous, delay, nowMs, true, mapping);
}
