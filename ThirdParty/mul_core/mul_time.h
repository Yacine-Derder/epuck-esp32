/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_TIME_H
#define MUL_TIME_H
#include "mul_common.h"
#define MUL_TIME_HZ UINT64_C(63897600000)
#define MUL_TIME_MAX_DELTA (MUL_TIME_HZ*5) /* < half 40-bit period */
typedef struct {uint8_t node,radio;uint64_t epoch;} mulClockDomain_t;
typedef struct {mulClockDomain_t domain;uint64_t value;bool valid;} mulTimeEvent_t;
typedef struct {
 mulClockDomain_t source,target;
 uint64_t sourceAnchor,targetAnchor;
 double rate,residualDtu;uint32_t samples,rejected,version;
 bool seen,valid; /* Valid APPARENT arrival model; offset includes unknown propagation. */
} mulArrivalModel_t;
bool mulClockSame(mulClockDomain_t a,mulClockDomain_t b);
bool mulTimeDelta(mulTimeEvent_t later,mulTimeEvent_t earlier,int64_t *delta);
bool mulArrivalUpdate(mulArrivalModel_t *m,mulTimeEvent_t tx,mulTimeEvent_t rx);
bool mulArrivalMap(const mulArrivalModel_t *m,mulTimeEvent_t tx,uint64_t *arrival);
#endif
