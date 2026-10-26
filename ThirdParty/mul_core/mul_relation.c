/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_relation.h"
#include <string.h>
static void put(uint8_t *p,uint64_t v,unsigned int n) { for (unsigned int i=0;i<n;i++) p[i]=v>>(8*i); }
static uint64_t get(const uint8_t *p,unsigned int n) {
  uint64_t v=0; for (unsigned int i=0;i<n;i++) v|=(uint64_t)p[i]<<(8*i); return v;
}
static uint32_t maximumAge(const mulRelation_t *r) { return r->version==2 ? r->maxAgeMs : 1000; }

void mulRelationEncode(uint8_t frame[MUL_RELATION_FRAME_LENGTH],
  const mulTestPacket_t *p,const mulRelation_t *r)
{
  memset(frame,0,MUL_RELATION_FRAME_LENGTH);
  mulEncodeTestPacket(frame,p->source,p->sequence,p->txTimestamp);
  frame[24]=MUL_PACKET_CLOCK_BROADCAST|mulScheduleTag(p->slotMs); frame[34]=r->version==2 ? 2 : 1;
  frame[35]=2 | (r->available ? 1 : 0) | (r->powerCorrectedTwr ? 4 : 0) |
    (r->version==2 ? r->delayState<<3 : 0); // Overall physical bounds UNKNOWN.
  put(frame+36,r->epoch,8); put(frame+44,r->referenceSequence,4);
  put(frame+48,r->twrSequence,4); put(frame+52,r->referenceOrigin,5);
  put(frame+57,r->followerOrigin,5); put(frame+62,(uint32_t)r->skewPpb,4);
  put(frame+66,r->originAgeMs,2); put(frame+68,r->delayAgeMs,2);
  put(frame+70,r->version==2 ? r->delayDtu : 1000,2);
  put(frame+72,r->version==2 ? r->maxAgeMs : r->remainingMs,2);
}

static bool coordinate(const mulRelation_t *r,uint64_t tx,uint64_t *ref,uint32_t *span)
{
  if (r->skewPpb < -MUL_CLOCK_MAX_PPB || r->skewPpb > MUL_CLOCK_MAX_PPB) return false;
  int64_t local=mulTimestampDifference(tx,r->followerOrigin);
  if (local < -(int64_t)MUL_CLOCK_MAP_HORIZON_DTU || local > (int64_t)MUL_CLOCK_MAP_HORIZON_DTU) return false;
  int64_t denominator=INT64_C(1000000000)+r->skewPpb;
  int64_t product=local*r->skewPpb;
  int64_t adjustment=product >= 0 ? (product+denominator/2)/denominator : -((-product+denominator/2)/denominator);
  int64_t d=local-adjustment;
  uint64_t magnitude=(uint64_t)(d<0 ? -d : d);
  if (magnitude > MUL_CLOCK_MAP_HORIZON_DTU) return false;
  *span=(r->delayAgeMs > r->originAgeMs ? r->delayAgeMs-r->originAgeMs : r->originAgeMs-r->delayAgeMs)+
    (uint32_t)((magnitude+63897599)/63897600);
  if (*span < r->delayAgeMs) *span=r->delayAgeMs;
  *ref=mulTimestampMask(r->referenceOrigin+d); return true;
}

bool mulRelationInverse(const mulRelation_t *r,uint64_t tx,uint32_t age,uint64_t *ref)
{
  uint32_t limit=maximumAge(r),span; uint64_t converted;
  if (!r->available || r->epoch==0 || r->remainingMs==0 || age > r->remainingMs ||
      limit<1000 || limit>MUL_PROP_MAX_AGE_MS || r->delayAgeMs>limit ||
      (uint32_t)r->originAgeMs+age > MUL_CLOCK_MAP_HORIZON_MS ||
      (uint32_t)(r->referenceSequence-r->twrSequence)>(r->period10ms ? (limit+r->period10ms*10u-1)/(r->period10ms*10u)+2 : limit<=2000 ? 3u : (limit+999)/1000+2) ||
      (r->version==2 && (r->delayState!=mulPropagationFresh && r->delayState!=mulPropagationReused)) ||
      !coordinate(r,tx,&converted,&span) || span+age>limit ||
      r->remainingMs>limit-span || r->remainingMs>limit-r->delayAgeMs) return false;
  *ref=converted; return true;
}

