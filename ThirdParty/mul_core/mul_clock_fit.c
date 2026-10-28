/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_clock_fit.h"
#include <math.h>
#include <string.h>
#if defined(__GNUC__)
#define SHARED_HELPER __attribute__((noinline))
#else
#define SHARED_HELPER
#endif
#define R 1024.0
#define K 63.8976 /* DTU/s per ppb */
#define Q 1.0     /* drift random-walk spectral density, ppb^2/s^3 */
static uint32_t sat(uint32_t n) { return n == UINT32_MAX ? n : n + 1; }
void mulClockFitDefaults(mulClockFitConfig_t *c) {
  *c = (mulClockFitConfig_t){
      .periodMs = 1000, .gateDtu = 256, .window = 5, .minSamples = 5};
}
bool mulClockFitConfigValid(const mulClockFitConfig_t *c) {
  return c &&
         (c->estimator == mulClockEstimatorMedian ||
          c->estimator == mulClockEstimatorKalman) &&
         c->periodMs >= 10 && c->periodMs <= 2000 && c->window == 5 &&
         c->minSamples == 5 && c->gateDtu >= 16 && c->compensate <= 1;
}
void mulClockFitReset(mulClockFit_t *s) {
  memset(s, 0, sizeof(*s));
  s->uncertaintyPpb = UINT32_MAX;
}
/* Three-state LDL covariance check; pivots retain their state units, so
 * roundoff tolerance scales with the corresponding diagonal. Nonpositive
 * leading pivots fail closed (initialized/Q/R covariance is positive definite).
 */
static bool healthy(const double x[3], double p[3][3]) {
  for (unsigned i = 0; i < 3; i++) {
    if (!isfinite(x[i]) || !isfinite(p[i][i]) || p[i][i] <= 0)
      return false;
    for (unsigned j = 0; j < 3; j++)
      if (!isfinite(p[i][j]))
        return false;
    for (unsigned j = 0; j < i; j++)
      if (fabs(p[i][j] - p[j][i]) > 1e-12 * (fabs(p[i][j]) + fabs(p[j][i]) + 1))
        return false;
  }
  double a = p[0][1] / p[0][0], b = p[0][2] / p[0][0];
  double d = p[1][1] - a * p[0][1];
  if (!isfinite(d) || d <= 0)
    return false;
  double cross = p[1][2] - b * p[0][1];
  double last = p[2][2] - b * p[0][2] - cross * cross / d;
  return isfinite(last) && last >= -1e-12 * p[2][2];
}
/* Inputs here are nonnegative and bounded before conversion; exact ceiling
 * without pulling general-purpose libm rounding routines into Cortex-M0. */
