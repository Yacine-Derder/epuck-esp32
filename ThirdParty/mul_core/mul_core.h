/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_CORE_H
#define MUL_CORE_H
#include "mul_common.h"
#define MUL_CORE_API_VERSION 1u
#define MUL_CORE_DIAG_TYPE 0x70u
#define MUL_CORE_DIAG_VERSION 1u
#define MUL_CORE_DIAG_LENGTH 44u
#define MUL_CORE_WIDTH 40u
#define MUL_CORE_DTU 1u /* nominal 1 / (499.2 MHz * 128), not calibrated SI time */
#define MUL_CORE_DW1000 1u
#define MUL_CORE_DW3000 2u

typedef enum { mulCoreOk, mulCoreArgument, mulCoreLength, mulCoreFormat, mulCoreSource } mulCoreStatus_t;
/* ABI is source-level only: never memcpy this struct onto the wire. */
typedef struct {
  uint8_t source[8];
  uint32_t sequence, markerSequence;
  uint64_t txMarker; /* Previous completed TX, source clock, hardware-adjusted. */
  uint8_t radio;
  bool markerValid; /* false => markerSequence=0 and txMarker=0, NOT timestamp zero. */
} mulCoreDiag_t;
typedef struct {
  uint32_t valid, gaps, duplicates, old, last;
  bool baseline;
} mulCoreSequence_t;
typedef enum { mulCoreFirst, mulCoreForward, mulCoreGap, mulCoreDuplicate, mulCoreOld } mulCoreOrder_t;
typedef struct {
  mulCoreDiag_t message;
  uint8_t receiver[8];
  uint64_t rawRxMarker; /* Receiver clock, uncorrected by application power model. */
  uint8_t receiverRadio;
} mulCoreObservation_t;

bool mulCoreSourceValid(const uint8_t source[8]);
mulCoreStatus_t mulCoreEncodeDiag(uint8_t *frame, size_t capacity, const mulCoreDiag_t *message);
mulCoreStatus_t mulCoreDecodeDiag(const uint8_t *frame, size_t length, mulCoreDiag_t *message);
/* New explicit epoch: preserve cumulative accounting, discard ordering baseline. */
void mulCoreSequenceBeginEpoch(mulCoreSequence_t *state);
void mulCoreSequenceReset(mulCoreSequence_t *state);
mulCoreOrder_t mulCoreObserveSequence(mulCoreSequence_t *state, uint32_t sequence);
mulCoreStatus_t mulCoreObserve(mulCoreObservation_t *out, const mulCoreDiag_t *message,
                              const uint8_t receiver[8], uint8_t radio, uint64_t rawRx);
#endif
