/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_legacy_twr.h"
#include <string.h>

static mulAlrRole_t sender(mulTwrType_t type)
{
  return type == mulTwrResponse || type == mulTwrReport ? mulAlrFollower : mulAlrReference;
}

bool mulTwrEncodeNetwork(const mulNetwork_t *network,uint8_t frame[MUL_TWR_FRAME_LENGTH], const mulTwrPacket_t *packet)
{
  if(!frame||!packet||!mulNetworkValid(network)||packet->type<mulTwrPoll||packet->type>mulTwrResult)return false;
  if(!network)network=&mulDefaultNetwork;
  const uint8_t (*addresses)[8]=network->addresses;
  mulAlrRole_t role = sender(packet->type);
  memset(frame, 0, MUL_TWR_FRAME_LENGTH);
  mulEncodeTestPacket(frame, addresses[role], packet->id, packet->stamps[0]);
  memcpy(frame + 5, addresses[1-role], 8);
  frame[24] = packet->type;
  for (unsigned int i = 0; i < 6; i++) {
    mulEncodeTimestamp(frame + 29 + 5*i, packet->stamps[i]);
  }
  frame[68]=1; // Explicit extension version; legacy peers require the same build.
  for (unsigned int i=0;i<3;i++) {
    unsigned int at=69+5*i;
    uint16_t p=(uint16_t)packet->power[i].powerCentiDbm;
    frame[at]=p; frame[at+1]=p>>8; frame[at+2]=(uint8_t)packet->power[i].deltaDtu;
    frame[at+3]=packet->power[i].status; frame[at+4]=packet->power[i].table;
  }
  mulEncodeTimestamp(frame + 59, packet->txTimestamp);
  for (unsigned int i = 0; i < 4; i++) {
    frame[64+i] = packet->tof >> (8*i);
  }
  return true;
}

bool mulTwrDecodeNetwork(const mulNetwork_t *network,const uint8_t *frame, size_t length, mulTwrPacket_t *packet)
{
  if (frame == NULL || packet == NULL || length != MUL_TWR_FRAME_LENGTH ||
      frame[68] != 1 || frame[0] != 0x41 || frame[1] != 0xdc || frame[3] != 0xcf || frame[4] != 0xbc ||
      memcmp(frame+21, "MUL", 3) || frame[24] < mulTwrPoll || frame[24] > mulTwrResult ||
      frame[2] != frame[25]) {
    return false;
  }
  if(!mulNetworkValid(network))return false;
  if(!network)network=&mulDefaultNetwork;
  const uint8_t (*addresses)[8]=network->addresses;
  mulAlrRole_t role = sender(frame[24]);
  if (memcmp(frame+13, addresses[role], 8) || memcmp(frame+5, addresses[1-role], 8)) {
    return false;
  }
  mulTwrPacket_t result = {.type = frame[24]};
  for (unsigned int i = 0; i < 4; i++) {
    result.id |= (uint32_t)frame[25+i] << (8*i);
    result.tof |= (uint32_t)frame[64+i] << (8*i);
  }
  for (unsigned int i = 0; i < 6; i++) {
    result.stamps[i] = mulDecodeTimestamp(frame + 29 + 5*i);
  }
  for (unsigned int i=0;i<3;i++) {
    unsigned int at=69+5*i;
    mulPower_t *p=&result.power[i];
    uint16_t bits=(uint16_t)frame[at] | (uint16_t)frame[at+1]<<8;
    p->powerCentiDbm=bits <= INT16_MAX ? (int16_t)bits : (int16_t)((int32_t)bits-65536);
    p->deltaDtu=frame[at+2] <= INT8_MAX ? (int8_t)frame[at+2] : (int8_t)((int)frame[at+2]-256);
    p->status=frame[at+3]; p->table=frame[at+4];
    unsigned int count=result.type==mulTwrPoll ? 0 : result.type==mulTwrResponse ? 1 : result.type==mulTwrFinal ? 2 : 3;
    if (p->status > mulPowerUnsupported || p->table > 2 || p->deltaDtu < -64 || p->deltaDtu > 64 ||
        (p->status != mulPowerApplied && p->deltaDtu != 0) ||
        (p->status == mulPowerApplied && (p->table == 0 || p->powerCentiDbm < -9500 || p->powerCentiDbm > -6100)) ||
        ((i >= count || p->status == mulPowerMissing) &&
         (p->status || p->table || p->powerCentiDbm || p->deltaDtu))) return false;
  }
  result.txTimestamp = mulDecodeTimestamp(frame+59);
  unsigned int used = result.type == mulTwrPoll ? 1 : result.type == mulTwrResponse ? 3 :
                      result.type == mulTwrFinal ? 5 : 6;
  for (unsigned int i = used; i < 6; i++) {
    if (result.stamps[i] != 0) return false;
  }
  if ((used < 6 && result.txTimestamp != result.stamps[used-1]) ||
      (result.type != mulTwrResult && result.tof != 0) || result.tof > MUL_CLOCK_DELAY_MAX_DTU) {
    return false;
  }
  *packet = result;
  return true;
}