static SHARED_HELPER uint32_t ceiling(double v) {
  uint32_t n = (uint32_t)v;
  return n + (v > n);
}
static bool predict(const mulClockFit_t *s, double d, double x[3],
                    double p[3][3], double horizon) {
  if (!s || !s->count || !isfinite(d) || d < 0 || d > horizon)
    return false;
  const double f[3][3] = {{1, K * d, K * d * d / 2}, {0, 1, d}, {0, 0, 1}};
  const double q[3][3] = {{K * K * d * d * d * d * d / 20,
                           K * d * d * d * d / 8, K * d * d * d / 6},
                          {K * d * d * d * d / 8, d * d * d / 3, d * d / 2},
                          {K * d * d * d / 6, d * d / 2, d}};
  for (unsigned i = 0; i < 3; i++) {
    x[i] = 0;
    for (unsigned j = 0; j < 3; j++)
      x[i] += f[i][j] * s->state[j];
    for (unsigned j = 0; j < 3; j++) {
      p[i][j] = Q * q[i][j];
      for (unsigned a = 0; a < 3; a++)
        for (unsigned b = 0; b < 3; b++)
          p[i][j] += f[i][a] * s->covariance[a][b] * f[j][b];
    }
  }
  return healthy(x, p);
}
bool mulClockFitPredict(const mulClockFit_t *s, double d, double x[3],
                        double p[3][3]) {
  return predict(s, d, x, p, 2);
}
static SHARED_HELPER void seed(mulClockFit_t *s, uint64_t epoch, uint64_t tx,
                               uint64_t rx, bool corrected) {
  uint32_t a = s->accepted, r = s->rejected, n = s->resets;
  mulClockFitReset(s);
  s->accepted = a;
  s->rejected = r;
  s->resets = sat(n);
  s->epoch = epoch;
  s->lastTx = tx;
  s->lastRx = rx;
  s->count = 1;
  s->compensated = corrected;
  s->covariance[0][0] = R;
  s->covariance[1][1] = 1e10;
  s->covariance[2][2] = 1e4;
}
static bool update(mulClockFit_t *s, double d, int64_t phase) {
  double x[3], p[3][3];
  if (!predict(s, d, x, p, 3))
    return false;
  double innovation = (double)phase - x[0], sigma = sqrt(p[0][0] + R);
  double weight =
      fabs(innovation) > 4 * sigma ? 4 * sigma / fabs(innovation) : 1;
  if (weight < 1)
    s->rejected = sat(s->rejected);
  double effective = R / (weight * weight), gain[3], a[3][3], out[3][3];
  for (unsigned i = 0; i < 3; i++) {
    gain[i] = p[i][0] / (p[0][0] + effective);
    x[i] += gain[i] * innovation;
    for (unsigned j = 0; j < 3; j++)
      a[i][j] = (i == j) - gain[i] * (j == 0);
  }
  for (unsigned i = 0; i < 3; i++)
    for (unsigned j = 0; j < 3; j++) {
      out[i][j] = effective * gain[i] * gain[j];
      for (unsigned b = 0; b < 3; b++)
        for (unsigned c = 0; c < 3; c++)
          out[i][j] += a[i][b] * p[b][c] * a[j][c];
    }
  for (unsigned i = 0; i < 3; i++)
    for (unsigned j = 0; j < i; j++)
      out[i][j] = out[j][i] = (out[i][j] + out[j][i]) / 2;
  if (!healthy(x, out))
    return false;
  memcpy(s->state, x, sizeof(x));
  memcpy(s->covariance, out, sizeof(out));
  s->rmsDtu =
      fabs(innovation) >= UINT32_MAX ? UINT32_MAX : ceiling(fabs(innovation));
  return true;
}
mulClockResult_t mulClockObserveFitted(const mulNetwork_t *network,
                                       mulClock_t *clock, mulClockFit_t *s,
                                       const mulClockFitConfig_t *c,
                                       const mulTestPacket_t *p, uint64_t rx,
                                       uint32_t now, bool valid,
                                       const mulClockDelay_t *delay) {
  if (!clock || !s || !mulClockFitConfigValid(c))
    return mulClockIgnored;
  int32_t previous = clock->skewPpb;
  mulClockResult_t result = mulClockObservePeriodNetwork(
      network, clock, p, rx, now, valid, c->periodMs);
  if (result == mulClockIgnored)
    return result;
  if (!clock->epoch) {
    clock->status = mulClockWarmup;
    s->ready = s->applied = 0;
    s->fallback = mulClockFitNumerical;
    return result;
  }
  if (c->estimator == mulClockEstimatorMedian) {
    s->epoch = clock->epoch;
    s->count = clock->count;
    s->inliers = clock->count;
    s->ready = clock->status == mulClockValid;
    s->applied = 0;
    s->fallback = s->ready ? mulClockFitUsed : mulClockFitWarmup;
    s->skewPpb = clock->skewPpb;
    s->uncertaintyPpb = UINT32_MAX;
    return result; // Median scatter is not covariance.
  }
  bool corrected =
      c->compensate && mulClockDelayStatusNetwork(network, clock, delay, now) ==
                           mulClockDelayValid;
  uint64_t fittedRx = rx;
  if (corrected) {
    int64_t n = delay->oneWayDtu * previous;
    fittedRx =
        mulTimestampMask(rx - delay->oneWayDtu -
                         (n + (n < 0 ? -500000000 : 500000000)) / 1000000000);
  }
  s->applied = 0;
  s->fallback = mulClockFitWarmup;
  if (result == mulClockDiscontinuity || s->epoch != clock->epoch ||
      !s->count || s->compensated != corrected) {
    seed(s, clock->epoch, p->txTimestamp, fittedRx, corrected);
    return result;
  }
  int64_t dt = mulTimestampDifference(p->txTimestamp, s->lastTx),
          dr = mulTimestampDifference(fittedRx, s->lastRx);
  if (dt <= 0 || dr <= 0 || dt >= INT64_C(191692800000) ||
      s->phase > INT64_MAX - INT64_C(1000000000) ||
      s->phase < INT64_MIN + INT64_C(1000000000)) {
    seed(s, clock->epoch, p->txTimestamp, fittedRx, corrected);
    return result;
  }
  int64_t phase = s->phase + dr - dt;
  if (!update(s, (double)dt / 63897600000., phase)) {
    seed(s, clock->epoch, p->txTimestamp, fittedRx, corrected);
    s->fallback = mulClockFitNumerical;
    return result;
  }
  s->phase = phase;
  s->lastTx = p->txTimestamp;
  s->lastRx = fittedRx;
  s->accepted = sat(s->accepted);
  if (s->count < 5)
    s->count++;
  s->inliers = s->count;
  /* Same base-slot average-rate intervention as the reference prototype. */
  double half = mulNetworkSlotMs(network) / 2000.;
  double rate = s->state[1] + half * s->state[2];
  double variance = s->covariance[1][1] + 2 * half * s->covariance[1][2] +
                    half * half * s->covariance[2][2];
  double sigma = sqrt(variance);
  s->uncertaintyPpb =
      !isfinite(sigma) || sigma >= UINT32_MAX ? UINT32_MAX : ceiling(sigma);
  /* 1ppm conditional sigma ceiling reuses the existing median stability scale;
   * not a physical accuracy bound. Five intervals are required on both methods.
   */
  bool mature = clock->count == 5 && s->count == 5;
  s->ready = mature && isfinite(rate) && fabs(rate) <= MUL_CLOCK_MAX_PPB &&
             isfinite(sigma) && variance >= 0 &&
             sigma <= MUL_CLOCK_STABILITY_PPB;
  s->skewPpb = isfinite(rate) && fabs(rate) <= MUL_CLOCK_MAX_PPB
                   ? (int32_t)(rate + (rate < 0 ? -.5 : .5))
                   : 0;
  /* A robust frequency fit cannot validate a grossly displaced RAW mapping
   * origin. Reuse the median's one-ppm interval stability gate after startup;
   * keep filtering, but expose this observation as unavailable in both methods.
   */
  if (mature &&
      s->rmsDtu > K * ((double)dt / 63897600000.) * MUL_CLOCK_STABILITY_PPB) {
    clock->status = mulClockWarmup;
    s->ready = 0;
    s->fallback = mulClockFitUncertain;
    return result;
  }
  if (s->ready) {
    clock->skewPpb = s->skewPpb;
    clock->status = mulClockValid;
    s->applied = 1;
    s->fallback = mulClockFitUsed;
  } else
    s->fallback = mature ? mulClockFitUncertain : mulClockFitWarmup;
  return result;
}
bool mulClockFitConfigEncode(uint8_t out[MUL_CLOCK_CONFIG_BYTES],
                             const mulClockFitConfig_t *c) {
  if (!out || !mulClockFitConfigValid(c))
    return false;
  uint8_t b[12] = {2,
                   c->estimator,
                   5,
                   5,
                   c->periodMs & 255,
                   c->periodMs >> 8,
                   c->gateDtu & 255,
                   c->gateDtu >> 8,
                   c->compensate,
                   0,
                   0,
                   0};
  memcpy(out, b, 12);
  return true;
}
bool mulClockFitConfigDecode(const uint8_t in[MUL_CLOCK_CONFIG_BYTES],
                             mulClockFitConfig_t *c) {
  if (!in || !c || (in[0] != 1 && in[0] != 2) || in[9] || in[10] || in[11])
    return false;
  if (in[0] == 2 && (in[2] != 5 || in[3] != 5))
    return false;
  mulClockFitConfig_t v = {.estimator = in[0] == 2 && in[1] == 2 ? 2 : 0,
                           .window = 5,
                           .minSamples = 5,
                           .periodMs = (uint16_t)(in[4] | in[5] << 8),
                           .gateDtu = (uint16_t)(in[6] | in[7] << 8),
                           .compensate = in[8]};
  if (!mulClockFitConfigValid(&v))
    return false;
  *c = v;
  return true;
}
