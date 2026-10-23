/* Synthetic physical clocks -> original wire frames -> actual C telemetry. */
/* Historical fixtures intentionally default to the prior500ms deployment. */
#ifndef MUL_FOLLOWER_SLOT_MS
#define MUL_FOLLOWER_SLOT_MS 500
#endif
#include "mul_hybrid.h"
#include "mul_telemetry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MS UINT64_C(63897600)
static int64_t scale(int64_t v,int32_t ppb){return v+(v*ppb+((v*ppb)<0?-500000000:500000000))/1000000000;}
int main(int argc,char **argv) {
 bool scheduled=argc>10&&atoi(argv[10]);
 bool stationary=argc==1||atoi(argv[1]);uint32_t sequence=0;
 /* Optional synthetic propagation/clock inputs; defaults preserve old fixtures.
  * Distances are converted from independent geometry by the host test. Origins
  * below are simultaneous antenna epochs, not delayed RX antenna coordinates. */
 uint64_t referencePath=argc>2?strtoull(argv[2],NULL,10):4000;
 uint64_t followerPath=argc>3?strtoull(argv[3],NULL,10):4477;
 int32_t receiverPpb=argc>4?(int32_t)strtol(argv[4],NULL,10):10000;
 int32_t followerPpb=argc>5?(int32_t)strtol(argv[5],NULL,10):-5000;
 uint32_t anchorPath=argc>6?(uint32_t)strtoul(argv[6],NULL,10):4263;
 uint64_t receiverOrigin=argc>7?strtoull(argv[7],NULL,10):900000;
 uint8_t referenceNode=argc>8?(uint8_t)strtoul(argv[8],NULL,10):0x84;
 uint8_t followerNode=argc>9?(uint8_t)strtoul(argv[9],NULL,10):0x85;
 for(unsigned i=0;i<11;i++) {
  uint8_t frame[84];uint64_t ref=mulTimestampMask(MUL_TIMESTAMP_MASK-2*1000*MS+i*1000*MS);
  uint64_t origin=mulTimestampMask(MUL_TIMESTAMP_MASK-100000+scale(i*1000*MS,followerPpb));
  for(unsigned follower=0;follower<2;follower++) {
   mulTestPacket_t packet={.source={follower?followerNode:referenceNode,0,0,0,0,0,0xcf,0xbc},.sequence=i+(follower?500:0),
    .slotMs=scheduled?MUL_FOLLOWER_SLOT_MS:0,
    .txTimestamp=follower?mulTimestampMask(origin+scale(MUL_FOLLOWER_SLOT_DTU,followerPpb)):ref};
   unsigned n=34;
   if(follower) {
    unsigned twr=i?((i-1)/5)*5:0;
    mulRelation_t r={.version=2,.epoch=1,.referenceSequence=i,.maxAgeMs=6000};
    if(i>=5)r=(mulRelation_t){.version=2,.epoch=1,.referenceSequence=i,.twrSequence=twr,.maxAgeMs=6000,
     .referenceOrigin=ref,.followerOrigin=origin,.skewPpb=followerPpb,.originAgeMs=MUL_FOLLOWER_SLOT_MS,
     .delayAgeMs=(i-twr)*1000+MUL_FOLLOWER_SLOT_MS,.delayDtu=anchorPath,.delayState=i-twr<=1?mulPropagationFresh:mulPropagationReused,.available=true};
    mulRelationEncode(frame,&packet,&r);n=74;
   }else if(scheduled) {if(!mulEncodeScheduledPacket(frame,packet.source,packet.sequence,packet.txTimestamp,MUL_FOLLOWER_SLOT_MS))return 1;}
   else mulEncodeTestPacket(frame,packet.source,packet.sequence,packet.txTimestamp);
   mulTelemetry_t r={.type=mulTlmHybridRx,.node=33,.peer=packet.source[0],.radio=2,.remoteRadio=1,.role=2,.mode=stationary?5:4,
     .epoch=500,.exchange=follower?1:0,.sequence=sequence++,.transaction=packet.sequence,.uptimeMs=i*1000+(follower?MUL_FOLLOWER_SLOT_MS:0),
     .flags=mulTlmStampValid,.legacyLength=n,.legacySlotMs=MUL_FOLLOWER_SLOT_MS,.captureAgeMs=1,.ratePolicy=stationary,.referenceGeneration=1,.followerGeneration=1,
     .legacyRx=mulTimestampMask(receiverOrigin+scale(i*1000*MS+(follower?MUL_FOLLOWER_SLOT_DTU+followerPath:referencePath),receiverPpb))};
   memcpy(r.legacyFrame,frame,n);char line[MUL_TLM_LINE];if(!mulTelemetryEncode(line,sizeof(line),&r))return 1;puts(line);
  }
 }
 return 0;
}
