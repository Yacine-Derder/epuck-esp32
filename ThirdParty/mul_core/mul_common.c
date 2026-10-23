/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_common.h"

#include <string.h>

const mulNetwork_t mulDefaultNetwork={.addresses={
  {0x84,0,0,0,0,0,0xcf,0xbc},{0x85,0,0,0,0,0,0xcf,0xbc}}};
bool mulNetworkValid(const mulNetwork_t *network) {
  if(!network)network=&mulDefaultNetwork;
  if(network->slotMs&&!mulSlotValid(network->slotMs))return false;
  for(unsigned i=0;i<MUL_ALR_COUNT;i++) {
    const uint8_t *a=network->addresses[i];
    if(a[0]==255||a[6]!=0xcf||a[7]!=0xbc)return false;
    for(unsigned j=1;j<6;j++)if(a[j])return false;
  }
  return memcmp(network->addresses[0],network->addresses[1],8)!=0;
}
bool mulNetworkInit(mulNetwork_t *network,uint8_t reference,uint8_t follower) {
  if(!network)return false;
  mulNetwork_t candidate=mulDefaultNetwork;
  candidate.addresses[0][0]=reference;candidate.addresses[1][0]=follower;
  if(!mulNetworkValid(&candidate))return false;
  *network=candidate;return true;
}
mulAlrRole_t mulNetworkRole(const mulNetwork_t *network,const uint8_t address[8]) {
  if(!network)network=&mulDefaultNetwork;
  if(address&&mulNetworkValid(network))for(int i=0;i<MUL_ALR_COUNT;i++)
    if(!memcmp(address,network->addresses[i],8))return (mulAlrRole_t)i;
  return mulAlrUnsupported;
}
mulAlrRole_t mulAlrRoleForAddress(const uint8_t address[MUL_ADDRESS_LENGTH]) {
  return mulNetworkRole(NULL,address);
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

uint8_t mulScheduleTag(uint16_t slot) {return slot==10?0x40:slot==50?0x80:slot==500?0xc0:0;}
uint16_t mulScheduleSlot(uint8_t type) {return (type>>6)==1?10:(type>>6)==2?50:(type>>6)==3?500:0;}
bool mulEncodeScheduledPacket(uint8_t frame[MUL_TEST_FRAME_LENGTH],const uint8_t source[8],
 uint32_t sequence,uint64_t timestamp,uint16_t slot) {
 if(!frame||!source||!mulSlotValid(slot))return false;
 mulEncodeTestPacket(frame,source,sequence,timestamp);frame[24]|=mulScheduleTag(slot);return true;
}
bool mulDecodeTestPacket(const uint8_t *frame, size_t length, mulTestPacket_t *packet)
{
  if (length != MUL_TEST_FRAME_LENGTH || frame == NULL || packet == NULL) {
    return false;
  }
  if (frame[0] != 0x41 || frame[1] != 0xdc ||
      frame[3] != 0xcf || frame[4] != 0xbc ||
      memcmp(&frame[21], "MUL", 3) != 0 ||
      (frame[24]&0x3f) != MUL_PACKET_TEST_BROADCAST || frame[2] != frame[25]) {
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
  packet->slotMs=mulScheduleSlot(frame[24]);
  packet->txTimestamp = mulDecodeTimestamp(&frame[MUL_TX_TIMESTAMP_OFFSET]);
  return true;
}

uint16_t mulSlotToleranceMs(uint16_t ms) {return ms/10<2?2:ms/10;}
int64_t mulSlotMinDtu(uint16_t ms) {return (ms-mulSlotToleranceMs(ms))*INT64_C(63897600);}
int64_t mulSlotMaxDtu(uint16_t ms) {return (ms+mulSlotToleranceMs(ms))*INT64_C(63897600);}

bool mulSlotValid(uint32_t ms) {return ms==10||ms==50||ms==500;}
uint16_t mulNetworkSlotMs(const mulNetwork_t *n) {return n&&n->slotMs?n->slotMs:MUL_FOLLOWER_SLOT_MS;}
uint32_t mulNetworkPairMaxAgeMs(const mulNetwork_t *n) {return mulNetworkSlotMs(n)+200u;}
bool mulNetworkScheduleMatches(const mulNetwork_t *n,uint16_t advertised) {
 return advertised ? advertised==mulNetworkSlotMs(n) : !n||!n->slotMs;
}
