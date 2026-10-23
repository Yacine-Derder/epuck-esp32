/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_MULTI_PROTOCOL_H
#define MUL_MULTI_PROTOCOL_H
#include "mul_schedule.h"
#include "mul_relation.h"
#include "mul_legacy_twr.h"
#define MUL_MULTI_VERSION 2u
#define MUL_MULTI_REFERENCE_BYTES 40u
#define MUL_MULTI_FOLLOWER_BYTES 80u
#define MUL_MULTI_TWR_BYTES 90u
#define MUL_MULTI_REFERENCE_TYPE 0x30u
#define MUL_MULTI_FOLLOWER_TYPE 0x31u
/* Radio FCS is not included in any length. All frames fit127bytes including FCS.
 * Broadcast: existing34-byte prefix, new type30/31; version2 at34, slot at35,
 * little-endian schedule CRC32 at36..39; follower relation bytes34..73 move40..79.
 * TWR: existing84-byte payload with type18..1c; version2/slot/hash at84/85/86..89.
 * Broadcast sequence is the reference cycle, including followers. Source remains
 * a full canonical8-byte MAC identity; fingerprint binds reference/roster/timing.
 */
typedef struct {mulTestPacket_t packet;mulRelation_t relation;uint8_t slot;} mulMultiBroadcast_t;
size_t mulMultiBroadcastEncode(uint8_t *out,size_t capacity,const mulSchedule_t *s,unsigned slot,
 uint32_t cycle,uint64_t tx,const mulRelation_t *relation);
bool mulMultiBroadcastDecode(const mulSchedule_t *s,const uint8_t *frame,size_t n,mulMultiBroadcast_t *out);
bool mulMultiTwrEncode(uint8_t out[MUL_MULTI_TWR_BYTES],const mulSchedule_t *s,unsigned window,const mulTwrPacket_t *packet);
bool mulMultiTwrDecode(const mulSchedule_t *s,unsigned window,const uint8_t *frame,size_t n,mulTwrPacket_t *out,unsigned *slot);
#endif
