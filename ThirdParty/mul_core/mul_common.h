/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_COMMON_H
#define MUL_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "mul_legacy_timing.h"

#define MUL_ADDRESS_LENGTH 8
#define MUL_TEST_FRAME_LENGTH 34
#define MUL_PACKET_TEST_BROADCAST 1
#define MUL_TIMESTAMP_MASK UINT64_C(0xffffffffff)
#define MUL_TIMESTAMP_BYTES 5
#define MUL_TX_TIMESTAMP_OFFSET 29

/* Wire format (all multibyte fields little endian, no C struct overlay):
 *  0..20: existing compressed-PAN, extended-address 802.15.4 data header
 *         FCF=0xdc41 (no ACK), MAC sequence, PAN=0xbccf, broadcast, source
 * 21..23: "MUL"
 *     24: packet type
 * 25..28: uint32 sequence (MAC sequence is its low byte)
 * 29..33: uint40 expected antenna-referenced TX timestamp, DW1000 time units
 * The radio driver appends/removes the two-byte FCS, not included in length.
 */
typedef struct {
  uint8_t source[MUL_ADDRESS_LENGTH];
  uint32_t sequence;
  uint64_t txTimestamp;
  uint16_t slotMs; // 0 for legacy unadvertised broadcasts.
} mulTestPacket_t;

void mulEncodeTestPacket(uint8_t frame[MUL_TEST_FRAME_LENGTH],
                         const uint8_t source[MUL_ADDRESS_LENGTH], uint32_t sequence,
                         uint64_t txTimestamp);
/* Scheduled broadcasts retain frame lengths and use the packet-type high bits:
 * 01=10ms, 10=50ms, 11=500ms; 00 remains legacy/unadvertised.
 * Older firmware rejects scheduled types rather than silently using wrong timing. */
uint8_t mulScheduleTag(uint16_t slotMs);
uint16_t mulScheduleSlot(uint8_t type);
bool mulEncodeScheduledPacket(uint8_t frame[MUL_TEST_FRAME_LENGTH],
 const uint8_t source[8],uint32_t sequence,uint64_t txTimestamp,uint16_t slotMs);
bool mulDecodeTestPacket(const uint8_t *frame, size_t length, mulTestPacket_t *packet);

uint64_t mulTimestampMask(uint64_t timestamp);
void mulEncodeTimestamp(uint8_t bytes[MUL_TIMESTAMP_BYTES], uint64_t timestamp);
uint64_t mulDecodeTimestamp(const uint8_t bytes[MUL_TIMESTAMP_BYTES]);
// Signed (a - b) modulo 2^40; unambiguous only within half a counter period.
// The exactly-half-period case is represented as -2^39.
int64_t mulTimestampDifference(uint64_t a, uint64_t b);

// Full eight-byte identities, not just the low node-ID byte. Unsupported ALRs
// must never fall back to transmitting. Role values also index bounded RX state.
#define MUL_ALR_COUNT 2
#define MUL_SOURCE_TIMEOUT_MS 3000 // Receiver liveness only; not a UWB timestamp.
typedef enum {
  mulAlrUnsupported = -1,
  mulAlrReference = 0,
  mulAlrFollower = 1,
} mulAlrRole_t;
/* Explicit ordered roster. NULL in Network APIs selects this legacy default;
 * an invalid explicit roster fails closed and never falls back to defaults.
 * Each protocol/receiver instance owns its roster; no mutable global routing. */
typedef struct {
 uint8_t addresses[MUL_ALR_COUNT][MUL_ADDRESS_LENGTH];
 uint16_t slotMs; // 0: legacy API fallback; nonzero: require schedule advertisement.
} mulNetwork_t;
bool mulSlotValid(uint32_t ms);
uint16_t mulNetworkSlotMs(const mulNetwork_t *n);
uint16_t mulSlotToleranceMs(uint16_t ms);
int64_t mulSlotMinDtu(uint16_t ms);
int64_t mulSlotMaxDtu(uint16_t ms);
uint32_t mulNetworkPairMaxAgeMs(const mulNetwork_t *n);
bool mulNetworkScheduleMatches(const mulNetwork_t *n,uint16_t advertised);
extern const mulNetwork_t mulDefaultNetwork;
bool mulNetworkInit(mulNetwork_t *network,uint8_t reference,uint8_t follower);
bool mulNetworkValid(const mulNetwork_t *network);
mulAlrRole_t mulNetworkRole(const mulNetwork_t *network,const uint8_t address[8]);
mulAlrRole_t mulAlrRoleForAddress(const uint8_t address[MUL_ADDRESS_LENGTH]);

typedef enum {
  mulSequenceFirst,
  mulSequenceForward,
  mulSequenceGap,
  mulSequenceDuplicate,
  mulSequenceOld,
} mulSequenceResult_t;

typedef struct {
  bool hasSequence;
  uint32_t sequence; // Last accepted sequence, not the last packet seen.
  uint32_t received; // Valid, supported packets, including duplicates/old packets.
  uint32_t gaps; // Skipped sequence positions; late arrivals do not undo this count.
  uint32_t duplicates;
  uint32_t outOfOrder;
  uint64_t txTimestamp; // Source clock; never subtract from rxTimestamp.
  uint64_t rxTimestamp; // Local receiver clock, from the same accepted observation.
} mulSourceState_t;

// Serial-number ordering modulo 2^32: forward distances must be < 2^31.
// Older repeats are counted as out-of-order; only a repeat of the latest is DUP.
mulSequenceResult_t mulObservePacket(mulSourceState_t *state,
                                    const mulTestPacket_t *packet, uint64_t localRx);

#endif // MUL_COMMON_H
