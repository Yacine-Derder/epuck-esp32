/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_ANTENNA_OFFSET_H
#define MUL_ANTENNA_OFFSET_H
#include <stdbool.h>
#include <stdint.h>
/* Nearest integer; exact ties away from zero. Internal products cannot reach
 * INT64_MIN and divisors are positive. No floating point or persistent state. */
static inline int64_t mulAntennaOffsetRound(int64_t n,int64_t d) {
 return n<0?-((-n+d/2)/d):(n+d/2)/d;
}
static inline bool mulAntennaOffsetApply(int32_t mm,uint16_t baseTx,uint16_t baseRx,
                                       uint16_t *tx,uint16_t *rx) {
 int64_t aggregate=mulAntennaOffsetRound((int64_t)mm*127795200,299792458);
 int64_t a=mulAntennaOffsetRound(aggregate*44,100),b=aggregate-a;
 a+=baseTx;b+=baseRx;
 if(a<0||a>65535||b<0||b>65535)return false;
 *tx=(uint16_t)a;*rx=(uint16_t)b;return true;
}
static inline bool mulAntennaOffsetParse(const char *s,uint16_t baseTx,uint16_t baseRx,
                                       uint16_t *tx,uint16_t *rx) {
 bool negative=*s=='-';if(*s=='-'||*s=='+')s++;
 if(!*s)return false;
 uint32_t n=0,limit=negative?UINT32_C(2147483648):INT32_MAX;
 for(;*s;s++) {
  if(*s<'0'||*s>'9')return false;
  unsigned digit=(unsigned)(*s-'0');
  if(n>limit/10||(n==limit/10&&digit>limit%10))return false;
  n=n*10+digit;
 }
 int32_t mm=negative?(n==UINT32_C(2147483648)?INT32_MIN:-(int32_t)n):(int32_t)n;
 return mulAntennaOffsetApply(mm,baseTx,baseRx,tx,rx);
}
/* This is the physical equivalent of the quantized registers, not the original
 * integer-mm request: typically2..3 requests encode the same aggregate DTU.
 * False means even the assumed44/56 split does not match (manual/custom pair). */
static inline bool mulAntennaOffsetEquivalentUm(uint16_t baseTx,uint16_t baseRx,
                                              uint16_t tx,uint16_t rx,int32_t *um) {
 int32_t aggregate=(int32_t)tx+rx-baseTx-baseRx;
 if((int32_t)tx-baseTx!=mulAntennaOffsetRound((int64_t)aggregate*44,100))return false;
 *um=(int32_t)mulAntennaOffsetRound((int64_t)aggregate*299792458000LL,127795200);
 return true;
}
#endif
