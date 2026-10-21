/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_beacon.h"
#include <string.h>
static void put(uint8_t *p,uint64_t v,unsigned n){for(unsigned i=0;i<n;i++)p[i]=(uint8_t)(v>>(8*i));}
static uint64_t get(const uint8_t *p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static bool valid(const mulBeacon_t *p){return p&&p->epoch&&p->radio>=1&&p->radio<=2&&p->previousTx<=MUL_TIMESTAMP_MASK&&
 (p->markerValid?(p->sequence-p->previousSequence>0&&p->sequence-p->previousSequence<UINT32_C(0x80000000)):(!p->previousSequence&&!p->previousTx));}
bool mulBeaconEncode(uint8_t *f,size_t capacity,const mulBeacon_t *p){
 if(!f||capacity<56||!valid(p))return false;
 uint8_t source[8]={p->source,0,0,0,0,0,0xcf,0xbc};mulEncodeTestPacket(f,source,p->sequence,0);
 f[24]=0x75;f[25]=1;f[26]=p->radio;f[27]=p->markerValid;put(f+28,p->epoch,8);put(f+36,p->sequence,4);put(f+40,p->previousSequence,4);put(f+44,p->previousTx,5);f[49]=40;f[50]=1;memset(f+51,0,5);return true;
}
bool mulBeaconDecode(const uint8_t *f,size_t length,mulBeacon_t *p){
 if(!f||!p||length!=56||f[0]!=0x41||f[1]!=0xdc||f[2]!=f[36]||f[3]!=0xcf||f[4]!=0xbc||memcmp(f+21,"MUL",3)||f[24]!=0x75||f[25]!=1||f[27]>1||f[49]!=40||f[50]!=1||f[19]!=0xcf||f[20]!=0xbc)return false;
 for(unsigned i=5;i<13;i++)if(f[i]!=255)return false;
 for(unsigned i=14;i<19;i++)if(f[i])return false;
 for(unsigned i=51;i<56;i++)if(f[i])return false;
 mulBeacon_t d={.source=f[13],.radio=f[26],.epoch=get(f+28,8),.sequence=(uint32_t)get(f+36,4),.previousSequence=(uint32_t)get(f+40,4),.previousTx=get(f+44,5),.markerValid=f[27]!=0};if(!valid(&d))return false;*p=d;return true;
}