bool mulBroadcastDecodePeriodNetwork(const mulNetwork_t *network,const uint8_t *frame,size_t length,mulTestPacket_t *p,mulRelation_t *r,uint16_t periodMs)
{
  if (!frame || !p || !r || (periodMs&&(periodMs<10 || periodMs>2000 || periodMs%10))) return false;
  mulTestPacket_t packet; mulRelation_t relation={0};
  if (length==MUL_TEST_FRAME_LENGTH) {
    if (!mulDecodeTestPacket(frame,length,&packet)) return false;
  } else {
    if (length!=MUL_RELATION_FRAME_LENGTH || (frame[24]&0x3f)!=MUL_PACKET_CLOCK_BROADCAST ||
        (frame[34]!=1 && frame[34]!=2) || (frame[35]&2)==0 ||
        (frame[35] & (frame[34]==1 ? ~7 : ~63))!=0) return false;
    uint8_t base[MUL_TEST_FRAME_LENGTH]; memcpy(base,frame,sizeof(base)); base[24]=(frame[24]&0xc0)|MUL_PACKET_TEST_BROADCAST;
    if (!mulDecodeTestPacket(base,sizeof(base),&packet) || mulNetworkRole(network,packet.source)!=mulAlrFollower) return false;
    relation.period10ms=periodMs/10;
    relation.version=frame[34];
    relation.epoch=get(frame+36,8); relation.referenceSequence=get(frame+44,4);
    relation.twrSequence=get(frame+48,4); relation.referenceOrigin=get(frame+52,5);
    relation.followerOrigin=get(frame+57,5);
    uint32_t bits=(uint32_t)get(frame+62,4);
    relation.skewPpb=bits<=INT32_MAX ? (int32_t)bits : (int32_t)((int64_t)bits-INT64_C(4294967296));
    relation.originAgeMs=get(frame+66,2); relation.delayAgeMs=get(frame+68,2);
    relation.available=frame[35]&1; relation.powerCorrectedTwr=(frame[35]&4)!=0;
    if (relation.version==1) {
      if (get(frame+70,2)!=1000) return false;
      relation.remainingMs=get(frame+72,2);
      relation.delayState=relation.available ? mulPropagationFresh : mulPropagationNone;
    } else {
      relation.delayDtu=get(frame+70,2); relation.maxAgeMs=get(frame+72,2);
      relation.delayState=frame[35]>>3;
      if (relation.maxAgeMs<1000 || relation.maxAgeMs>MUL_PROP_MAX_AGE_MS ||
          relation.delayDtu>MUL_CLOCK_DELAY_MAX_DTU || relation.delayState>mulPropagationInvalidated) return false;
      bool valid=relation.delayState==mulPropagationFresh || relation.delayState==mulPropagationReused;
      if (valid && (relation.delayAgeMs>relation.maxAgeMs ||
          relation.delayState!=((uint32_t)(relation.referenceSequence-relation.twrSequence)<=1 ? mulPropagationFresh : mulPropagationReused))) return false;
      if (relation.delayState==mulPropagationExpired && relation.delayAgeMs<=relation.maxAgeMs) return false;
      if ((relation.delayState==mulPropagationNone || relation.delayState==mulPropagationInvalidated) &&
          (relation.delayDtu || relation.delayAgeMs || relation.twrSequence || relation.powerCorrectedTwr)) return false;
      if (relation.available) {
        uint64_t converted; uint32_t span;
        if (!valid || relation.originAgeMs>=2000 || !coordinate(&relation,packet.txTimestamp,&converted,&span) || span>=relation.maxAgeMs) return false;
        uint32_t remaining=relation.maxAgeMs-span;
        if (remaining>2000u-relation.originAgeMs) remaining=2000u-relation.originAgeMs;
        relation.remainingMs=remaining-1; // Conservative guard, derived from original ages.
      }
    }
    if (relation.available) {
      uint64_t converted;
      if (!mulRelationInverse(&relation,packet.txTimestamp,0,&converted)) return false;
    } else if (relation.referenceOrigin || relation.followerOrigin || relation.skewPpb || relation.originAgeMs ||
        (relation.version==1 && (relation.twrSequence || relation.delayAgeMs || relation.remainingMs || relation.powerCorrectedTwr))) return false;
  }
  *p=packet; *r=relation; return true;
}

