/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_BEACON_H
#define MUL_BEACON_H
#include "mul_time.h"
#define MUL_BEACON_LENGTH 56u
#define MUL_BEACON_TYPE 0x75u
typedef struct {uint8_t source,radio;uint64_t epoch,previousTx;uint32_t sequence,previousSequence;bool markerValid;} mulBeacon_t;
bool mulBeaconEncode(uint8_t *f,size_t capacity,const mulBeacon_t *p);
bool mulBeaconDecode(const uint8_t *f,size_t length,mulBeacon_t *p);
#endif
