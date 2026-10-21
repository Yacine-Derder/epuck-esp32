/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_ads.h"
#include <math.h>
#include <string.h>
/* ADS IDs occupy one octet of an extended MAC, including 00 and FF.
 * Preserve legacy diagnostic source policy; validate ADS suffix independently. */
static bool address(const uint8_t *v) {
  if(v[6]!=0xcf || v[7]!=0xbc) return false;
  for(unsigned i=1;i<6;i++) if(v[i]) return false;
  return true;
}
static void put(uint8_t *p,uint64_t v,unsigned n) { for(unsigned i=0;i<n;i++) p[i]=(uint8_t)(v>>(8*i)); }
static uint64_t get(const uint8_t *p,unsigned n) { uint64_t v=0; for(unsigned i=0;i<n;i++) v|=(uint64_t)p[i]<<(8*i); return v; }
static bool valid(const mulAdsPacket_t *p) {
  if (!p || p->source==p->destination ||
      !p->session || !p->transaction || p->type<mulAdsPoll || p->type>mulAdsReport) return false;
  for(unsigned i=0;i<3;i++) if(p->stamps[i]>MUL_TIMESTAMP_MASK || (p->type!=mulAdsReport && p->stamps[i])) return false;
  return true;
}
bool mulAdsEncode(uint8_t *f,size_t n,const mulAdsPacket_t *p) {
  if(!f || n<MUL_ADS_LENGTH || !valid(p)) return false;
  uint8_t source[8]={p->source,0,0,0,0,0,0xcf,0xbc};
  mulEncodeTestPacket(f,source,p->transaction,0);
  memcpy(f+5,source,8); f[5]=p->destination;
  f[24]=p->type; f[25]=MUL_ADS_VERSION; put(f+26,p->session,8); put(f+34,p->transaction,4);
  for(unsigned i=0;i<3;i++) mulEncodeTimestamp(f+38+5*i,p->stamps[i]);
  f[53]=40; f[54]=MUL_CORE_DTU; f[55]=0xa5; return true;
}
bool mulAdsDecode(const uint8_t *f,size_t n,mulAdsPacket_t *p) {
  if(!f || !p || n!=MUL_ADS_LENGTH || f[0]!=0x41 || f[1]!=0xdc || f[2]!=f[34] ||
     f[3]!=0xcf || f[4]!=0xbc || memcmp(f+21,"MUL",3) || f[25]!=MUL_ADS_VERSION ||
     f[53]!=40 || f[54]!=MUL_CORE_DTU || f[55]!=0xa5 ||
     !address(f+5) || !address(f+13)) return false;
  mulAdsPacket_t d={.source=f[13],.destination=f[5],.type=f[24],.session=get(f+26,8),.transaction=(uint32_t)get(f+34,4)};
  for(unsigned i=0;i<3;i++) d.stamps[i]=mulDecodeTimestamp(f+38+5*i);
  if(!valid(&d)) return false;
  *p=d; return true;
}
bool mulAdsCalculate(const uint64_t t[6],double *tof,double *ratio) {
  if(!t || !tof || !ratio) return false;
  for(unsigned i=0;i<6;i++) if(t[i]>MUL_TIMESTAMP_MASK) return false;
  int64_t ra=mulTimestampDifference(t[3],t[0]), rb=mulTimestampDifference(t[5],t[2]);
  int64_t da=mulTimestampDifference(t[4],t[3]), db=mulTimestampDifference(t[2],t[1]);
  if(ra<=0 || rb<=0 || da<=0 || db<=0 || (uint64_t)ra>MUL_ADS_MAX_INTERVAL ||
     (uint64_t)rb>MUL_ADS_MAX_INTERVAL || (uint64_t)da>MUL_ADS_MAX_INTERVAL || (uint64_t)db>MUL_ADS_MAX_INTERVAL) return false;
  double a=(double)ra,b=(double)rb,c=(double)da,d=(double)db;
  double r=(b+d)/(a+c);
  if(r<0.999 || r>1.001) return false; /* Explicit +/-1000ppm clock consistency envelope. */
  /* Convert BEFORE multiplication: maximal products exceed uint64. At this
   * bounded interval, binary64 cancellation error is well below 0.00001 DTU. */
  double value=(a*b-c*d)/(a+b+c+d);
  if(!isfinite(value)) return false;
  *tof=value; *ratio=r; return true;
}
bool mulAdsInit(mulAds_t *s,uint8_t local,uint8_t peer,bool initiator,uint64_t session) {
  if(!s || local==peer || !session) return false;
  memset(s,0,sizeof(*s)); s->local=local; s->peer=peer; s->initiator=initiator; s->session=session; return true;
}
bool mulAdsRangeValid(double tof) { return isfinite(tof) && tof>=0; }
void mulAdsAbortWithReason(mulAds_t *s,mulAdsAbortReason_t reason) {
  if(s && s->stage!=adsIdle) {
    s->lastAbortStage=s->stage; s->lastAbortReason=(uint8_t)reason;
    if(s->stage<=adsReportTx && s->failedStages[s->stage-1]<UINT16_MAX) s->failedStages[s->stage-1]++;
    s->aborted++; s->stage=adsIdle; s->pending=false; s->rangeValid=false;
  }
}
void mulAdsAbort(mulAds_t *s) { mulAdsAbortWithReason(s,adsAbortCancelled); }
bool mulAdsTick(mulAds_t *s,uint32_t now) {
  if(s && s->stage!=adsIdle && (uint32_t)(now-s->changedMs)>=MUL_ADS_TIMEOUT_MS) { mulAdsAbortWithReason(s,adsAbortDeadline); return true; }
  return false;
}
bool mulAdsBegin(mulAds_t *s,uint32_t now) {
  if(!s || !s->initiator || s->stage!=adsIdle || s->transaction==UINT32_MAX) return false;
  s->transaction++; s->attempts++; s->changedMs=now; s->stage=adsPollTx; s->pending=false; s->rangeValid=false;
  memset(s->stamps,0,sizeof(s->stamps)); return true;
}
bool mulAdsTakeTx(mulAds_t *s,uint32_t now,mulAdsPacket_t *p) {
  if(!s || !p || s->pending || mulAdsTick(s,now)) return false;
  uint8_t type=s->stage==adsPollTx ? mulAdsPoll : s->stage==adsResponseTx ? mulAdsResponse :
               s->stage==adsFinalTx ? mulAdsFinal : s->stage==adsReportTx ? mulAdsReport : 0;
  if(!type) return false;
  *p=(mulAdsPacket_t){.session=s->session,.transaction=s->transaction,.source=s->local,.destination=s->peer,.type=type};
  if(type==mulAdsReport) memcpy(p->stamps,s->stamps,sizeof(p->stamps));
  s->pending=true; return true;
}
bool mulAdsTxDone(mulAds_t *s,uint64_t marker,uint32_t now) {
  if(!s || !s->pending || marker>MUL_TIMESTAMP_MASK || mulAdsTick(s,now)) return false;
  switch(s->stage) {
    case adsPollTx: s->stamps[0]=marker; s->stage=adsResponseRx; break;
    case adsResponseTx: s->stamps[1]=marker; s->stage=adsFinalRx; break;
    case adsFinalTx: s->stamps[2]=marker; s->stage=adsReportRx; break;
    case adsReportTx: s->completed++; s->stage=adsIdle; break;
    default: return false;
  }
  s->pending=false; s->changedMs=now; return true;
}
bool mulAdsReceive(mulAds_t *s,const mulAdsPacket_t *p,uint64_t rx,uint32_t now) {
  if(!s || !p) return false;
  bool expired=mulAdsTick(s,now);
  if(expired || !valid(p) || rx>MUL_TIMESTAMP_MASK || p->source!=s->peer || p->destination!=s->local) goto reject;
  if(!s->initiator && s->stage==adsIdle && p->type==mulAdsPoll) {
    if(s->seen && p->session==s->session && (p->transaction-s->transaction==0 || p->transaction-s->transaction>=UINT32_C(0x80000000))) goto reject;
    s->seen=true; s->session=p->session; s->transaction=p->transaction; s->stamps[0]=rx;
    s->stage=adsResponseTx; s->attempts++;
  } else {
    if(p->session!=s->session || p->transaction!=s->transaction || s->pending) goto reject;
    if(s->initiator && s->stage==adsResponseRx && p->type==mulAdsResponse) { s->stamps[1]=rx; s->stage=adsFinalTx; }
    else if(!s->initiator && s->stage==adsFinalRx && p->type==mulAdsFinal) { s->stamps[2]=rx; s->stage=adsReportTx; }
    else if(s->initiator && s->stage==adsReportRx && p->type==mulAdsReport) {
      uint64_t t[6]={s->stamps[0],p->stamps[0],p->stamps[1],s->stamps[1],s->stamps[2],p->stamps[2]};
      if(!mulAdsCalculate(t,&s->tof,&s->rateRatio)) { mulAdsAbortWithReason(s,adsAbortMath); goto reject; }
      s->rangeValid=mulAdsRangeValid(s->tof);
      if(!s->rangeValid) s->invalidRanges++;
      s->completed++; s->stage=adsIdle;
    } else goto reject;
  }
  s->changedMs=now; return true;
reject: s->rejected++; return false;
}
