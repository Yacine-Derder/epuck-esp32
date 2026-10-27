/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_MULTI_RECEIVER_H
#define MUL_MULTI_RECEIVER_H
#include "mul_multi_protocol.h"
#include "mul_plan_protocol.h"
#include "mul_localization.h"
#include "mul_clock_fit.h"
typedef enum {
 mulMultiOk=0,mulMultiMissing,mulMultiNoReference,mulMultiCaptureAge,
 mulMultiScheduleMismatch,mulMultiClockUnavailable,mulMultiNoRate,
 mulMultiStaleRate,mulMultiTiming,mulMultiSequence
} mulMultiReason_t;
typedef struct {int32_t rangeDifferenceMm,tdoaDtu;uint8_t status,clockValid,rateState;uint8_t reason:4,source:2;} mulMultiValue_t;
typedef struct {
 uint32_t cycle,observedMs,fingerprint,revision;
 uint64_t referenceRx;
 uint16_t slotMs,periodMs;
 uint8_t count,ids[MUL_MAX_ALRS],seenMask,validMask,sourceVersion;
 mulMultiValue_t values[MUL_MAX_ALRS-1];
} mulMultiCycle_t;
typedef struct {
 const mulRangingPlan_t *plan; // Optional immutable caller-owned configuration.
 mulSchedule_t schedule;mulClock_t rate;
 mulClockFit_t fit;mulClockFitConfig_t clockConfig;
 mulObservation_t sources[MUL_MAX_ALRS];
 mulRelation_t relations[MUL_MAX_ALRS-1];
 uint64_t followerEpoch[MUL_MAX_ALRS-1];
 mulMultiValue_t values[MUL_MAX_ALRS-1];
 uint16_t captureAge[MUL_MAX_ALRS];
 uint32_t cycle,cycleStartMs,lastCycleMs;
 uint32_t received,rejected,duplicates,old,late,mismatches,cycles;
 uint8_t seenMask;
 bool stationary,hasCycle,active;
} mulMultiReceiver_t;
typedef struct {mulMultiBroadcast_t broadcast;mulMultiCycle_t completed;bool cycleReady;} mulMultiReceiveOutput_t;
bool mulMultiReceiverInit(mulMultiReceiver_t *s,const mulSchedule_t *schedule,const uint8_t receiver[8],bool stationary);
/* Passive reconstruction only; this API never authorizes scheduled RLR TX.
 * Keep plan immutable/alive until reinitialization. */
bool mulMultiReceiverInitPlan(mulMultiReceiver_t *s,const mulRangingPlan_t *plan,const uint8_t receiver[8],bool stationary);
bool mulMultiReceiverPoll(mulMultiReceiver_t *s,uint32_t nowMs,mulMultiCycle_t *out);
bool mulMultiReceiverReceive(mulMultiReceiver_t *s,const uint8_t *frame,size_t n,uint64_t rx,
 uint32_t captureAgeMs,uint32_t nowMs,mulMultiReceiveOutput_t *out);
#endif
