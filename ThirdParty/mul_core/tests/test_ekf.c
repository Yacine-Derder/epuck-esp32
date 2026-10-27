#include "mul_ekf_stream.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static mulEkfConfig_t config(void) {
 return (mulEkfConfig_t){.wheelBaseM=.2,.metresPerCount=.001,.variancePerCountM2=1e-8,.varianceFloorM2=1e-9,
  .gatePerDof=25,.maxDeltaCount=10000,.maxIntervalUs=200000,.maxTimeUncertaintyUs=1000,.lagUs=200000,
  .modes=3,.allowSynthetic=1,.experimentalNoise=1};
}
static void init(mulEkf_t *s) {
 mulEkfConfig_t c=config();double x[3]={1,1,.2},p[3][3]={{.04,.005,0},{.005,.04,0},{0,0,.01}};
 assert(mulEkfInit(s,&c,1,2,3,0,x,p));
 assert(!s->enabled);assert(mulEkfEnable(s,true));
}
static mulPose_t anchor(unsigned node,int x,int y,uint64_t time) {
 return (mulPose_t){.clock=1,.epoch=2,.frame=3,.sequence=(uint32_t)time,.sourceUs=time,.ttlMs=1000,
  .node=node,.flags=7,.positionMm={x,y,1000},.covarianceMm2={400,20,10,900,30,100}};
}
static mulEkfBatch_t batch(uint64_t time,uint64_t group) {
 mulEkfBatch_t b={.clock=1,.epoch=2,.frame=3,.timeUs=time,.group=group,.count=2,.anchorCount=3,.independentPoseTimes=1};
 b.anchors[0]=anchor(132,0,0,time);b.anchors[1]=anchor(133,4000,0,time);b.anchors[2]=anchor(134,0,4000,time);
 b.measurements[0]=(mulEkfMeasurement_t){.id=group*8+1,.kind=MUL_EKF_TDOA,.anchor=1,.reference=0,.valid=1,.rateValid=1};
 b.measurements[1]=(mulEkfMeasurement_t){.id=group*8+2,.kind=MUL_EKF_TDOA,.anchor=2,.reference=0,.valid=1,.rateValid=1};
 b.noise[0][0]=b.noise[1][1]=.01;b.noise[0][1]=b.noise[1][0]=.002;return b;
}
static void expected(mulEkfBatch_t *b,const mulEkf_t *s) {
 for(unsigned i=0;i<b->count;i++) {
  const mulEkfMeasurement_t *m=&b->measurements[i];double value,h[3],j[3],reference;
  assert(mulEkfRangeModel(&s->config,s->x,&b->anchors[m->anchor],&value,h,j));
  if(m->kind==MUL_EKF_TDOA){assert(mulEkfRangeModel(&s->config,s->x,&b->anchors[m->reference],&reference,h,j));value-=reference;}
  b->measurements[i].valueM=value;
 }
}
static void jacobians(void) {
 mulEkfConfig_t c=config();c.leverXM=.04;c.leverYM=-.03;
 const double inputs[][2]={{.1,.1},{-.04,.08},{-.1,.1},{.1000000001,.1}};
 double x[3]={1,2,1.1};
 for(unsigned k=0;k<4;k++) {
  double y[3],f[3][3],g[3][2];assert(mulEkfMotion(&c,x,inputs[k][0],inputs[k][1],y,f,g));
  for(unsigned a=0;a<5;a++) {
   double plus[3],minus[3],state[3],ff[3][3],gg[3][2],e=1e-6;memcpy(state,x,sizeof(state));
   double l=inputs[k][0],r=inputs[k][1];if(a<3)state[a]+=e;else if(a==3)l+=e;else r+=e;
   assert(mulEkfMotion(&c,state,l,r,plus,ff,gg));memcpy(state,x,sizeof(state));l=inputs[k][0];r=inputs[k][1];
   if(a<3)state[a]-=e;else if(a==3)l-=e;else r-=e;
   assert(mulEkfMotion(&c,state,l,r,minus,ff,gg));
   for(unsigned i=0;i<3;i++){double numeric=(i==2?mulEkfWrap(plus[i]-minus[i]):plus[i]-minus[i])/(2*e);assert(fabs(numeric-(a<3?f[i][a]:g[i][a-3]))<1e-7);}
  }
 }
 mulPose_t p=anchor(132,-2000,3000,0);double value,h[3],j[3];assert(mulEkfRangeModel(&c,x,&p,&value,h,j));
 for(unsigned i=0;i<3;i++){double state[3],a,b,hh[3],jj[3];memcpy(state,x,sizeof(state));state[i]+=1e-6;assert(mulEkfRangeModel(&c,state,&p,&a,hh,jj));state[i]-=2e-6;assert(mulEkfRangeModel(&c,state,&p,&b,hh,jj));assert(fabs((a-b)/2e-6-h[i])<1e-7);}
}
static void counters(void) {
 mulEkfConfig_t c=config();double l=0,r=0,q[3];
 for(unsigned bits=16;bits<=32;bits+=16) {
  uint32_t mask=bits==16?65535:UINT32_MAX;
  mulEkfOdom_t a={.clock=1,.epoch=2,.sequence=UINT32_MAX,.timeUs=UINT64_C(0x100000000000),.bits=bits,.provenance=mulOdomMeasured,.left=mask-4,.right=3};
  mulEkfOdom_t b=a;b.sequence=0;b.timeUs+=100000;b.left=5;b.right=mask-6;
  assert(mulEkfWheelDelta(&c,&a,&b,&l,&r,q)==mulEkfOk&&fabs(l-.010)<1e-12&&fabs(r+.010)<1e-12);
  b.left=(uint32_t)(((uint64_t)a.left+(UINT64_C(1)<<(bits-1)))&mask);assert(mulEkfWheelDelta(&c,&a,&b,&l,&r,q)==mulEkfAmbiguous);
  b=a;b.sequence=0;b.timeUs+=200001;assert(mulEkfWheelDelta(&c,&a,&b,&l,&r,q)==mulEkfGap);
  b=a;b.sequence=0;b.timeUs++;b.epoch++;assert(mulEkfWheelDelta(&c,&a,&b,&l,&r,q)==mulEkfDomain);
  b=a;b.provenance=mulOdomCommanded;assert(mulEkfWheelDelta(&c,&b,&b,&l,&r,q)==mulEkfProvenance);
  b.provenance=mulOdomUnknown;assert(mulEkfWheelDelta(&c,&b,&b,&l,&r,q)==mulEkfProvenance);
 }
 c.allowSynthetic=0;mulEkfOdom_t a={.clock=1,.bits=32,.provenance=mulOdomSyntheticMeasured};assert(mulEkfWheelDelta(&c,&a,&a,&l,&r,q)==mulEkfProvenance);
}
static void updates(void) {
 mulEkf_t s;init(&s);mulEkfWork_t w;mulEkfBatch_t b=batch(0,1);expected(&b,&s);
 mulEkf_t before=s;assert(mulEkfUpdate(&s,&b,&w)==mulEkfOk);assert(s.updates==1&&s.p[0][0]<before.p[0][0]);
 // Shared reference uncertainty and supplied cross-radio covariance both appear.
 double dot=0;const int32_t *c=b.anchors[0].covarianceMm2;
 double cov[3][3]={{c[0],c[1],c[2]},{c[1],c[3],c[4]},{c[2],c[4],c[5]}};
 for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++)dot+=w.j[0][0][i]*cov[i][j]*w.j[1][0][j]/1e6;
 assert(fabs(w.r[0][1]-(.002+dot))<1e-12&&fabs(dot)>1e-5);
 assert(mulEkfUpdate(&s,&b,&w)==mulEkfDuplicate);
 b.group=2;b.measurements[0].id+=100;b.measurements[1].id+=100;assert(mulEkfUpdate(&s,&b,&w)==mulEkfCorrelation);
 double q[3]={1e-6,0,1e-6};assert(mulEkfPredict(&s,100000,.1,.12,q)==mulEkfOk);
 b=batch(100000,3);expected(&b,&s);b.measurements[0].valueM+=100;
 before=s;assert(mulEkfUpdate(&s,&b,&w)==mulEkfGated);assert(!memcmp(s.x,before.x,sizeof(s.x))&&!memcmp(s.p,before.p,sizeof(s.p)));
 assert(mulEkfUpdate(&s,&b,&w)==mulEkfDuplicate);
 b=batch(100000,4);expected(&b,&s);b.anchors[0].flags=3;before=s;
 assert(mulEkfUpdate(&s,&b,&w)==mulEkfCovariance);assert(!memcmp(s.x,before.x,sizeof(s.x)));
 b=batch(100000,5);expected(&b,&s);b.measurements[0].rateValid=0;assert(mulEkfUpdate(&s,&b,&w)==mulEkfInvalid);
 b=batch(100000,6);expected(&b,&s);b.noise[0][1]=b.noise[1][0]=1;assert(mulEkfUpdate(&s,&b,&w)==mulEkfCovariance);
 b=batch(100000,7);expected(&b,&s);b.independentPoseTimes=0;assert(mulEkfUpdate(&s,&b,&w)==mulEkfCorrelation);
 b=batch(100000,8);b.count=1;b.measurements[0].kind=MUL_EKF_RANGE;expected(&b,&s);assert(mulEkfUpdate(&s,&b,&w)==mulEkfOk);
 assert(s.p[0][0]>=0&&s.p[1][1]>=0&&s.p[2][2]>=0);
}
static void replay(void) {
 mulEkfStream_t ordered,delayed;mulEkfConfig_t c=config();double x[3]={1,1,.2},p[3][3]={{.04,0,0},{0,.04,0},{0,0,.01}};
 assert(mulEkfStreamInit(&ordered,&c,1,2,3,0,x,p)&&mulEkfStreamInit(&delayed,&c,1,2,3,0,x,p));
 assert(mulEkfStreamEnable(&ordered,true)&&mulEkfStreamEnable(&delayed,true));
 mulEkfOdom_t odom={.clock=1,.epoch=2,.bits=32,.provenance=mulOdomSyntheticMeasured};
 assert(mulEkfStreamOdom(&ordered,&odom)==mulEkfBaseline&&mulEkfStreamOdom(&delayed,&odom)==mulEkfBaseline);
 mulEkfBatch_t a=batch(50000,1),b=batch(150000,2);expected(&a,&ordered.current);expected(&b,&ordered.current);
 assert(mulEkfStreamBatch(&ordered,&a)==mulEkfQueued);
 odom.sequence=1;odom.timeUs=100000;odom.left=100;odom.right=120;assert(mulEkfStreamOdom(&ordered,&odom)==mulEkfOk&&mulEkfStreamOdom(&delayed,&odom)==mulEkfOk);
 assert(mulEkfStreamBatch(&ordered,&b)==mulEkfQueued);
 odom.sequence=2;odom.timeUs=200000;odom.left=200;odom.right=240;assert(mulEkfStreamOdom(&ordered,&odom)==mulEkfOk&&mulEkfStreamOdom(&delayed,&odom)==mulEkfOk);
 assert(mulEkfStreamBatch(&delayed,&b)==mulEkfOk);assert(mulEkfStreamBatch(&delayed,&a)==mulEkfOk);
 for(unsigned i=0;i<3;i++){assert(fabs(ordered.current.x[i]-delayed.current.x[i])<1e-12);for(unsigned j=0;j<3;j++)assert(fabs(ordered.current.p[i][j]-delayed.current.p[i][j])<1e-12);}
 assert(mulEkfStreamBatch(&delayed,&a)==mulEkfDuplicate);
 for(unsigned i=3;i<15;i++){odom.sequence=i;odom.timeUs=i*100000;odom.left=i*100;odom.right=i*120;assert(mulEkfStreamOdom(&delayed,&odom)==mulEkfOk);}
 assert(mulEkfStreamBatch(&delayed,&a)==mulEkfTooLate);
}
int main(void) {
 jacobians();counters();updates();replay();
 printf("EKF: exact arc/Jacobian finite differences,16/32 counters/provenance, joint shared-pose/radio covariance, Joseph PSD, gating/duplicates/NO_RATE and fixed-lag ordered/delayed equivalence passed; state=%zu workspace=%zu stream=%zu\n",sizeof(mulEkf_t),sizeof(mulEkfWork_t),sizeof(mulEkfStream_t));
}
