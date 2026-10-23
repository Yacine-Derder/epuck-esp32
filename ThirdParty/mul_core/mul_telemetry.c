/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_telemetry.h"
#include <string.h>
static void put(uint8_t *p,uint64_t v,unsigned n){for(unsigned i=0;i<n;i++)p[i]=(uint8_t)(v>>(8*i));}
static uint64_t get(const uint8_t *p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
uint16_t mulTelemetryCrc(const uint8_t *p,size_t n){uint16_t c=0xffff;for(size_t i=0;i<n;i++){c^=(uint16_t)p[i]<<8;for(unsigned j=0;j<8;j++)c=(uint16_t)((c<<1)^((c&0x8000)?0x1021:0));}return c;}
static bool valid(const mulTelemetry_t *r){
 if(!r||r->type<1||r->type>9||r->radio<1||r->radio>2||!r->epoch||r->mode>5||r->role<1||r->role>2||r->flags>31||r->remoteRadio>2)return false;
 if(r->type==mulTlmHybridRx&&((r->legacyLength!=34&&r->legacyLength!=74&&r->legacyLength!=84)||
    r->legacyRx>UINT64_C(0xffffffffff)||r->ratePolicy>1||
    (r->legacySlotMs!=500&&r->legacySlotMs!=50&&r->legacySlotMs!=10)))return false;
 if(r->type==mulTlmTdoa) {
  const mulTdoaTelemetry_t *t=&r->tdoa;
  bool available=t->status==4;
  if(r->radio!=2||r->role!=2||(r->mode!=4&&r->mode!=5)||r->ratePolicy!=(r->mode==5)||
    t->reference==t->follower||t->reference==255||t->follower==255||r->node==t->reference||r->node==t->follower||
    t->rx>UINT64_C(0xffffffffff)||t->status>4||t->clockValid>1||t->rateState>2||t->delayState>4||
    t->powerFlags>7||t->unknownComponents>63||
    (r->legacySlotMs!=500&&r->legacySlotMs!=50&&r->legacySlotMs!=10)||
    ((t->rateState==1)!=(t->ratePpb!=INT32_MIN))||
    (t->rateState&&(!r->ratePolicy||r->mode!=5))||
    (available&&(!t->clockValid||t->rateState!=1))||
    (available?(t->tdoaDtu==INT32_MIN||t->rangeDifferenceMm==INT32_MIN):
      (t->tdoaDtu!=INT32_MIN||t->rangeDifferenceMm!=INT32_MIN)))return false;
 }
 if(r->type==mulTlmVector||r->type==mulTlmMultiRx) {
  if(r->radio!=2||r->role!=2||(r->mode!=4&&r->mode!=5)||r->ratePolicy!=(r->mode==5))return false;
 }
 if(r->type==mulTlmMultiRx&&((r->legacyLength!=40&&r->legacyLength!=80&&r->legacyLength!=90)||
    r->legacyRx>UINT64_C(0xffffffffff)||!r->scheduleRevision||
    (r->legacySlotMs!=10&&r->legacySlotMs!=50&&r->legacySlotMs!=500)))return false;
 if(r->type==mulTlmVector) {
  const mulMultiCycle_t *v=&r->vector;
  if(v->count<2||v->count>8||!v->revision||v->periodMs!=1000||
    (v->slotMs!=10&&v->slotMs!=50&&v->slotMs!=500)||(v->count-1)*v->slotMs+20>630||
    v->referenceRx>UINT64_C(0xffffffffff)||v->seenMask>>v->count||v->validMask>>v->count||
    (v->validMask&1)||v->validMask&~v->seenMask)return false;
  for(unsigned i=0;i<8;i++) {
   if(i>=v->count){if(v->ids[i])return false;continue;}
   if(v->ids[i]==255||v->ids[i]==r->node)return false;
   for(unsigned j=0;j<i;j++)if(v->ids[i]==v->ids[j])return false;
   if(!i)continue;
   const mulMultiValue_t *x=&v->values[i-1];bool available=x->status==4;
   if(x->status>4||x->reason>9||x->clockValid>1||x->rateState>2||
     (!r->ratePolicy&&x->rateState)||available!=((v->validMask&(1u<<i))!=0)||
     (available&&(!x->clockValid||x->rateState!=1||x->reason||!(v->seenMask&1)))||
     (available?(x->rangeDifferenceMm==INT32_MIN||x->tdoaDtu==INT32_MIN):
       (x->rangeDifferenceMm!=INT32_MIN||x->tdoaDtu!=INT32_MIN||!x->reason)))return false;
  }
 }
 for(unsigned i=0;i<6;i++)if(r->stamps[i]>UINT64_C(0xffffffffff))return false;
 return true;
}
bool mulTelemetryEncode(char *line,size_t capacity,const mulTelemetry_t *r){
 if(!line||capacity<MUL_TLM_LINE||!valid(r))return false;
 uint8_t b[152]={0x4d,0x54,2,r->type,r->node,r->peer,r->radio,r->mode,r->role,r->flags,r->remoteRadio,r->quality};
 put(b+12,r->sequence,4);put(b+16,r->epoch,8);put(b+24,r->exchange,8);put(b+32,r->transaction,4);put(b+36,r->uptimeMs,4);
 put(b+40,(uint32_t)r->rawMm,4);put(b+44,(uint32_t)r->calMm,4);
 uint32_t v[]={r->attempts,r->completed,r->aborted,r->rejected,r->invalid};for(unsigned i=0;i<5;i++)put(b+48+4*i,v[i],4);
 put(b+68,r->depth,2);put(b+70,r->highWater,2);
 uint32_t w[]={r->dropped,r->uartErrors,r->spiCycles,r->serviceCycles,r->gapCycles,r->clockHz};for(unsigned i=0;i<6;i++)put(b+72+4*i,w[i],4);
 for(unsigned i=0;i<6;i++)put(b+96+5*i,r->stamps[i],5);
 put(b+126,r->radioErrors,4);put(b+130,r->timeouts,4);put(b+134,r->txDropped,4);put(b+138,r->modelVersion,4);put(b+142,r->remoteEpoch,8);
 if(r->mode>=4||r->type==mulTlmHybridRx)b[2]=3;
 if(r->type==mulTlmHybridRx) {
   memset(b+40,0,110);memcpy(b+40,r->legacyFrame,r->legacyLength);put(b+124,r->legacyRx,5);b[129]=r->legacyLength;
   put(b+130,r->captureAgeMs,2);put(b+132,r->referenceGeneration,4);put(b+136,r->followerGeneration,4);
   b[140]=r->ratePolicy;put(b+141,r->legacySlotMs,2);put(b+143,r->radioErrors,4);
 }
 if(r->type==mulTlmTdoa) {
  const mulTdoaTelemetry_t *t=&r->tdoa;memset(b+40,0,110);b[2]=3;
  put(b+40,t->referenceSequence,4);put(b+44,t->followerSequence,4);
  put(b+48,(uint32_t)t->tdoaDtu,4);put(b+52,(uint32_t)t->rangeDifferenceMm,4);put(b+56,(uint32_t)t->ratePpb,4);
  b[60]=t->reference;b[61]=t->follower;b[62]=t->status;b[63]=t->clockValid;b[64]=t->rateState;b[65]=r->ratePolicy;put(b+66,t->rx,5);
  put(b+72,r->dropped,4);put(b+76,r->uartErrors,4);put(b+80,r->referenceGeneration,4);put(b+84,r->followerGeneration,4);
  put(b+88,r->depth,2);put(b+90,r->highWater,2);put(b+92,r->captureAgeMs,2);put(b+94,r->legacySlotMs,2);
  put(b+104,t->delayAgeMs,4);put(b+108,t->delayDtu,2);put(b+110,t->clockAgeMs,2);
  b[112]=t->delayState;b[113]=t->unknownComponents;b[114]=t->powerFlags;
  put(b+118,r->radioErrors,4);put(b+122,r->timeouts,4);put(b+126,r->txDropped,4);
 }
 if(r->type==mulTlmVector) {
  const mulMultiCycle_t *v=&r->vector;memset(b+40,0,110);b[2]=3;
  put(b+40,v->cycle,4);put(b+44,v->fingerprint,4);put(b+48,v->revision,4);put(b+52,v->referenceRx,5);
  put(b+57,v->slotMs,2);put(b+59,v->periodMs,2);b[61]=v->count;b[62]=v->seenMask;b[63]=v->validMask;
  memcpy(b+64,v->ids,8);
  for(unsigned i=0;i+1<v->count;i++) {
   const mulMultiValue_t *x=&v->values[i];uint8_t *entry=b+72+10*i;
   put(entry,(uint32_t)x->rangeDifferenceMm,4);put(entry+4,(uint32_t)x->tdoaDtu,4);
   entry[8]=x->status|(x->clockValid<<3)|(x->rateState<<4);entry[9]=x->reason;
  }
  put(b+142,r->dropped,4);b[146]=r->depth>255?255:r->depth;b[147]=r->highWater>255?255:r->highWater;b[148]=r->ratePolicy;
 }
 if(r->type==mulTlmMultiRx) {
  memset(b+40,0,110);b[2]=3;memcpy(b+40,r->legacyFrame,r->legacyLength);
  put(b+130,r->legacyRx,5);put(b+135,r->captureAgeMs,2);b[137]=r->legacyLength;
  put(b+138,r->legacySlotMs,2);put(b+140,r->scheduleFingerprint,4);put(b+144,r->scheduleRevision,4);b[148]=r->ratePolicy;
 }
 put(b+150,mulTelemetryCrc(b,150),2);memcpy(line,b[2]==3?"UWB3 ":"UWB2 ",5);
 static const char hex[]="0123456789ABCDEF";for(unsigned i=0;i<152;i++){line[5+2*i]=hex[b[i]>>4];line[6+2*i]=hex[b[i]&15];}line[309]=0;return true;
}
static int hex(char c){return c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:c>='a'&&c<='f'?c-'a'+10:-1;}
static int32_t signed32(uint32_t v){return v<=INT32_MAX?(int32_t)v:-1-(int32_t)(UINT32_MAX-v);}
bool mulTelemetryDecode(const char *line,mulTelemetry_t *r){
 if(!line||!r||(strncmp(line,"UWB1 ",5)&&strncmp(line,"UWB2 ",5)&&strncmp(line,"UWB3 ",5)))return false;
 unsigned size=line[3]=='1'?128:152;uint8_t b[152]={0};for(unsigned i=0;i<size;i++){int a=hex(line[5+2*i]);if(a<0)return false;int c=hex(line[6+2*i]);if(c<0)return false;b[i]=(uint8_t)(a*16+c);}
 if(line[5+2*size]||b[0]!=0x4d||b[1]!=0x54||b[2]!=(unsigned)(line[3]-'0')||get(b+size-2,2)!=mulTelemetryCrc(b,size-2))return false;
 mulTelemetry_t d={.type=b[3],.node=b[4],.peer=b[5],.radio=b[6],.mode=b[7],.role=b[8],.flags=b[9],.remoteRadio=b[10],.quality=b[11]};
 d.sequence=(uint32_t)get(b+12,4);d.epoch=get(b+16,8);d.exchange=get(b+24,8);d.transaction=(uint32_t)get(b+32,4);d.uptimeMs=(uint32_t)get(b+36,4);
 if(d.type==mulTlmVector) {
  if(b[2]!=3||b[149]||b[61]<2||b[61]>8)return false;
  mulMultiCycle_t *v=&d.vector;
  v->cycle=(uint32_t)get(b+40,4);v->fingerprint=(uint32_t)get(b+44,4);v->revision=(uint32_t)get(b+48,4);
  v->observedMs=d.uptimeMs;v->referenceRx=get(b+52,5);v->slotMs=(uint16_t)get(b+57,2);v->periodMs=(uint16_t)get(b+59,2);
  v->count=b[61];v->seenMask=b[62];v->validMask=b[63];memcpy(v->ids,b+64,8);
  for(unsigned i=0;i+1<v->count;i++) {
   const uint8_t *entry=b+72+10*i;if(entry[8]&0xc0)return false;
   mulMultiValue_t *x=&v->values[i];x->rangeDifferenceMm=signed32((uint32_t)get(entry,4));x->tdoaDtu=signed32((uint32_t)get(entry+4,4));
   x->status=entry[8]&7;x->clockValid=(entry[8]>>3)&1;x->rateState=(entry[8]>>4)&3;x->reason=entry[9];
  }
  for(unsigned i=72+10*(v->count-1);i<142;i++)if(b[i])return false;
  d.dropped=(uint32_t)get(b+142,4);d.depth=b[146];d.highWater=b[147];d.ratePolicy=b[148];
  if(!valid(&d))return false;
  *r=d;return true;
 }
 if(d.type==mulTlmMultiRx) {
  if(b[2]!=3||b[149]||b[137]>90)return false;
  d.legacyLength=b[137];memcpy(d.legacyFrame,b+40,d.legacyLength);d.legacyRx=get(b+130,5);
  d.captureAgeMs=(uint16_t)get(b+135,2);d.legacySlotMs=(uint16_t)get(b+138,2);
  d.scheduleFingerprint=(uint32_t)get(b+140,4);d.scheduleRevision=(uint32_t)get(b+144,4);d.ratePolicy=b[148];
  for(unsigned i=d.legacyLength;i<90;i++)if(b[40+i])return false;
  if(!valid(&d))return false;
  *r=d;return true;
 }
 if(d.type==mulTlmTdoa) {
  if(b[2]!=3||b[71])return false;
  for(unsigned i=96;i<104;i++)if(b[i])return false;
  for(unsigned i=115;i<118;i++)if(b[i])return false;
  for(unsigned i=130;i<150;i++)if(b[i])return false;
  mulTdoaTelemetry_t *t=&d.tdoa;
  t->referenceSequence=(uint32_t)get(b+40,4);t->followerSequence=(uint32_t)get(b+44,4);
  t->tdoaDtu=signed32((uint32_t)get(b+48,4));t->rangeDifferenceMm=signed32((uint32_t)get(b+52,4));t->ratePpb=signed32((uint32_t)get(b+56,4));
  t->reference=b[60];t->follower=b[61];t->status=b[62];t->clockValid=b[63];t->rateState=b[64];d.ratePolicy=b[65];t->rx=get(b+66,5);
  d.dropped=(uint32_t)get(b+72,4);d.uartErrors=(uint32_t)get(b+76,4);d.referenceGeneration=(uint32_t)get(b+80,4);d.followerGeneration=(uint32_t)get(b+84,4);
  d.depth=(uint16_t)get(b+88,2);d.highWater=(uint16_t)get(b+90,2);d.captureAgeMs=(uint16_t)get(b+92,2);d.legacySlotMs=(uint16_t)get(b+94,2);
  t->delayAgeMs=(uint32_t)get(b+104,4);t->delayDtu=(uint16_t)get(b+108,2);t->clockAgeMs=(uint16_t)get(b+110,2);
  t->delayState=b[112];t->unknownComponents=b[113];t->powerFlags=b[114];
  d.radioErrors=(uint32_t)get(b+118,4);d.timeouts=(uint32_t)get(b+122,4);d.txDropped=(uint32_t)get(b+126,4);
  if(!valid(&d))return false;
  *r=d;return true;
 }
 if(d.type==mulTlmHybridRx) {
   if(b[2]!=3||b[147]||b[148]||b[149]||b[129]>84)return false;
   d.legacyLength=b[129];memcpy(d.legacyFrame,b+40,d.legacyLength);d.legacyRx=get(b+124,5);
   d.captureAgeMs=(uint16_t)get(b+130,2);d.referenceGeneration=(uint32_t)get(b+132,4);d.followerGeneration=(uint32_t)get(b+136,4);
   d.ratePolicy=b[140];d.legacySlotMs=(uint16_t)get(b+141,2);d.radioErrors=(uint32_t)get(b+143,4);
   for(unsigned i=d.legacyLength;i<84;i++)if(b[40+i])return false;
   if(!valid(&d))return false;
   *r=d;return true;
 }
 if((b[2]<3&&d.mode>3)||d.type>5)return false;
 d.rawMm=signed32((uint32_t)get(b+40,4));d.calMm=signed32((uint32_t)get(b+44,4));
 uint32_t *v[]={&d.attempts,&d.completed,&d.aborted,&d.rejected,&d.invalid};for(unsigned i=0;i<5;i++)*v[i]=(uint32_t)get(b+48+4*i,4);
 d.depth=(uint16_t)get(b+68,2);d.highWater=(uint16_t)get(b+70,2);
 uint32_t *w[]={&d.dropped,&d.uartErrors,&d.spiCycles,&d.serviceCycles,&d.gapCycles,&d.clockHz};for(unsigned i=0;i<6;i++)*w[i]=(uint32_t)get(b+72+4*i,4);
 for(unsigned i=0;i<6;i++)d.stamps[i]=get(b+96+5*i,5);
 if(size==152){d.radioErrors=(uint32_t)get(b+126,4);d.timeouts=(uint32_t)get(b+130,4);d.txDropped=(uint32_t)get(b+134,4);d.modelVersion=(uint32_t)get(b+138,4);d.remoteEpoch=get(b+142,8);}
 if(!valid(&d))return false;
 *r=d;return true;
}
