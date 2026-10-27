#include "mul_pose.h"
#include "mul_schedule.h"
#include "mul_plan_protocol.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static mulPose_t pose(uint8_t node,uint32_t now) {
 return (mulPose_t){.clock=1,.epoch=2,.frame=3,.sequence=UINT32_MAX,.sourceUs=1000000,
  .receivedMs=now,.ageUpperMs=20,.ttlMs=1000,.timeUncertaintyMs=UINT16_MAX,.node=node,
  .flags=7,.covarianceMm2={100,20,0,400,0,900}};
}
int main(void) {
 mulPose_t a=pose(132,UINT32_MAX-10),b=pose(133,UINT32_MAX-10),decoded;
 b.positionMm[0]=3000;b.positionMm[1]=4000;
 uint8_t bytes[80];assert(mulPoseEncode(bytes,&a)&&mulPoseDecode(bytes,123,&decoded)&&decoded.receivedMs==123);
 for(unsigned i=0;i<80;i++){bytes[i]^=1;assert(!mulPoseDecode(bytes,0,&decoded));bytes[i]^=1;}
 mulPoseGeometry_t g;assert(mulPoseGeometry(&a,&b,9,0,&g)==mulPoseAvailable);
 assert(g.distanceMm==5000&&g.delayDtu==1066&&g.ageMs==40&&g.measuredMs==(uint32_t)(9-40));
 assert(g.covarianceKnown&&fabs(g.varianceMm2-622.4)<1e-8);
 mulClock_t clock={.epoch=17,.sequence=3};mulNetwork_t network;assert(mulNetworkInit(&network,132,133));
 mulClockDelay_t delay;assert(mulPoseDelay(&network,&clock,&g,&delay));
 assert(delay.source==mulDelayExternal);
 assert(delay.epoch==17&&delay.referenceSequence==3&&delay.measurementBoundDtu==UINT64_MAX&&delay.motionBoundDtuPerMs==UINT64_MAX);
 assert(mulClockDelayStatusNetwork(&network,&clock,&delay,9)==mulClockDelayValid);
 clock.hasBaseline=true;clock.status=mulClockValid;clock.referenceTx=100000;clock.localRx=200000;clock.observedMs=9;
 uint64_t tx=clock.localRx+10*UINT64_C(63897600),hw=clock.localRx+UINT64_C(63897600);
 mulRelation_t relation=mulRelationPublishNetwork(&network,&clock,&delay,tx,hw,9,false);
 assert(relation.available&&relation.version==3&&mulRelationSource(&relation)==mulDelayExternal&&relation.maxAgeMs==1000);
 mulTestPacket_t packet={.sequence=3,.txTimestamp=tx};memcpy(packet.source,network.addresses[1],8);
 uint8_t frame[90];mulRelationEncode(frame,&packet,&relation);mulRelation_t parsed;mulTestPacket_t parsedPacket;
 assert(mulBroadcastDecodeNetwork(&network,frame,74,&parsedPacket,&parsed)&&mulRelationSource(&parsed)==mulDelayExternal);
 uint64_t reference;assert(mulRelationInverse(&parsed,tx,0,&reference)&&reference==clock.referenceTx+10*UINT64_C(63897600)+1066);
 frame[35]|=4;assert(!mulBroadcastDecodeNetwork(&network,frame,74,&parsedPacket,&parsed));
 relation=mulRelationPublishSourceNetwork(&network,&clock,NULL,tx,hw,9,false,mulDelayNone);
 mulRelationEncode(frame,&packet,&relation);assert(mulBroadcastDecodeNetwork(&network,frame,74,&parsedPacket,&parsed)&&!parsed.available&&mulRelationSource(&parsed)==mulDelayNone);
 relation=mulRelationPublishSourceNetwork(&network,&clock,NULL,tx,hw,9,false,mulDelayExternal);
 mulRelationEncode(frame,&packet,&relation);assert(mulBroadcastDecodeNetwork(&network,frame,74,&parsedPacket,&parsed)&&!parsed.available&&mulRelationSource(&parsed)==mulDelayExternal);
 mulPoseGeometry_t sentinel={.delayDtu=42};
 b.sourceUs++;assert(mulPoseGeometry(&a,&b,9,0,&sentinel)==mulPoseSkew&&sentinel.delayDtu==42);
 b.sourceUs=a.sourceUs;b.frame++;assert(mulPoseGeometry(&a,&b,9,0,&g)==mulPoseFrame);b.frame--;
 b.epoch++;assert(mulPoseGeometry(&a,&b,9,0,&g)==mulPoseDomain);b.epoch--;
 b.ageUpperMs=UINT16_MAX;assert(mulPoseGeometry(&a,&b,9,0,&g)==mulPoseAgeUnknown);b.ageUpperMs=20;
 assert(mulPoseGeometry(&a,&b,1000,0,&g)==mulPoseStale);
 assert(mulPoseGeometry(&a,&b,UINT32_MAX-11,0,&g)==mulPoseStale);
 b.flags=3;assert(mulPoseGeometry(&a,&b,9,0,&g)==mulPoseAvailable&&!g.covarianceKnown);
 b.flags=0;memset(b.covarianceMm2,0,sizeof(b.covarianceMm2));assert(mulPoseGeometry(&a,&b,9,0,&g)==mulPoseInvalid);
 b=pose(133,0);b.covarianceMm2[1]=1000;assert(!mulPoseValid(&b));
 b.covarianceMm2[1]=-20;assert(mulPoseValid(&b));
 b.positionMm[0]=400000;assert(mulPoseGeometry(&a,&b,0,0,&g)==mulPoseDistance);
 mulPoseStream_t s;assert(mulPoseStreamBegin(&s,1,2,3,20));assert(mulPoseStreamPut(&s,&a)==mulPoseAvailable);
 assert(mulPoseStreamPut(&s,&a)==mulPoseReordered);a.sequence=0;a.sourceUs++;
 assert(mulPoseStreamPut(&s,&a)==mulPoseAvailable);a.sequence=UINT32_C(0x80000000);a.sourceUs++;
 assert(mulPoseStreamPut(&s,&a)==mulPoseReordered);a.sequence=1;a.sourceUs--;
 assert(mulPoseStreamPut(&s,&a)==mulPoseReordered);
 a.flags=0;memset(a.covarianceMm2,0,sizeof(a.covarianceMm2));assert(mulPoseStreamPut(&s,&a)==mulPoseAvailable);
 assert(!(mulPoseStreamGet(&s,132)->flags&MUL_POSE_VALID));
 a.epoch=3;a.sourceUs++;a.sequence++;assert(mulPoseStreamPut(&s,&a)==mulPoseDomain);
 assert(mulPoseStreamBegin(&s,1,3,3,0)&&!mulPoseStreamGet(&s,132));assert(mulPoseStreamPut(&s,&a)==mulPoseAvailable);
 for(unsigned i=0;i<7;i++){a.node=i;assert(mulPoseStreamPut(&s,&a)==mulPoseAvailable);}
 a.node=200;assert(mulPoseStreamPut(&s,&a)==mulPoseFull);
 a=pose(132,0);b=pose(133,0);b.positionMm[0]=10000;
 assert(mulPoseStreamBegin(&s,1,2,3,0));assert(mulPoseStreamPut(&s,&a)==mulPoseAvailable&&mulPoseStreamPut(&s,&b)==mulPoseAvailable);
 assert(mulPoseStreamExpire(&s,1001)==2);
 assert(mulPoseGeometry(mulPoseStreamGet(&s,132),mulPoseStreamGet(&s,133),0,0,&g)==mulPoseStale);
 assert(!mulPoseEncode(bytes,mulPoseStreamGet(&s,132)));
 a.sequence=b.sequence=0;a.sourceUs++;b.sourceUs++;
 assert(mulPoseStreamPut(&s,&a)==mulPoseAvailable&&mulPoseStreamPut(&s,&b)==mulPoseAvailable);
 assert(mulPoseGeometry(mulPoseStreamGet(&s,132),mulPoseStreamGet(&s,133),0,0,&g)==mulPoseAvailable);
 puts("Pose interface: strict80-byte CRC/PSD covariance, separate source/local time, age/TTL/skew/domain/frame/validity, sequence+uptime wrap, epoch reset, bounded bank, geometry covariance and UNKNOWN physical bounds passed");
}