static bool intervalReply(uint64_t a,uint64_t b,unsigned replyMs)
{
  int64_t delta = mulTimestampDifference(a, b);
  return (replyMs==5||replyMs==10||replyMs==20)&&delta >= (int64_t)(3*replyMs*MUL_TWR_DTU_PER_MS/4) && delta <= (int64_t)(3*replyMs*MUL_TWR_DTU_PER_MS/2);
}

bool mulTwrCalculateReply(const uint64_t t[6],uint32_t *tof,unsigned replyMs)
{
  if (t == NULL || tof == NULL || !intervalReply(t[3],t[0],replyMs) || !intervalReply(t[4],t[3],replyMs) ||
      !intervalReply(t[2],t[1],replyMs) || !intervalReply(t[5],t[2],replyMs)) return false;
  uint64_t ra = (uint64_t)mulTimestampDifference(t[3],t[0]);
  uint64_t da = (uint64_t)mulTimestampDifference(t[4],t[3]);
  uint64_t db = (uint64_t)mulTimestampDifference(t[2],t[1]);
  uint64_t rb = (uint64_t)mulTimestampDifference(t[5],t[2]);
  int64_t rateDifference = (int64_t)(rb+db) - (int64_t)(ra+da);
  int64_t rateLimit = (int64_t)((ra+da)*MUL_CLOCK_MAX_PPB / UINT64_C(1000000000));
  if (rateDifference < -rateLimit || rateDifference > rateLimit) return false;
  // Asymmetric DS-TWR, expressed explicitly in REFERENCE units. With constant
  // reciprocal delay, (rb+db)/(ra+da) is follower/reference rate. Replacing the
  // nominal mixed-clock denominator with 2*(rb+db) yields reference DTU exactly.
  // Each interval <=30 ms: both products fit uint64 and subtraction is checked.
  uint64_t roundProduct = ra*rb;
  uint64_t replyProduct = da*db;
  if (roundProduct < replyProduct) return false;
  uint64_t denominator = 2*(rb+db);
  uint64_t numerator = roundProduct - replyProduct;
  uint64_t rounded = (numerator + denominator/2) / denominator;
  if (rounded > (uint64_t)MUL_CLOCK_DELAY_MAX_DTU) return false;
  *tof = (uint32_t)rounded;
  return true;
}

bool mulTwrResultsReply(const uint64_t stamps[6],const mulPower_t power[3],
  uint32_t *raw,uint32_t *corrected,unsigned replyMs)
{
  if(!stamps||!power||!raw||!corrected)return false;
  uint64_t original[6]; memcpy(original,stamps,sizeof(original));
  bool complete=true;
  for (unsigned int i=0;i<3;i++) {
    original[2*i+1]=mulTimestampMask(original[2*i+1]-power[i].deltaDtu);
    if (power[i].status != mulPowerApplied) complete=false;
  }
  *corrected=UINT32_MAX;
  if (!mulTwrCalculateReply(original,raw,replyMs)) return false;
  if (complete) mulTwrCalculateReply(stamps,corrected,replyMs);
  return true;
}


/* Legacy API wrappers retain the default network. */
void mulTwrEncode(uint8_t frame[MUL_TWR_FRAME_LENGTH],const mulTwrPacket_t *packet) { mulTwrEncodeNetwork(NULL,frame,packet); }
bool mulTwrDecode(const uint8_t *frame,size_t length,mulTwrPacket_t *packet) { return mulTwrDecodeNetwork(NULL,frame,length,packet); }

bool mulTwrCalculate(const uint64_t t[6],uint32_t *tof){return mulTwrCalculateReply(t,tof,20);}
bool mulTwrResults(const uint64_t t[6],const mulPower_t p[3],uint32_t *raw,uint32_t *corrected){return mulTwrResultsReply(t,p,raw,corrected,20);}
