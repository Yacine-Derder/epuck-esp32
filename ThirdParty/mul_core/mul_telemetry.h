/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_TELEMETRY_H
#define MUL_TELEMETRY_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "mul_multi_receiver.h"
#define MUL_TLM_BYTES 152u
#define MUL_TLM_LINE 310u /* UWB2 + 304 hex + NUL; newline owned by transport */
enum {mulTlmSummary=1,mulTlmRange,mulTlmEvent,mulTlmObservation,mulTlmPeer,mulTlmHybridRx,mulTlmTdoa,mulTlmVector,mulTlmMultiRx,mulTlmClock};
enum {mulTlmRawValid=1,mulTlmCalibration=2,mulTlmCalValid=4,mulTlmStampValid=8,mulTlmTx=16};
/* UWB3 type7: actual onboard association result, never an ALR-ALR range.
 * INT32_MIN remains the wire sentinel; consumers expose unavailable values null. */
typedef struct {
 uint64_t rx;
 uint32_t referenceSequence,followerSequence,delayAgeMs;
 int32_t tdoaDtu,rangeDifferenceMm,ratePpb;
 uint16_t delayDtu,clockAgeMs;
 uint8_t reference,follower,status,clockValid,rateState,delayState,powerFlags,unknownComponents;
} mulTdoaTelemetry_t;
typedef struct {
 uint64_t referenceTx,rawRx;
 uint32_t referenceSequence,rmsDtu,rejected,fitSigmaPpb,ageMs;
 int32_t appliedPpb,fittedPpb;
 uint8_t reference,method,window,count,inliers,status,ready,applied,fallback,compensated;
} mulClockTelemetry_t;
typedef struct {
 mulTdoaTelemetry_t tdoa;
 mulClockTelemetry_t clock;
 mulMultiCycle_t vector;
 uint32_t scheduleFingerprint,scheduleRevision;
 uint8_t type,node,peer,radio,mode,role,flags,remoteRadio,quality;
 uint32_t sequence,transaction,uptimeMs;
 uint64_t epoch,exchange;
 int32_t rawMm,calMm;
 uint32_t attempts,completed,aborted,rejected,invalid;
 uint16_t depth,highWater;
 uint32_t dropped,uartErrors,spiCycles,serviceCycles,gapCycles,clockHz;
 uint32_t radioErrors,timeouts,txDropped,modelVersion;uint64_t remoteEpoch;
 uint8_t legacyFrame[90],legacyLength,ratePolicy;
 uint64_t legacyRx;uint16_t captureAgeMs,legacySlotMs;
 uint32_t referenceGeneration,followerGeneration;
 uint64_t stamps[6]; /* ADS: t1..t6; event/observation: local marker, advertised previous TX */
} mulTelemetry_t;
uint16_t mulTelemetryCrc(const uint8_t *p,size_t n);
bool mulTelemetryEncode(char *line,size_t capacity,const mulTelemetry_t *r);
bool mulTelemetryDecode(const char *line,mulTelemetry_t *r);
#endif
