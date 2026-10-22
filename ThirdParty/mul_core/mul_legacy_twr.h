/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_LEGACY_TWR_H
#define MUL_LEGACY_TWR_H
#include "mul_clock.h"
#include "mul_power_types.h"
#define MUL_TWR_FRAME_LENGTH 84 // Original 68 bytes + version 1 + three 5-byte RX records.
#define MUL_TWR_DTU_PER_MS UINT64_C(63897600)
#define MUL_TWR_POLL_MS 650
#define MUL_TWR_PREPARE_MS 630
#define MUL_TWR_END_MS 800
#define MUL_TWR_REPLY_MS 20
#define MUL_TWR_STEP_MS 50

typedef enum {
  mulTwrPoll = 0x10,
  mulTwrResponse,
  mulTwrFinal,
  mulTwrReport,
  mulTwrResult,
} mulTwrType_t;

typedef struct {
  mulTwrType_t type;
  uint32_t id; // Reference broadcast sequence defines the ranging superframe.
  uint64_t stamps[6]; // P1(ref TX), P2(fol RX), P3(fol TX), P4(ref RX), P5(ref TX), P6(fol RX).
  uint64_t txTimestamp; // This message's verified scheduled antenna TX time.
  mulPower_t power[3]; // Receiver-owned P2/P4/P6 correction metadata.
  uint32_t tof; // RESULT only, reference DTU.
} mulTwrPacket_t;

void mulTwrEncode(uint8_t frame[MUL_TWR_FRAME_LENGTH],const mulTwrPacket_t *packet);
bool mulTwrDecode(const uint8_t *frame,size_t length,mulTwrPacket_t *packet);
bool mulTwrCalculate(const uint64_t stamps[6],uint32_t *tof);
bool mulTwrResults(const uint64_t stamps[6],const mulPower_t power[3],uint32_t *raw,uint32_t *corrected);
#endif
