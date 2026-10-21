/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "golden.h"
static void unhex(uint8_t *out,const char *hex,size_t n) {
  for (size_t i=0;i<n;i++) { unsigned v; assert(sscanf(hex+2*i,"%2x",&v)==1); out[i]=(uint8_t)v; }
}
int main(void) {
  uint8_t f[128],expected[44],saved[44];
  mulCoreDiag_t m={.source={0x84,0,0,0,0,0,0xcf,0xbc},.radio=MUL_CORE_DW1000},decoded;
  for (unsigned i=0;i<3;i++) {
    m.source[0]=i==1 ? 0x86 : 0x84; m.radio=i==1 ? MUL_CORE_DW3000 : MUL_CORE_DW1000;
    m.sequence=i==0 ? 0 : i==1 ? UINT32_C(0x12345678) : 0;
    m.markerValid=i!=0; m.markerSequence=i==0 ? 0 : m.sequence-1;
    m.txMarker=i==0 ? 0 : i==1 ? UINT64_C(0x123456789a) : MUL_TIMESTAMP_MASK;
    assert(mulCoreEncodeDiag(f,sizeof(f),&m)==mulCoreOk); unhex(expected,golden[i],44);
    assert(!memcmp(f,expected,44)); assert(mulCoreDecodeDiag(f,44,&decoded)==mulCoreOk);
    assert(decoded.sequence==m.sequence && decoded.txMarker==m.txMarker && decoded.markerSequence==m.markerSequence);
    for (size_t n=0;n<sizeof(f);n++) if (n!=44) assert(mulCoreDecodeDiag(f,n,&decoded)==mulCoreLength);
    memcpy(saved,f,44);
    const unsigned strict[]={0,1,2,3,4,5,12,13,14,20,21,22,23,24,25,30,31,32,33,43};
    for (unsigned k=0;k<sizeof(strict)/sizeof(strict[0]);k++) {
      f[strict[k]]^=0x80; decoded.sequence=42;
      assert(mulCoreDecodeDiag(f,44,&decoded)!=mulCoreOk); assert(decoded.sequence==42);
      memcpy(f,saved,44);
    }
  }
  assert(mulCoreEncodeDiag(NULL,44,&m)==mulCoreArgument);
  assert(mulCoreEncodeDiag(f,43,&m)==mulCoreLength);
  assert(mulCoreEncodeDiag(f,44,NULL)==mulCoreArgument);
  assert(mulCoreDecodeDiag(NULL,44,&m)==mulCoreArgument);
  assert(mulCoreDecodeDiag(f,44,NULL)==mulCoreArgument);
  m.source[1]=1; assert(mulCoreEncodeDiag(f,44,&m)==mulCoreSource); m.source[1]=0;
  m.markerSequence=m.sequence; assert(mulCoreEncodeDiag(f,44,&m)==mulCoreFormat);
  m.markerSequence=m.sequence-1; m.txMarker=UINT64_C(1)<<40;
  assert(mulCoreEncodeDiag(f,44,&m)==mulCoreFormat); m.txMarker=0; m.markerValid=false;
  assert(mulCoreEncodeDiag(f,44,&m)==mulCoreFormat); m.markerSequence=0;
  assert(mulCoreEncodeDiag(f,44,&m)==mulCoreOk);
  mulCoreObservation_t observation; const uint8_t receiver[8]={0x86,0,0,0,0,0,0xcf,0xbc};
  assert(mulCoreObserve(&observation,&m,receiver,2,MUL_TIMESTAMP_MASK)==mulCoreOk);
  assert(observation.rawRxMarker==MUL_TIMESTAMP_MASK && observation.message.sequence==m.sequence);
  assert(mulCoreObserve(NULL,&m,receiver,2,1)==mulCoreArgument);
  assert(mulCoreObserve(&observation,&m,m.source,1,1)==mulCoreFormat);
  assert(mulCoreObserve(&observation,&m,receiver,2,UINT64_C(1)<<40)==mulCoreFormat);
  assert(mulTimestampDifference(0,MUL_TIMESTAMP_MASK)==1);
  assert(mulTimestampDifference(MUL_TIMESTAMP_MASK,0)==-1);
  assert(mulTimestampDifference(UINT64_C(1)<<39,0)==-(INT64_C(1)<<39));
  uint8_t timestamp[5]; mulEncodeTimestamp(timestamp,MUL_TIMESTAMP_MASK); assert(mulDecodeTimestamp(timestamp)==MUL_TIMESTAMP_MASK);
  mulCoreSequence_t s; mulCoreSequenceReset(&s);
  assert(mulCoreObserveSequence(&s,UINT32_MAX)==mulCoreFirst);
  assert(mulCoreObserveSequence(&s,0)==mulCoreForward);
  assert(mulCoreObserveSequence(&s,2)==mulCoreGap);
  assert(mulCoreObserveSequence(&s,2)==mulCoreDuplicate);
  assert(mulCoreObserveSequence(&s,1)==mulCoreOld);
  assert(s.valid==5 && s.gaps==1 && s.duplicates==1 && s.old==1 && s.last==2);
  mulCoreSequenceReset(&s); assert(!s.baseline && !s.valid);
  /* Fast restart is ambiguous in v1; explicit epoch reset is caller-owned. */
  mulCoreObserveSequence(&s,2994); assert(mulCoreObserveSequence(&s,0)==mulCoreOld);
  mulCoreSequenceBeginEpoch(&s); assert(mulCoreObserveSequence(&s,1)==mulCoreFirst);
  assert(mulCoreObserveSequence(&s,3)==mulCoreGap);
  assert(s.valid==4 && s.old==1 && s.gaps==1 && s.valid-s.old-s.duplicates==3);
  mulCoreSequence_t other={0}; /* Separate source/session, never share baseline. */
  assert(mulCoreObserveSequence(&other,900)==mulCoreFirst);
  assert(mulCoreObserveSequence(&s,4)==mulCoreForward && other.last==900);
  uint32_t random=1;
  for (unsigned i=0;i<20000;i++) {
    for (unsigned k=0;k<sizeof(f);k++) { random=random*1664525u+1013904223u; f[k]=(uint8_t)(random>>24); }
    assert(mulCoreDecodeDiag(f,random%sizeof(f),&decoded)!=mulCoreOk);
  }
  /* Legacy codec remains wire compatible and accepts the same source contract. */
  uint8_t legacy[34]; mulTestPacket_t old;
  mulEncodeTestPacket(legacy,m.source,UINT32_MAX,MUL_TIMESTAMP_MASK);
  assert(mulDecodeTestPacket(legacy,34,&old) && old.sequence==UINT32_MAX && old.txTimestamp==MUL_TIMESTAMP_MASK);
  unhex(expected,"41dcffcfbcffffffffffffffff840000000000cfbc4d554c01ffffffffffffffffff",34);
  assert(!memcmp(legacy,expected,34));
  puts("MUL core: 3 golden vectors, malformed/null/bounds/source/version/type, serial/40-bit wrap, reset, observation and 20000 fuzz inputs passed");
}