static void unavailableModel(mulRelation_t *r)
{
  r->available=false; r->referenceOrigin=r->followerOrigin=0;
  r->skewPpb=0; r->originAgeMs=r->remainingMs=0;
  if (r->version!=2) {
    r->twrSequence=0; r->delayAgeMs=r->delayDtu=0;
    r->powerCorrectedTwr=false; r->delayState=mulPropagationNone;
  }
}

mulRelation_t mulRelationPublishNetwork(const mulNetwork_t *network,const mulClock_t *clock,const mulClockDelay_t *delay,
  uint64_t tx,uint64_t hardwareNow,uint32_t nowMs,bool powerCorrectedTwr)
{
  mulRelation_t result={.period10ms=clock->periodMs&&clock->periodMs!=1000?clock->periodMs/10:0,.epoch=clock->epoch,.referenceSequence=clock->sequence,
    .version=MUL_PROP_MAX_AGE_MS==1000 ? 1 : 2,.maxAgeMs=MUL_PROP_MAX_AGE_MS};
  int64_t lead=mulTimestampDifference(tx,hardwareNow);
  if (lead < 0 || lead > INT64_C(600)*63897600) return result;
  uint32_t atTx=nowMs+(uint32_t)((lead+63897599)/63897600)+1;
  if (delay && delay->maxAgeMs==MUL_PROP_MAX_AGE_MS && delay->epoch==clock->epoch && delay->valid &&
      delay->oneWayDtu>=0 && delay->oneWayDtu<=MUL_CLOCK_DELAY_MAX_DTU) {
    uint32_t age=atTx-delay->measuredMs;
    result.delayAgeMs=age>UINT16_MAX ? UINT16_MAX : age;
    result.delayDtu=delay->oneWayDtu; result.twrSequence=delay->referenceSequence;
    result.powerCorrectedTwr=powerCorrectedTwr;
    result.delayState=age>MUL_PROP_MAX_AGE_MS ? mulPropagationExpired :
      (uint32_t)(clock->sequence-delay->referenceSequence)<=1 ? mulPropagationFresh : mulPropagationReused;
  }
  mulClockCorrectedMapping_t origin,inverse;
  if (!delay || delay->maxAgeMs!=MUL_PROP_MAX_AGE_MS ||
      !mulClockMapRefToFollowerSimultaneousNetwork(network,clock,clock->referenceTx,delay,atTx,&origin) ||
      !mulClockMapFollowerSimultaneousToRefNetwork(network,clock,tx,delay,atTx,&inverse)) {
    unavailableModel(&result);
    return result;
  }
  uint32_t span=origin.applicationAgeMs > inverse.applicationAgeMs ? origin.applicationAgeMs : inverse.applicationAgeMs;
  uint32_t remaining=MUL_PROP_MAX_AGE_MS-span;
  uint32_t originRemaining=2000-origin.affine.ageMs;
  if (remaining>originRemaining) remaining=originRemaining;
  if (remaining<=1) { unavailableModel(&result); return result; }
  result.available=true; result.referenceOrigin=clock->referenceTx; result.followerOrigin=origin.timestamp;
  result.skewPpb=clock->skewPpb; result.originAgeMs=origin.affine.ageMs;
  result.delayAgeMs=origin.delayAgeMs; result.remainingMs=remaining-1;
  uint64_t check;
  if (!mulRelationInverse(&result,tx,0,&check)) {
    unavailableModel(&result);
  }
  return result;
}

/* Legacy API wrappers retain the default network. */
bool mulBroadcastDecode(const uint8_t *frame,size_t length,
  mulTestPacket_t *packet,mulRelation_t *relation) { return mulBroadcastDecodeNetwork(NULL,frame,length,packet,relation); }
mulRelation_t mulRelationPublish(const mulClock_t *clock,const mulClockDelay_t *delay,
  uint64_t followerTx,uint64_t hardwareNow,uint32_t nowMs,bool powerCorrectedTwr) { return mulRelationPublishNetwork(NULL,clock,delay,followerTx,hardwareNow,nowMs,powerCorrectedTwr); }

bool mulBroadcastDecodeNetwork(const mulNetwork_t *n,const uint8_t *f,size_t length,mulTestPacket_t *p,mulRelation_t *r) {
 return mulBroadcastDecodePeriodNetwork(n,f,length,p,r,0);
}
