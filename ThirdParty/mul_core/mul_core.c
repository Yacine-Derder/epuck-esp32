/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_core.h"
#include <string.h>
_Static_assert(MUL_CORE_DIAG_LENGTH == 44, "diagnostic v1 wire size");
_Static_assert(MUL_TIMESTAMP_BYTES == 5, "full 40-bit marker required");
static bool radioValid(uint8_t radio) { return radio==MUL_CORE_DW1000 || radio==MUL_CORE_DW3000; }
bool mulCoreSourceValid(const uint8_t s[8]) {
  if (!s || (s[0]!=0x84 && s[0]!=0x85 && s[0]!=0x86) || s[6]!=0xcf || s[7]!=0xbc) return false;
  for (unsigned i=1;i<6;i++) if (s[i]) return false;
  return true;
}
static mulCoreStatus_t validate(const mulCoreDiag_t *m) {
  if (!m) return mulCoreArgument;
  if (!mulCoreSourceValid(m->source)) return mulCoreSource;
  if (!radioValid(m->radio) || m->txMarker>MUL_TIMESTAMP_MASK) return mulCoreFormat;
  if (!m->markerValid && (m->markerSequence || m->txMarker)) return mulCoreFormat;
  uint32_t advance=m->sequence-m->markerSequence;
  if (m->markerValid && (!advance || advance>=UINT32_C(0x80000000))) return mulCoreFormat;
  return mulCoreOk;
}
static void put32(uint8_t *p,uint32_t v) { for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
static uint32_t get32(const uint8_t *p) {
  uint32_t v=0; for (unsigned i=0;i<4;i++) v|=(uint32_t)p[i]<<(8*i); return v;
}
mulCoreStatus_t mulCoreEncodeDiag(uint8_t *f,size_t capacity,const mulCoreDiag_t *m) {
  if (!f || !m) return mulCoreArgument;
  if (capacity<MUL_CORE_DIAG_LENGTH) return mulCoreLength;
  mulCoreStatus_t status=validate(m); if (status!=mulCoreOk) return status;
  mulEncodeTestPacket(f,m->source,m->sequence,0); /* unchanged extended MAC header */
  f[24]=MUL_CORE_DIAG_TYPE; f[25]=MUL_CORE_DIAG_VERSION;
  put32(f+26,m->sequence); f[30]=m->markerValid ? 1 : 0;
  f[31]=MUL_CORE_WIDTH; f[32]=MUL_CORE_DTU; f[33]=m->radio;
  put32(f+34,m->markerSequence); mulEncodeTimestamp(f+38,m->txMarker); f[43]=0xa5;
  return mulCoreOk;
}
mulCoreStatus_t mulCoreDecodeDiag(const uint8_t *f,size_t length,mulCoreDiag_t *m) {
  if (!f || !m) return mulCoreArgument;
  if (length!=MUL_CORE_DIAG_LENGTH) return mulCoreLength;
  if (f[0]!=0x41 || f[1]!=0xdc || f[3]!=0xcf || f[4]!=0xbc ||
      memcmp(f+21,"MUL",3) || f[24]!=MUL_CORE_DIAG_TYPE || f[25]!=MUL_CORE_DIAG_VERSION ||
      f[2]!=f[26] || f[30]>1 || f[31]!=MUL_CORE_WIDTH || f[32]!=MUL_CORE_DTU || f[43]!=0xa5) return mulCoreFormat;
  for (unsigned i=5;i<13;i++) if (f[i]!=0xff) return mulCoreFormat;
  mulCoreDiag_t decoded={0};
  memcpy(decoded.source,f+13,8); decoded.sequence=get32(f+26); decoded.markerValid=f[30]!=0;
  decoded.radio=f[33]; decoded.markerSequence=get32(f+34); decoded.txMarker=mulDecodeTimestamp(f+38);
  mulCoreStatus_t status=validate(&decoded); if (status==mulCoreOk) *m=decoded;
  return status;
}
void mulCoreSequenceBeginEpoch(mulCoreSequence_t *s) { if (s) s->baseline=false; }
void mulCoreSequenceReset(mulCoreSequence_t *s) { if (s) memset(s,0,sizeof(*s)); }
mulCoreOrder_t mulCoreObserveSequence(mulCoreSequence_t *s,uint32_t seq) {
  if (!s) return mulCoreOld;
  s->valid++;
  if (!s->baseline) { s->baseline=true; s->last=seq; return mulCoreFirst; }
  uint32_t advance=seq-s->last;
  if (!advance) { s->duplicates++; return mulCoreDuplicate; }
  if (advance>=UINT32_C(0x80000000)) { s->old++; return mulCoreOld; }
  s->gaps+=advance-1; s->last=seq; return advance==1 ? mulCoreForward : mulCoreGap;
}
mulCoreStatus_t mulCoreObserve(mulCoreObservation_t *out,const mulCoreDiag_t *m,
                              const uint8_t receiver[8],uint8_t radio,uint64_t rawRx) {
  if (!out || !receiver || !m) return mulCoreArgument;
  mulCoreStatus_t status=validate(m); if (status!=mulCoreOk) return status;
  if (!mulCoreSourceValid(receiver)) return mulCoreSource;
  if (!radioValid(radio) || rawRx>MUL_TIMESTAMP_MASK || !memcmp(receiver,m->source,8)) return mulCoreFormat;
  out->message=*m; memcpy(out->receiver,receiver,8); out->receiverRadio=radio; out->rawRxMarker=rawRx;
  return mulCoreOk;
}
