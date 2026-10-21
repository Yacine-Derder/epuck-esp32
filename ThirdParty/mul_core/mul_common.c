/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_common.h"

#include <string.h>

mulAlrRole_t mulAlrRoleForAddress(const uint8_t address[MUL_ADDRESS_LENGTH])
{
  static const uint8_t addresses[MUL_ALR_COUNT][MUL_ADDRESS_LENGTH] = {
    {0x84, 0, 0, 0, 0, 0, 0xcf, 0xbc},
    {0x85, 0, 0, 0, 0, 0, 0xcf, 0xbc},
  };
  if (address != NULL) {
    for (int i = 0; i < MUL_ALR_COUNT; i++) {
      if (memcmp(address, addresses[i], MUL_ADDRESS_LENGTH) == 0) {
        return (mulAlrRole_t)i;
      }
    }
  }
  return mulAlrUnsupported;
}

mulSequenceResult_t mulObservePacket(mulSourceState_t *state,
                                    const mulTestPacket_t *packet, uint64_t localRx)
{
  state->received++;
  mulSequenceResult_t result = mulSequenceFirst;
  if (state->hasSequence) {
    uint32_t advance = packet->sequence - state->sequence;
    if (advance == 0) {
      state->duplicates++;
      return mulSequenceDuplicate;
    }
    if (advance >= UINT32_C(0x80000000)) {
      state->outOfOrder++;
      return mulSequenceOld;
    }
    state->gaps += advance - 1;
    result = advance == 1 ? mulSequenceForward : mulSequenceGap;
  }
  state->hasSequence = true;
  state->sequence = packet->sequence;
  state->txTimestamp = mulTimestampMask(packet->txTimestamp);
  state->rxTimestamp = mulTimestampMask(localRx);
  return result;
}

uint64_t mulTimestampMask(uint64_t timestamp)
{
  return timestamp & MUL_TIMESTAMP_MASK;
}

void mulEncodeTimestamp(uint8_t bytes[MUL_TIMESTAMP_BYTES], uint64_t timestamp)
{
  for (unsigned int i = 0; i < MUL_TIMESTAMP_BYTES; i++) {
    bytes[i] = (uint8_t)(timestamp >> (8 * i));
  }
}

uint64_t mulDecodeTimestamp(const uint8_t bytes[MUL_TIMESTAMP_BYTES])
{
  uint64_t timestamp = 0;
  for (unsigned int i = 0; i < MUL_TIMESTAMP_BYTES; i++) {
    timestamp |= (uint64_t)bytes[i] << (8 * i);
  }
  return timestamp;
}

int64_t mulTimestampDifference(uint64_t a, uint64_t b)
{
  uint64_t difference = mulTimestampMask(a - b);
  if (difference & (UINT64_C(1) << 39)) {
    return (int64_t)difference - (INT64_C(1) << 40);
  }
  return (int64_t)difference;
}

void mulEncodeTestPacket(uint8_t frame[MUL_TEST_FRAME_LENGTH],
                         const uint8_t source[MUL_ADDRESS_LENGTH], uint32_t sequence,
                         uint64_t txTimestamp)
{
  frame[0] = 0x41;
  frame[1] = 0xdc;
  frame[2] = (uint8_t)sequence;
  frame[3] = 0xcf;
  frame[4] = 0xbc;
  memset(&frame[5], 0xff, MUL_ADDRESS_LENGTH);
  memcpy(&frame[13], source, MUL_ADDRESS_LENGTH);
  memcpy(&frame[21], "MUL", 3);
  frame[24] = MUL_PACKET_TEST_BROADCAST;
  for (unsigned int i = 0; i < 4; i++) {
    frame[25 + i] = (uint8_t)(sequence >> (8 * i));
  }
  mulEncodeTimestamp(&frame[MUL_TX_TIMESTAMP_OFFSET], txTimestamp);
}

bool mulDecodeTestPacket(const uint8_t *frame, size_t length, mulTestPacket_t *packet)
{
  if (length != MUL_TEST_FRAME_LENGTH || frame == NULL || packet == NULL) {
    return false;
  }
  if (frame[0] != 0x41 || frame[1] != 0xdc ||
      frame[3] != 0xcf || frame[4] != 0xbc ||
      memcmp(&frame[21], "MUL", 3) != 0 ||
      frame[24] != MUL_PACKET_TEST_BROADCAST || frame[2] != frame[25]) {
    return false;
  }
  for (unsigned int i = 0; i < MUL_ADDRESS_LENGTH; i++) {
    if (frame[5 + i] != 0xff) {
      return false;
    }
  }

  memcpy(packet->source, &frame[13], MUL_ADDRESS_LENGTH);
  packet->sequence = 0;
  for (unsigned int i = 0; i < 4; i++) {
    packet->sequence |= (uint32_t)frame[25 + i] << (8 * i);
  }
  packet->txTimestamp = mulDecodeTimestamp(&frame[MUL_TX_TIMESTAMP_OFFSET]);
  return true;
}
