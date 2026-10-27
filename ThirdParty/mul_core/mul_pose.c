/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_pose.h"
#include "mul_schedule.h"
#include <math.h>
#include <string.h>
static void put(uint8_t *p,uint64_t n,unsigned bytes){for(unsigned i=0;i<bytes;i++)p[i]=(uint8_t)(n>>(8*i));}
static uint64_t get(const uint8_t *p,unsigned bytes){uint64_t n=0;for(unsigned i=0;i<bytes;i++)n|=(uint64_t)p[i]<<(8*i);return n;}
static bool covariance(const int32_t *c) {
 if(c[0]<0||c[3]<0||c[5]<0)return false;
 for(unsigned i=0;i<6;i++)if(c[i]<-1000000000||c[i]>1000000000)return false;
 if((int64_t)c[1]*c[1]>(int64_t)c[0]*c[3]||
    (int64_t)c[2]*c[2]>(int64_t)c[0]*c[5]||
    (int64_t)c[4]*c[4]>(int64_t)c[3]*c[5])return false;
 double scale=c[0];if(c[3]>scale)scale=c[3];if(c[5]>scale)scale=c[5];
 if(!scale)return true;
 double a=c[0]/scale,b=c[1]/scale,d=c[2]/scale,e=c[3]/scale,f=c[4]/scale,g=c[5]/scale;
 return a*e*g+2*b*d*f-a*f*f-e*d*d-g*b*b>=-1e-12;
}
bool mulPoseValid(const mulPose_t *p) {
 if(!p||p->node==255||!p->clock||!p->frame||!p->ttlMs||p->ttlMs>MUL_CLOCK_DELAY_MAX_AGE_MS||
    (p->flags&~7u)||((p->flags&MUL_POSE_INDEPENDENT)&&!(p->flags&MUL_POSE_COVARIANCE)))return false;
 for(unsigned i=0;i<3;i++)if(p->positionMm[i]<-1000000||p->positionMm[i]>1000000)return false;
 if(p->flags&MUL_POSE_COVARIANCE)return covariance(p->covarianceMm2);
 for(unsigned i=0;i<6;i++)if(p->covarianceMm2[i])return false;
 return true;
}
bool mulPoseEncode(uint8_t out[MUL_POSE_BYTES],const mulPose_t *p) {
 if(!out||!mulPoseValid(p))return false;
 uint8_t b[MUL_POSE_BYTES]={1,p->node,p->flags,0};
 put(b+4,p->clock,4);put(b+8,p->epoch,4);put(b+12,p->frame,4);put(b+16,p->sequence,4);put(b+20,p->sourceUs,8);
 for(unsigned i=0;i<3;i++)put(b+28+4*i,(uint32_t)p->positionMm[i],4);
 for(unsigned i=0;i<6;i++)put(b+40+4*i,(uint32_t)p->covarianceMm2[i],4);
 put(b+64,p->ageUpperMs,2);put(b+66,p->ttlMs,2);put(b+68,p->timeUncertaintyMs,2);put(b+76,mulScheduleCrc(b,76),4);
 memcpy(out,b,sizeof(b));return true;
}
bool mulPoseDecode(const uint8_t in[MUL_POSE_BYTES],uint32_t receivedMs,mulPose_t *out) {
 if(!in||!out||in[0]!=1||in[3]||get(in+76,4)!=mulScheduleCrc(in,76))return false;
 for(unsigned i=70;i<76;i++)if(in[i])return false;
 mulPose_t p={.node=in[1],.flags=in[2],.clock=(uint32_t)get(in+4,4),.epoch=(uint32_t)get(in+8,4),
  .frame=(uint32_t)get(in+12,4),.sequence=(uint32_t)get(in+16,4),.sourceUs=get(in+20,8),
  .ageUpperMs=(uint16_t)get(in+64,2),.ttlMs=(uint16_t)get(in+66,2),.timeUncertaintyMs=(uint16_t)get(in+68,2),.receivedMs=receivedMs};
 for(unsigned i=0;i<3;i++)p.positionMm[i]=(int32_t)(uint32_t)get(in+28+4*i,4);
 for(unsigned i=0;i<6;i++)p.covarianceMm2[i]=(int32_t)(uint32_t)get(in+40+4*i,4);
 if(!mulPoseValid(&p))return false;
 *out=p;return true;
}
bool mulPoseStreamBegin(mulPoseStream_t *s,uint32_t clock,uint32_t epoch,uint32_t frame,uint16_t maxSkewMs) {
 if(!s||!clock||!frame||maxSkewMs>1000)return false;
 s->clock=clock;s->epoch=epoch;s->frame=frame;s->maxSkewMs=maxSkewMs;s->count=0;s->active=true;return true;
}
mulPoseStatus_t mulPoseStreamCommit(mulPoseStream_t *s,const mulPose_t *p) {
 if(!s||!s->active||!p)return mulPoseInvalid;
 if(p->clock!=s->clock||p->epoch!=s->epoch)return mulPoseDomain;
 if(p->frame!=s->frame)return mulPoseFrame;
 unsigned at=0;while(at<s->count&&s->poses[at].node!=p->node)at++;
 if(at<s->count) {
  const mulPose_t *old=&s->poses[at];uint32_t advance=p->sequence-old->sequence;
  if(!advance||advance>=UINT32_C(0x80000000)||p->sourceUs<old->sourceUs||
     (p->sourceUs==old->sourceUs&&(p->flags&MUL_POSE_VALID)))return mulPoseReordered;
 }else if(at==MUL_POSE_MAX)return mulPoseFull;
 else s->count++;
 s->poses[at]=*p;return mulPoseAvailable;
}
mulPoseStatus_t mulPoseStreamPut(mulPoseStream_t *s,const mulPose_t *p){return mulPoseValid(p)?mulPoseStreamCommit(s,p):mulPoseInvalid;}
unsigned mulPoseStreamExpire(mulPoseStream_t *s,uint32_t nowMs) {
 if(!s||!s->active)return 0;
 unsigned expired=0;
 for(unsigned i=0;i<s->count;i++) {
  mulPose_t *p=&s->poses[i];uint32_t elapsed=nowMs-p->receivedMs;
  if(p->ttlMs&&(p->flags&MUL_POSE_VALID)&&(elapsed>p->ttlMs||
     (p->ageUpperMs!=MUL_POSE_AGE_UNKNOWN&&elapsed+p->ageUpperMs>p->ttlMs))) {
   p->ttlMs=0;expired++;
  }
 }
 return expired;
}
const mulPose_t *mulPoseStreamGet(const mulPoseStream_t *s,uint8_t node) {
 if(!s||!s->active)return NULL;
 for(unsigned i=0;i<s->count;i++)if(s->poses[i].node==node)return &s->poses[i];
 return NULL;
}
mulPoseStatus_t mulPoseGeometry(const mulPose_t *a,const mulPose_t *b,uint32_t nowMs,uint16_t maxSkewMs,mulPoseGeometry_t *out) {
 if(!a||!b)return mulPoseMissing;
 if(!a->ttlMs||!b->ttlMs)return mulPoseStale;
 if(!out||!mulPoseValid(a)||!mulPoseValid(b)||a->node==b->node||!(a->flags&MUL_POSE_VALID)||!(b->flags&MUL_POSE_VALID)||maxSkewMs>1000)return mulPoseInvalid;
 if(a->clock!=b->clock||a->epoch!=b->epoch)return mulPoseDomain;
 if(a->frame!=b->frame)return mulPoseFrame;
 uint64_t skew=a->sourceUs>b->sourceUs?a->sourceUs-b->sourceUs:b->sourceUs-a->sourceUs;
 if(skew>(uint64_t)maxSkewMs*1000)return mulPoseSkew;
 if(a->ageUpperMs==MUL_POSE_AGE_UNKNOWN||b->ageUpperMs==MUL_POSE_AGE_UNKNOWN)return mulPoseAgeUnknown;
 uint32_t elapsedA=nowMs-a->receivedMs,elapsedB=nowMs-b->receivedMs;
 if(elapsedA>a->ttlMs||elapsedB>b->ttlMs)return mulPoseStale;
 uint32_t ageA=elapsedA+a->ageUpperMs,ageB=elapsedB+b->ageUpperMs;
 uint16_t ttl=a->ttlMs<b->ttlMs?a->ttlMs:b->ttlMs;
 uint32_t age=ageA>ageB?ageA:ageB;
 if(age>ttl)return mulPoseStale;
 double d[3],squared=0;for(unsigned i=0;i<3;i++){d[i]=(double)b->positionMm[i]-a->positionMm[i];squared+=d[i]*d[i];}
 double distance=sqrt(squared),tof=distance*(63897600.0/299792458.0);
 if(!isfinite(tof)||tof>MUL_CLOCK_DELAY_MAX_DTU)return mulPoseDistance;
 mulPoseGeometry_t g={.valid=true,.sourceUs={a->sourceUs,b->sourceUs},.clock=a->clock,.epoch=a->epoch,.frame=a->frame,
  .sequence={a->sequence,b->sequence},.reference=a->node,.follower=b->node,.distanceMm=distance,.delayDtu=(int64_t)(tof+0.5),.ageMs=age,.measuredMs=nowMs-age,.ttlMs=ttl,.skewMs=(uint16_t)((skew+999)/1000)};
 g.covarianceKnown=distance>0&&(a->flags&6u)==6u&&(b->flags&6u)==6u;
 if(g.covarianceKnown) {
  double c[6];for(unsigned i=0;i<6;i++)c[i]=(double)a->covarianceMm2[i]+b->covarianceMm2[i];
  g.varianceMm2=(d[0]*d[0]*c[0]+2*d[0]*d[1]*c[1]+2*d[0]*d[2]*c[2]+d[1]*d[1]*c[3]+2*d[1]*d[2]*c[4]+d[2]*d[2]*c[5])/squared;
  if(g.varianceMm2<0)g.varianceMm2=0; // PSD roundoff only; inputs were validated.
 }
 *out=g;return mulPoseAvailable;
}
bool mulPoseDelay(const mulNetwork_t *network,const mulClock_t *clock,const mulPoseGeometry_t *g,mulClockDelay_t *out) {
 if(!network||!mulNetworkValid(network)||!clock||!g||!out||!g->valid||network->addresses[0][0]!=g->reference||network->addresses[1][0]!=g->follower||g->delayDtu<0||g->delayDtu>MUL_CLOCK_DELAY_MAX_DTU||!g->ttlMs||g->ttlMs>MUL_CLOCK_DELAY_MAX_AGE_MS||g->ageMs>g->ttlMs||g->ageMs>MUL_PROP_MAX_AGE_MS)return false;
 mulClockDelay_t d={.source=mulDelayExternal,.epoch=clock->epoch,.oneWayDtu=g->delayDtu,.units=mulClockDelayReferenceDtu,
  .measuredMs=g->measuredMs,.referenceSequence=clock->sequence,.maxAgeMs=g->ttlMs>MUL_PROP_MAX_AGE_MS?MUL_PROP_MAX_AGE_MS:g->ttlMs,.valid=true,
  .measurementBoundDtu=MUL_CLOCK_BOUND_UNKNOWN,.motionBoundDtuPerMs=MUL_CLOCK_BOUND_UNKNOWN};
 memcpy(d.reference,network->addresses[0],8);memcpy(d.follower,network->addresses[1],8);*out=d;return true;
}
