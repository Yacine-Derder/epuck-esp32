/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_CLOCK_FIT_H
#define MUL_CLOCK_FIT_H
#include "mul_clock.h"
#define MUL_CLOCK_FIT_MAX 5u
#define MUL_CLOCK_FIT_DEFAULT 5u
#define MUL_CLOCK_CONFIG_BYTES 12u
/* Value1 is retired regression. Never reinterpret an old EEPROM selector. */
enum { mulClockEstimatorMedian = 0, mulClockEstimatorKalman = 2 };
enum {
  mulClockFitUsed = 0,
  mulClockFitWarmup = 1,
  mulClockFitUncertain = 2,
  mulClockFitNumerical = 3
};
typedef struct {
  uint16_t periodMs, gateDtu;
  uint8_t estimator, window, minSamples, compensate;
} mulClockFitConfig_t;
/* Shared phase(DTU), frequency(ppb), drift(ppb/s), covariance in those units.
 * Integers unwrap local-minus-reference intervals before conversion to double.
 * No absolute 40-bit timestamp is converted to floating point. */
typedef struct {
  double state[3], covariance[3][3];
  uint64_t lastTx, lastRx, epoch;
  int64_t phase;
  int32_t skewPpb;
  uint32_t rmsDtu, uncertaintyPpb, rejected, accepted, resets;
  uint8_t count, inliers, compensated, ready, applied,
      fallback;
} mulClockFit_t;
void mulClockFitDefaults(mulClockFitConfig_t *c);
bool mulClockFitConfigValid(const mulClockFitConfig_t *c);
bool mulClockFitConfigEncode(uint8_t out[MUL_CLOCK_CONFIG_BYTES],
                             const mulClockFitConfig_t *c);
bool mulClockFitConfigDecode(const uint8_t in[MUL_CLOCK_CONFIG_BYTES],
                             mulClockFitConfig_t *c);
void mulClockFitReset(mulClockFit_t *s);
/* Returns prediction without changing state/age. False on nonfinite/non-PSD
 * covariance or beyond the existing 2s mapping horizon. */
bool mulClockFitPredict(const mulClockFit_t *s, double seconds, double state[3],
                        double covariance[3][3]);
mulClockResult_t mulClockObserveFitted(const mulNetwork_t *network,
                                       mulClock_t *clock, mulClockFit_t *fit,
                                       const mulClockFitConfig_t *config,
                                       const mulTestPacket_t *packet,
                                       uint64_t rx, uint32_t nowMs, bool valid,
                                       const mulClockDelay_t *delay);
#endif
