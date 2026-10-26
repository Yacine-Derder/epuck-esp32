/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_plan_protocol.h"
#include <string.h>
static uint32_t hash(const uint8_t *b){return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);}
static void putHash(uint8_t *b,uint32_t n){for(unsigned i=0;i<4;i++)b[i]=(uint8_t)(n>>(8*i));}
size_t mulPlanBroadcastEncode(uint8_t *out,size_t capacity,const mulRangingPlan_t *p,unsigned slot,uint32_t cycle,uint64_t tx,const mulRelation_t *r) {
 if(mulRangingPlanValidate(p,NULL))return 0;
 size_t n=mulMultiBroadcastEncode(out,capacity,&p->broadcast,slot,cycle,tx,r);
 if(n){out[34]=MUL_PLAN_RADIO_VERSION;putHash(out+36,mulRangingPlanFingerprint(p));}
 return n;
}
bool mulPlanBroadcastDecode(const mulRangingPlan_t *p,const uint8_t *frame,size_t n,mulMultiBroadcast_t *out) {
 if(!frame||!out||mulRangingPlanValidate(p,NULL)||(n!=40&&n!=80)||frame[34]!=MUL_PLAN_RADIO_VERSION||hash(frame+36)!=mulRangingPlanFingerprint(p))return false;
 uint8_t base[MUL_MULTI_FOLLOWER_BYTES];memcpy(base,frame,n);base[34]=3;putHash(base+36,mulScheduleFingerprint(&p->broadcast));
 return mulMultiBroadcastDecode(&p->broadcast,base,n,out);
}
bool mulPlanOperationNetwork(const mulRangingPlan_t *p,uint32_t cycle,unsigned index,mulNetwork_t *network) {
 mulRangingOperation_t op;if(!network||!mulRangingPlanOperation(p,cycle,index,&op))return false;
 if(!mulNetworkInit(network,op.initiator,op.responder))return false;
 network->slotMs=p->broadcast.slotMs;return true;
}
bool mulPlanTwrEncode(uint8_t out[MUL_MULTI_TWR_BYTES],const mulRangingPlan_t *p,unsigned index,const mulTwrPacket_t *packet) {
 mulNetwork_t network;if(!out||!packet||!mulPlanOperationNetwork(p,packet->id,index,&network)||!mulTwrEncodeNetwork(&network,out,packet))return false;
 out[24]+=8;out[84]=MUL_PLAN_RADIO_VERSION;out[85]=(uint8_t)index;putHash(out+86,mulRangingPlanFingerprint(p));return true;
}
bool mulPlanTwrDecode(const mulRangingPlan_t *p,uint32_t cycle,unsigned index,const uint8_t *frame,size_t n,mulTwrPacket_t *out) {
 if(!frame||!out||n!=MUL_MULTI_TWR_BYTES||frame[84]!=MUL_PLAN_RADIO_VERSION||frame[85]!=index||frame[24]<0x18||frame[24]>0x1c||hash(frame+86)!=mulRangingPlanFingerprint(p))return false;
 mulNetwork_t network;if(!mulPlanOperationNetwork(p,cycle,index,&network))return false;
 uint8_t base[MUL_TWR_FRAME_LENGTH];memcpy(base,frame,sizeof(base));base[24]-=8;mulTwrPacket_t packet;
 if(!mulTwrDecodeNetwork(&network,base,sizeof(base),&packet)||packet.id!=cycle)return false;
 *out=packet;return true;
}
