/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_multi_protocol.h"
#include <string.h>
static void putHash(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static uint32_t getHash(const uint8_t *p){uint32_t v=0;for(unsigned i=0;i<4;i++)v|=(uint32_t)p[i]<<(8*i);return v;}
static bool multi(const mulSchedule_t *s){return !mulScheduleValidate(s)&&s->profile>=MUL_PROFILE_MULTI;}
size_t mulMultiBroadcastEncode(uint8_t *out,size_t capacity,const mulSchedule_t *s,unsigned slot,
 uint32_t cycle,uint64_t tx,const mulRelation_t *relation) {
 size_t n=slot?MUL_MULTI_FOLLOWER_BYTES:MUL_MULTI_REFERENCE_BYTES;
 if(!out||!multi(s)||slot>=s->count||capacity<n||tx>MUL_TIMESTAMP_MASK||(slot&&!relation)||(!slot&&relation))return 0;
 uint8_t base[MUL_RELATION_FRAME_LENGTH]={0};
 mulTestPacket_t p={.sequence=cycle,.txTimestamp=tx};mulNetwork_t pair;
 if(!mulSchedulePair(s,slot?slot:1,&pair))return 0;
 memcpy(p.source,pair.addresses[slot?1:0],8);
 if(slot) {
  if(relation->available&&relation->referenceSequence!=cycle)return 0;
  mulRelationEncode(base,&p,relation);
  mulTestPacket_t check;mulRelation_t checked;
  if(!mulBroadcastDecodePeriodNetwork(&pair,base,sizeof(base),&check,&checked,s->profile==MUL_PROFILE_EXPERIMENTAL?s->periodMs:0))return 0;
 } else mulEncodeTestPacket(base,p.source,cycle,tx);
 memcpy(out,base,34);out[24]=slot?MUL_MULTI_FOLLOWER_TYPE:MUL_MULTI_REFERENCE_TYPE;
 out[34]=s->profile==MUL_PROFILE_EXPERIMENTAL?3:MUL_MULTI_VERSION;out[35]=(uint8_t)slot;putHash(out+36,mulScheduleFingerprint(s));
 if(slot)memcpy(out+40,base+34,40);
 return n;
}
bool mulMultiBroadcastDecode(const mulSchedule_t *s,const uint8_t *frame,size_t n,mulMultiBroadcast_t *out) {
 if(!out||!frame||!multi(s)||(n!=MUL_MULTI_REFERENCE_BYTES&&n!=MUL_MULTI_FOLLOWER_BYTES))return false;
 unsigned slot=frame[35];
 if(frame[34]!=(s->profile==MUL_PROFILE_EXPERIMENTAL?3:MUL_MULTI_VERSION)||slot>=s->count||getHash(frame+36)!=mulScheduleFingerprint(s)||
    frame[24]!=(slot?MUL_MULTI_FOLLOWER_TYPE:MUL_MULTI_REFERENCE_TYPE)||n!=(slot?MUL_MULTI_FOLLOWER_BYTES:MUL_MULTI_REFERENCE_BYTES))return false;
 mulNetwork_t pair;if(!mulSchedulePair(s,slot?slot:1,&pair))return false;
 uint8_t base[MUL_RELATION_FRAME_LENGTH]={0};memcpy(base,frame,34);base[24]=slot?MUL_PACKET_CLOCK_BROADCAST:MUL_PACKET_TEST_BROADCAST;
 if(slot)memcpy(base+34,frame+40,40);
 mulMultiBroadcast_t decoded={.slot=(uint8_t)slot};
 if(!mulBroadcastDecodePeriodNetwork(&pair,base,slot?74:34,&decoded.packet,&decoded.relation,s->profile==MUL_PROFILE_EXPERIMENTAL?s->periodMs:0)||
    memcmp(decoded.packet.source,pair.addresses[slot?1:0],8)||
    (slot&&decoded.relation.available&&decoded.relation.referenceSequence!=decoded.packet.sequence))return false;
 *out=decoded;return true;
}
bool mulMultiTwrEncode(uint8_t out[MUL_MULTI_TWR_BYTES],const mulSchedule_t *s,unsigned window,const mulTwrPacket_t *p) {
 if(!out||!p||!multi(s))return false;
 unsigned slot=mulScheduleSyncOwner(s,p->id,window);mulNetwork_t pair;
 if(!slot||!mulSchedulePair(s,slot,&pair)||!mulTwrEncodeNetwork(&pair,out,p))return false;
 out[24]+=8;out[84]=s->profile==MUL_PROFILE_EXPERIMENTAL?3:MUL_MULTI_VERSION;out[85]=(uint8_t)slot;putHash(out+86,mulScheduleFingerprint(s));return true;
}
bool mulMultiTwrDecode(const mulSchedule_t *s,unsigned window,const uint8_t *frame,size_t n,mulTwrPacket_t *out,unsigned *slot) {
 if(!frame||!out||!slot||!multi(s)||n!=MUL_MULTI_TWR_BYTES||frame[84]!=(s->profile==MUL_PROFILE_EXPERIMENTAL?3:MUL_MULTI_VERSION)||
    getHash(frame+86)!=mulScheduleFingerprint(s)||frame[24]<0x18||frame[24]>0x1c)return false;
 mulNetwork_t pair;unsigned follower=frame[85];if(!mulSchedulePair(s,follower,&pair))return false;
 uint8_t base[84];memcpy(base,frame,sizeof(base));base[24]-=8;mulTwrPacket_t p;
 if(!mulTwrDecodeNetwork(&pair,base,sizeof(base),&p)||mulScheduleSyncOwner(s,p.id,window)!=follower)return false;
 *out=p;*slot=follower;return true;
}
