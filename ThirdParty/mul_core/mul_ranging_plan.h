/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_RANGING_PLAN_H
#define MUL_RANGING_PLAN_H
#include "mul_schedule.h"
#define MUL_MAX_RLRS 8u
#define MUL_PLAN_MAX_OPERATIONS 32u
#define MUL_PLAN_BYTES 64u
#define MUL_PLAN_VERSION 2u
#define MUL_PLAN_FRESHNESS_VERSION 3u
#define MUL_PLAN_DELAY_VERSION 4u
#define MUL_PLAN_FAIR 0u
#define MUL_PLAN_FRESHNESS 1u
#define MUL_PLAN_SERVICE_MARGIN_US 1000u // One1ms service quantum before TX setup.
#define MUL_PLAN_AA 1u
#define MUL_PLAN_AR 2u
/* Per-pair rates, in milliHz. This version is a planner/configuration codec;
 * radio adapters must explicitly opt in before using these reservations. */
typedef struct {
 mulSchedule_t broadcast;
 uint32_t aaStartDelayUs; // Shift AA windows only; zero preserves codec2/3 and timing.
 uint16_t aaMilliHz,arMilliHz;
 uint8_t rlrCount,rlrs[MUL_MAX_RLRS],enabled,replyMs,maxUtilization;
 uint8_t policy,aaPriority,arPriority; // EDF tie priorities; never inflate promised rates.
} mulRangingPlan_t;
typedef struct {
 uint32_t dataEndUs,reservationUs,prepareUs,usableEndUs,requestedMilliHz;
 uint16_t capacity,peakOperations;
 uint8_t aaPairs,arPairs;
} mulRangingBudget_t;
typedef struct {
 uint32_t startUs,pollUs,endUs;
 uint8_t kind,initiator,responder;
} mulRangingOperation_t;
uint8_t mulRangingPlanVersion(const mulRangingPlan_t *p);
const char *mulRangingPlanValidate(const mulRangingPlan_t *p,mulRangingBudget_t *budget);
unsigned mulRangingPlanCount(const mulRangingPlan_t *p,uint32_t cycle);
bool mulRangingPlanOperation(const mulRangingPlan_t *p,uint32_t cycle,unsigned index,mulRangingOperation_t *out);
bool mulRangingPlanEncode(uint8_t out[MUL_PLAN_BYTES],const mulRangingPlan_t *p);
bool mulRangingPlanDecode(const uint8_t in[MUL_PLAN_BYTES],mulRangingPlan_t *p);
const char *mulRangingPlanDecodeError(const uint8_t in[MUL_PLAN_BYTES],mulRangingPlan_t *p);
uint32_t mulRangingPlanFingerprint(const mulRangingPlan_t *p);
#endif
