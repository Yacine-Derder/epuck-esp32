/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_telemetry.h"
#include <string.h>
static void put(uint8_t *p,uint64_t v,unsigned n){for(unsigned i=0;i<n;i++)p[i]=(uint8_t)(v>>(8*i));}
static uint64_t get(const uint8_t *p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
uint16_t mulTelemetryCrc(const uint8_t *p,size_t n){uint16_t c=0xffff;for(size_t i=0;i<n;i++){c^=(uint16_t)p[i]<<8;for(unsigned j=0;j<8;j++)c=(uint16_t)((c<<1)^((c&0x8000)?0x1021:0));}return c;}
static bool valid(const mulTelemetry_t *r){
 if(!r||r->type<1||r->type>5||r->radio<1||r->radio>2||!r->epoch||r->mode>3||r->role<1||r->role>2||r->flags>31||r->remoteRadio>2)return false;
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
 put(b+150,mulTelemetryCrc(b,150),2);memcpy(line,"UWB2 ",5);
 static const char hex[]="0123456789ABCDEF";for(unsigned i=0;i<152;i++){line[5+2*i]=hex[b[i]>>4];line[6+2*i]=hex[b[i]&15];}line[309]=0;return true;
}
static int hex(char c){return c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:c>='a'&&c<='f'?c-'a'+10:-1;}
static int32_t signed32(uint32_t v){return v<=INT32_MAX?(int32_t)v:-1-(int32_t)(UINT32_MAX-v);}
bool mulTelemetryDecode(const char *line,mulTelemetry_t *r){
 if(!line||!r||(strncmp(line,"UWB1 ",5)&&strncmp(line,"UWB2 ",5)))return false;
 unsigned size=line[3]=='1'?128:152;uint8_t b[152]={0};for(unsigned i=0;i<size;i++){int a=hex(line[5+2*i]);if(a<0)return false;int c=hex(line[6+2*i]);if(c<0)return false;b[i]=(uint8_t)(a*16+c);}
 if(line[5+2*size]||b[0]!=0x4d||b[1]!=0x54||b[2]!=(size==128?1:2)||get(b+size-2,2)!=mulTelemetryCrc(b,size-2))return false;
 mulTelemetry_t d={.type=b[3],.node=b[4],.peer=b[5],.radio=b[6],.mode=b[7],.role=b[8],.flags=b[9],.remoteRadio=b[10],.quality=b[11]};
 d.sequence=(uint32_t)get(b+12,4);d.epoch=get(b+16,8);d.exchange=get(b+24,8);d.transaction=(uint32_t)get(b+32,4);d.uptimeMs=(uint32_t)get(b+36,4);
 d.rawMm=signed32((uint32_t)get(b+40,4));d.calMm=signed32((uint32_t)get(b+44,4));
 uint32_t *v[]={&d.attempts,&d.completed,&d.aborted,&d.rejected,&d.invalid};for(unsigned i=0;i<5;i++)*v[i]=(uint32_t)get(b+48+4*i,4);
 d.depth=(uint16_t)get(b+68,2);d.highWater=(uint16_t)get(b+70,2);
 uint32_t *w[]={&d.dropped,&d.uartErrors,&d.spiCycles,&d.serviceCycles,&d.gapCycles,&d.clockHz};for(unsigned i=0;i<6;i++)*w[i]=(uint32_t)get(b+72+4*i,4);
 for(unsigned i=0;i<6;i++)d.stamps[i]=get(b+96+5*i,5);
 if(size==152){d.radioErrors=(uint32_t)get(b+126,4);d.timeouts=(uint32_t)get(b+130,4);d.txDropped=(uint32_t)get(b+134,4);d.modelVersion=(uint32_t)get(b+138,4);d.remoteEpoch=get(b+142,8);}
 if(!valid(&d))return false;
 *r=d;return true;
}
