/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_CLOCK_FIT_H
#define MUL_CLOCK_FIT_H
#include "mul_clock.h"
#define MUL_CLOCK_FIT_MAX 32u
#define MUL_CLOCK_FIT_DEFAULT 24u
#define MUL_CLOCK_CONFIG_BYTES 12u
enum {mulClockEstimatorMedian=0,mulClockEstimatorRegression=1};
enum {mulClockFitUsed=0,mulClockFitWarmup=1,mulClockFitChangingRate=2,mulClockFitOutlier=3};
typedef struct {
 uint16_t periodMs,gateDtu;
 uint8_t estimator,window,minSamples,compensate;
} mulClockFitConfig_t;
/* Coordinates are reference time/65536 and local-minus-reference DTU. The
 * window is rebased on eviction; each raw40-bit difference is unwrapped only
 * across a validated <3s interval. No absolute timestamp is cast to float. */
typedef struct {
 int32_t x[MUL_CLOCK_FIT_MAX],y[MUL_CLOCK_FIT_MAX];
 uint64_t lastTx,lastRx,elapsed,epoch;
 int64_t phase;
 int32_t skewPpb,centerX,centerY;
 uint32_t accepted,rejected,resets,rmsDtu,uncertaintyPpb;
 uint8_t count,next,inliers,consecutive,compensated,ready,applied,fallback;
} mulClockFit_t;
void mulClockFitDefaults(mulClockFitConfig_t *c);
bool mulClockFitConfigValid(const mulClockFitConfig_t *c);
bool mulClockFitConfigEncode(uint8_t out[MUL_CLOCK_CONFIG_BYTES],const mulClockFitConfig_t *c);
bool mulClockFitConfigDecode(const uint8_t in[MUL_CLOCK_CONFIG_BYTES],mulClockFitConfig_t *c);
void mulClockFitReset(mulClockFit_t *s);
/* The baseline median always runs. Regression overrides only the frequency;
 * mapping origins remain RAW arrival-aligned timestamps, so downstream delay
 * correction is still applied exactly once. Delay uncertainty is not a physical
 * clock bound. Switching compensation availability restarts the fit window. */
mulClockResult_t mulClockObserveFitted(const mulNetwork_t *network,mulClock_t *clock,
 mulClockFit_t *fit,const mulClockFitConfig_t *config,const mulTestPacket_t *packet,
 uint64_t rx,uint32_t nowMs,bool valid,const mulClockDelay_t *delay);
#endif
