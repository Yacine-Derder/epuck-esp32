/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_TELEMETRY_H
#define MUL_TELEMETRY_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define MUL_TLM_BYTES 152u
#define MUL_TLM_LINE 310u /* UWB2 + 304 hex + NUL; newline owned by transport */
enum {mulTlmSummary=1,mulTlmRange,mulTlmEvent,mulTlmObservation,mulTlmPeer};
enum {mulTlmRawValid=1,mulTlmCalibration=2,mulTlmCalValid=4,mulTlmStampValid=8,mulTlmTx=16};
typedef struct {
 uint8_t type,node,peer,radio,mode,role,flags,remoteRadio,quality;
 uint32_t sequence,transaction,uptimeMs;
 uint64_t epoch,exchange;
 int32_t rawMm,calMm;
 uint32_t attempts,completed,aborted,rejected,invalid;
 uint16_t depth,highWater;
 uint32_t dropped,uartErrors,spiCycles,serviceCycles,gapCycles,clockHz;
 uint32_t radioErrors,timeouts,txDropped,modelVersion;uint64_t remoteEpoch;
 uint64_t stamps[6]; /* ADS: t1..t6; event/observation: local marker, advertised previous TX */
} mulTelemetry_t;
uint16_t mulTelemetryCrc(const uint8_t *p,size_t n);
bool mulTelemetryEncode(char *line,size_t capacity,const mulTelemetry_t *r);
bool mulTelemetryDecode(const char *line,mulTelemetry_t *r);
#endif
