/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_ekf.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
const char *mulEkfStatusName(mulEkfStatus_t status) {
 static const char *const names[]={"ok","disabled","invalid","time","domain","provenance","gap","ambiguous","covariance","geometry","gated","duplicate","correlation","capacity","queued","baseline","too_late","numeric"};
 return (unsigned)status<sizeof(names)/sizeof(*names)?names[status]:"invalid_status";
}
static bool finiteRange(double x,double lo,double hi){return isfinite(x)&&x>=lo&&x<=hi;}
static bool psd3(const double p[3][3]) {
 double scale=1;
 for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++) {
  if(!isfinite(p[i][j])||fabs(p[i][j])>1e12||fabs(p[i][j]-p[j][i])>1e-10*(1+fabs(p[i][j])))return false;
  if(fabs(p[i][j])>scale)scale=fabs(p[i][j]);
 }
 double a=p[0][0]/scale,b=p[0][1]/scale,c=p[0][2]/scale,d=p[1][1]/scale,e=p[1][2]/scale,f=p[2][2]/scale;
 return a>=0&&d>=0&&f>=0&&a*d-b*b>=-1e-12&&a*f-c*c>=-1e-12&&d*f-e*e>=-1e-12&&
  a*d*f+2*b*c*e-a*e*e-d*c*c-f*b*b>=-1e-12;
}
static bool chol(double a[MUL_EKF_MEASUREMENTS][MUL_EKF_MEASUREMENTS],unsigned n) {
 for(unsigned i=0;i<n;i++)for(unsigned j=0;j<=i;j++) {
  double x=a[i][j];for(unsigned k=0;k<j;k++)x-=a[i][k]*a[j][k];
  if(i==j){if(!isfinite(x)||x<=1e-15)return false;a[i][j]=sqrt(x);}
  else a[i][j]=x/a[j][j];
 }
 return true;
}
static void solve(const double l[MUL_EKF_MEASUREMENTS][MUL_EKF_MEASUREMENTS],unsigned n,const double *b,double *x) {
 for(unsigned i=0;i<n;i++){double v=b[i];for(unsigned j=0;j<i;j++)v-=l[i][j]*x[j];x[i]=v/l[i][i];}
 for(unsigned i=n;i-->0;){double v=x[i];for(unsigned j=i+1;j<n;j++)v-=l[j][i]*x[j];x[i]=v/l[i][i];}
}
bool mulEkfConfigValid(const mulEkfConfig_t *c) {
 return c&&finiteRange(c->wheelBaseM,0.001,10)&&finiteRange(c->metresPerCount,1e-7,0.1)&&
  finiteRange(c->variancePerCountM2,0,1)&&finiteRange(c->varianceFloorM2,0,1)&&
  finiteRange(c->wheelCorrelation,-0.99,0.99)&&finiteRange(c->leverXM,-10,10)&&finiteRange(c->leverYM,-10,10)&&
  finiteRange(c->heightM,-1000,1000)&&finiteRange(c->gatePerDof,1,1000)&&c->maxDeltaCount&&c->maxDeltaCount<UINT32_C(0x80000000)&&
  c->maxIntervalUs&&c->maxIntervalUs<=200000&&c->lagUs>=c->maxIntervalUs&&c->lagUs<=2000000&&
  c->maxPoseSkewUs<=100000&&c->maxTimeUncertaintyUs<=100000&&c->modes&&!(c->modes&~3u)&&
  c->allowSynthetic<=1&&c->experimentalNoise==1;
}
double mulEkfWrap(double x){return atan2(sin(x),cos(x));}
bool mulEkfInit(mulEkf_t *s,const mulEkfConfig_t *c,uint32_t clock,uint32_t epoch,uint32_t frame,
 uint64_t time,const double x[3],const double covariance[3][3]) {
 if(!s||!x||!covariance||!clock||!frame||!mulEkfConfigValid(c)||!psd3(covariance))return false;
 for(unsigned i=0;i<3;i++)if(!finiteRange(x[i],-1e6,1e6))return false;
 mulEkf_t candidate={.config=*c,.clock=clock,.epoch=epoch,.frame=frame,.timeUs=time,.initialized=true};
 memcpy(candidate.x,x,sizeof(candidate.x));candidate.x[2]=mulEkfWrap(x[2]);memcpy(candidate.p,covariance,sizeof(candidate.p));
 *s=candidate;return true;
}
bool mulEkfEnable(mulEkf_t *s,bool enabled){if(!s||!s->initialized)return false;s->enabled=enabled;return true;}
bool mulEkfMotion(const mulEkfConfig_t *c,const double x[3],double left,double right,double predicted[3],double f[3][3],double g[3][2]) {
 if(!c||!x||!predicted||!f||!g||!finiteRange(left,-1000,1000)||!finiteRange(right,-1000,1000)||!finiteRange(c->wheelBaseM,0.001,10))return false;
 for(unsigned i=0;i<3;i++)if(!isfinite(x[i]))return false;
 double d=(right+left)/2,h=(right-left)/(2*c->wheelBaseM),s,ds;
 if(fabs(h)<1e-4){double h2=h*h;s=1-h2/6+h2*h2/120;ds=-h/3+h*h2/30-h*h2*h2/840;}
 else {s=sin(h)/h;ds=(h*cos(h)-sin(h))/(h*h);}
 double angle=x[2]+h,co=cos(angle),si=sin(angle),a=d*s,dx=a*co,dy=a*si,b=2*c->wheelBaseM;
 predicted[0]=x[0]+dx;predicted[1]=x[1]+dy;predicted[2]=mulEkfWrap(x[2]+2*h);
 memset(f,0,9*sizeof(double));for(unsigned i=0;i<3;i++)f[i][i]=1;f[0][2]=-dy;f[1][2]=dx;
 double al=0.5*s-d*ds/b,ar=0.5*s+d*ds/b;
 g[0][0]=al*co+a*si/b;g[1][0]=al*si-a*co/b;g[2][0]=-1/c->wheelBaseM;
 g[0][1]=ar*co-a*si/b;g[1][1]=ar*si+a*co/b;g[2][1]=1/c->wheelBaseM;
 return isfinite(predicted[0])&&isfinite(predicted[1])&&isfinite(predicted[2]);
}
mulEkfStatus_t mulEkfPredict(mulEkf_t *s,uint64_t time,double left,double right,const double q[3]) {
 if(!s||!s->initialized||!s->enabled)return mulEkfDisabled;
 if(time<s->timeUs||time-s->timeUs>s->config.maxIntervalUs)return mulEkfTime;
 if(!q||!finiteRange(q[0],0,1e6)||!finiteRange(q[2],0,1e6)||!isfinite(q[1])||q[1]*q[1]>q[0]*q[2]+1e-20)return mulEkfCovariance;
 if(time==s->timeUs)return left==0&&right==0&&q[0]==0&&q[1]==0&&q[2]==0?mulEkfOk:mulEkfTime;
 double x[3],f[3][3],g[3][2],p[3][3]={0};
 if(!mulEkfMotion(&s->config,s->x,left,right,x,f,g))return mulEkfInvalid;
 for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++) {
  for(unsigned a=0;a<3;a++)for(unsigned b=0;b<3;b++)p[i][j]+=f[i][a]*s->p[a][b]*f[j][b];
  p[i][j]+=g[i][0]*q[0]*g[j][0]+g[i][0]*q[1]*g[j][1]+g[i][1]*q[1]*g[j][0]+g[i][1]*q[2]*g[j][1];
 }
 if(!psd3(p))return mulEkfNumeric;
 memcpy(s->x,x,sizeof(x));memcpy(s->p,p,sizeof(p));s->timeUs=time;s->predictions++;return mulEkfOk;
}
static mulEkfStatus_t odomValid(const mulEkfConfig_t *c,const mulEkfOdom_t *p) {
 if(!c||!p||!mulEkfConfigValid(c)||!p->clock||(p->bits!=16&&p->bits!=32)||p->timeUncertaintyUs>c->maxTimeUncertaintyUs||
    (p->bits==16&&(p->left>65535||p->right>65535||c->maxDeltaCount>=32768)))return mulEkfInvalid;
 if(p->provenance!=mulOdomMeasured&&!(p->provenance==mulOdomSyntheticMeasured&&c->allowSynthetic))return mulEkfProvenance;
 return mulEkfOk;
}
mulEkfStatus_t mulEkfWheelDelta(const mulEkfConfig_t *c,const mulEkfOdom_t *a,const mulEkfOdom_t *b,double *left,double *right,double q[3]) {
 mulEkfStatus_t status=odomValid(c,a);if(status!=mulEkfOk)return status;
 status=odomValid(c,b);if(status!=mulEkfOk)return status;
 if(!left||!right||!q)return mulEkfInvalid;
 if(a->clock!=b->clock||a->epoch!=b->epoch||a->bits!=b->bits||a->provenance!=b->provenance)return mulEkfDomain;
 uint32_t seq=b->sequence-a->sequence;
 if(!seq||seq>=UINT32_C(0x80000000)||b->timeUs<=a->timeUs)return mulEkfTime;
 if(b->timeUs-a->timeUs>c->maxIntervalUs)return mulEkfGap;
 uint64_t modulus=UINT64_C(1)<<b->bits,mask=modulus-1,half=modulus/2;
 uint64_t l=((uint64_t)b->left-a->left)&mask,r=((uint64_t)b->right-a->right)&mask;
 if(l==half||r==half)return mulEkfAmbiguous;
 int64_t dl=l<half?(int64_t)l:-(int64_t)(modulus-l),dr=r<half?(int64_t)r:-(int64_t)(modulus-r);
 if(llabs(dl)>c->maxDeltaCount||llabs(dr)>c->maxDeltaCount)return mulEkfGap;
 *left=dl*c->metresPerCount;*right=dr*c->metresPerCount;
 q[0]=llabs(dl)*c->variancePerCountM2+c->varianceFloorM2;q[2]=llabs(dr)*c->variancePerCountM2+c->varianceFloorM2;
 q[1]=c->wheelCorrelation*sqrt(q[0]*q[2]);return mulEkfOk;
}
bool mulEkfRangeModel(const mulEkfConfig_t *c,const double x[3],const mulPose_t *a,double *range,double h[3],double j[3]) {
 if(!c||!x||!a||!range||!h||!j)return false;
 double co=cos(x[2]),si=sin(x[2]),dx=-si*c->leverXM-co*c->leverYM,dy=co*c->leverXM-si*c->leverYM;
 double d[3]={x[0]+co*c->leverXM-si*c->leverYM-a->positionMm[0]/1000.0,
  x[1]+si*c->leverXM+co*c->leverYM-a->positionMm[1]/1000.0,c->heightM-a->positionMm[2]/1000.0};
 double r=sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);if(!isfinite(r)||r<1e-6)return false;
 for(unsigned k=0;k<3;k++)j[k]=-d[k]/r;
 h[0]=-j[0];h[1]=-j[1];h[2]=-j[0]*dx-j[1]*dy;*range=r;return true;
}
mulEkfStatus_t mulEkfBatchValid(const mulEkf_t *s,const mulEkfBatch_t *b,mulEkfWork_t *w) {
 if(!s||!s->initialized||!b||!w||!b->group||!b->count||b->count>MUL_EKF_MEASUREMENTS||!b->anchorCount||b->anchorCount>MUL_POSE_MAX)return mulEkfInvalid;
 if(b->clock!=s->clock||b->epoch!=s->epoch||b->frame!=s->frame)return mulEkfDomain;
 if(b->timeUncertaintyUs>s->config.maxTimeUncertaintyUs)return mulEkfTime;
 if(b->independentPoseTimes!=1)return mulEkfCorrelation;
 for(unsigned i=0;i<b->anchorCount;i++) {
  const mulPose_t *p=&b->anchors[i];uint64_t skew=p->sourceUs>b->timeUs?p->sourceUs-b->timeUs:b->timeUs-p->sourceUs;
  if(!mulPoseValid(p)||(p->flags&7)!=7||p->ageUpperMs==MUL_POSE_AGE_UNKNOWN||p->ageUpperMs>p->ttlMs||
     p->timeUncertaintyMs==UINT16_MAX||(uint32_t)p->timeUncertaintyMs*1000>s->config.maxTimeUncertaintyUs||skew>s->config.maxPoseSkewUs)return mulEkfCovariance;
  if(p->clock!=s->clock||p->epoch!=s->epoch||p->frame!=s->frame)return mulEkfDomain;
  for(unsigned j=0;j<i;j++)if(p->node==b->anchors[j].node)return mulEkfInvalid;
 }
 for(unsigned i=0;i<b->count;i++) {
  const mulEkfMeasurement_t *m=&b->measurements[i];
  if(!m->id||!m->valid||!isfinite(m->valueM)||fabs(m->valueM)>10000||
     (m->kind!=MUL_EKF_TDOA&&m->kind!=MUL_EKF_RANGE)||!(s->config.modes&m->kind)||m->anchor>=b->anchorCount||
     (m->kind==MUL_EKF_TDOA&&(!m->rateValid||m->reference>=b->anchorCount||m->reference==m->anchor))||
     (m->kind==MUL_EKF_RANGE&&m->valueM<0))return mulEkfInvalid;
  for(unsigned j=0;j<i;j++)if(m->id==b->measurements[j].id)return mulEkfDuplicate;
  for(unsigned j=0;j<b->count;j++) {
   double v=b->noise[i][j];if(!isfinite(v)||fabs(v)>1e6||fabs(v-b->noise[j][i])>1e-12*(1+fabs(v)))return mulEkfCovariance;
   w->s[i][j]=v;
  }
 }
 return chol(w->s,b->count)?mulEkfOk:mulEkfCovariance;
}
static void remember(mulEkf_t *s,const mulEkfBatch_t *batch) {
 uint64_t group=batch->group;
 for(unsigned i=0;i<batch->count;i++){s->ids[s->idNext]=batch->measurements[i].id;s->idNext=(s->idNext+1)%MUL_EKF_IDS;if(s->idCount<MUL_EKF_IDS)s->idCount++;}
 s->groups[s->groupNext]=group;s->groupNext=(s->groupNext+1)%MUL_EKF_GROUPS;
 if(s->groupCount<MUL_EKF_GROUPS)s->groupCount++;
}
mulEkfStatus_t mulEkfUpdate(mulEkf_t *s,const mulEkfBatch_t *b,mulEkfWork_t *w) {
 if(!s||!s->enabled)return mulEkfDisabled;
 mulEkfStatus_t valid=mulEkfBatchValid(s,b,w);if(valid!=mulEkfOk)return valid;
 if(b->timeUs!=s->timeUs)return mulEkfTime;
 for(unsigned i=0;i<s->groupCount;i++)if(s->groups[i]==b->group){s->duplicates++;return mulEkfDuplicate;}
 for(unsigned i=0;i<b->count;i++)for(unsigned j=0;j<s->idCount;j++)if(b->measurements[i].id==s->ids[j]){s->duplicates++;return mulEkfDuplicate;}
 unsigned n=b->count;bool used[MUL_POSE_MAX]={0};
 memset(w->j,0,sizeof(w->j));
 for(unsigned i=0;i<n;i++) {
  const mulEkfMeasurement_t *m=&b->measurements[i];double expected,h[3],j[3];
  if(!mulEkfRangeModel(&s->config,s->x,&b->anchors[m->anchor],&expected,h,j))return mulEkfGeometry;
  used[m->anchor]=true;
  for(unsigned k=0;k<3;k++){w->h[i][k]=h[k];w->j[i][m->anchor][k]=j[k];}
  if(m->kind==MUL_EKF_TDOA) {
   double reference;if(!mulEkfRangeModel(&s->config,s->x,&b->anchors[m->reference],&reference,h,j))return mulEkfGeometry;
   expected-=reference;used[m->reference]=true;
   for(unsigned k=0;k<3;k++){w->h[i][k]-=h[k];w->j[i][m->reference][k]-=j[k];}
  }
  w->residual[i]=m->valueM-expected;
 }
 unsigned newNodes=0;
 for(unsigned a=0;a<b->anchorCount;a++)if(used[a]) {
  const mulPose_t *p=&b->anchors[a];unsigned k=0;while(k<s->usedCount&&s->used[k].node!=p->node)k++;
  if(k==s->usedCount)newNodes++;
  else {uint32_t advance=p->sequence-s->used[k].sequence;if(p->sourceUs<=s->used[k].sourceUs||!advance||advance>=UINT32_C(0x80000000))return mulEkfCorrelation;}
 }
 if(s->usedCount+newNodes>MUL_POSE_MAX)return mulEkfCapacity;
 for(unsigned i=0;i<n;i++)for(unsigned j=0;j<n;j++) {
  double value=b->noise[i][j];
  for(unsigned a=0;a<b->anchorCount;a++)if(used[a]) {
   const int32_t *c=b->anchors[a].covarianceMm2;double cov[3][3]={{c[0],c[1],c[2]},{c[1],c[3],c[4]},{c[2],c[4],c[5]}};
   for(unsigned k=0;k<3;k++)for(unsigned l=0;l<3;l++)value+=w->j[i][a][k]*cov[k][l]*w->j[j][a][l]/1e6;
  }
  w->r[i][j]=value;
 }
 for(unsigned k=0;k<3;k++)for(unsigned i=0;i<n;i++){w->ph[k][i]=0;for(unsigned a=0;a<3;a++)w->ph[k][i]+=s->p[k][a]*w->h[i][a];}
 for(unsigned i=0;i<n;i++)for(unsigned j=0;j<n;j++){double value=w->r[i][j];for(unsigned a=0;a<3;a++)value+=w->h[i][a]*w->ph[a][j];w->s[i][j]=value;}
 if(!chol(w->s,n))return mulEkfNumeric;
 solve(w->s,n,w->residual,w->temp);double nis=0;for(unsigned i=0;i<n;i++)nis+=w->residual[i]*w->temp[i];
 if(!isfinite(nis)||nis< -1e-10)return mulEkfNumeric;
 s->nis=nis;s->innovationCount=(uint8_t)n;s->lastGroup=b->group;memcpy(s->innovation,w->residual,n*sizeof(double));
 if(nis>s->config.gatePerDof*n){remember(s,b);s->gated++;return mulEkfGated;}
 for(unsigned a=0;a<3;a++)solve(w->s,n,w->ph[a],w->k[a]);
 double x[3],p[3][3]={0},a[3][3];
 for(unsigned i=0;i<3;i++) {
  x[i]=s->x[i];for(unsigned j=0;j<n;j++)x[i]+=w->k[i][j]*w->residual[j];
  if(!finiteRange(x[i],-1e6,1e6))return mulEkfNumeric;
  for(unsigned j=0;j<3;j++){a[i][j]=i==j;for(unsigned k=0;k<n;k++)a[i][j]-=w->k[i][k]*w->h[k][j];}
 }
 x[2]=mulEkfWrap(x[2]);
 for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++) {
  for(unsigned k=0;k<3;k++)for(unsigned l=0;l<3;l++)p[i][j]+=a[i][k]*s->p[k][l]*a[j][l];
  for(unsigned k=0;k<n;k++)for(unsigned l=0;l<n;l++)p[i][j]+=w->k[i][k]*w->r[k][l]*w->k[j][l];
 }
 for(unsigned i=0;i<3;i++)for(unsigned j=0;j<i;j++){double mean=(p[i][j]+p[j][i])/2;p[i][j]=p[j][i]=mean;}
 if(!psd3(p))return mulEkfNumeric;
 memcpy(s->x,x,sizeof(x));memcpy(s->p,p,sizeof(p));remember(s,b);s->updates++;
 for(unsigned i=0;i<b->anchorCount;i++)if(used[i]) {
  unsigned k=0;while(k<s->usedCount&&s->used[k].node!=b->anchors[i].node)k++;
  if(k==s->usedCount)s->usedCount++;
  s->used[k]=(mulEkfPoseUse_t){.node=b->anchors[i].node,.sourceUs=b->anchors[i].sourceUs,.sequence=b->anchors[i].sequence};
 }
 return mulEkfOk;
}
