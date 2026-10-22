/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Host-only ABI boundary. No copy of receiver math, no hardware dependencies. */
#include "mul_hybrid.h"
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
void *hybrid_new(unsigned node,unsigned stationary) {
 mulHybrid_t *s=calloc(1,sizeof(*s));if(!s)return NULL;
 uint8_t id[8]={(uint8_t)node,0,0,0,0,0,0xcf,0xbc};mulHybridInit(s,id,stationary!=0);return s;
}
void hybrid_free(void *s){free(s);}
int hybrid_feed(void *context,const uint8_t *frame,unsigned length,uint64_t rx,uint32_t captureAge,uint32_t now,char *out,unsigned capacity) {
 mulHybrid_t *s=context;mulHybridOutput_t r;
 if(!mulHybridReceive(s,frame,length,rx,captureAge,now,&r))return 0;
 char tdoa[40]="null",difference[40]="null",rate[40]="null";
 char txSep[40]="null",rxSep[40]="null",referenceTx[40]="null",broadcastTx[40]="null";
 if(r.broadcast)snprintf(broadcastTx,sizeof(broadcastTx),"\"%010" PRIX64 "\"",r.packet.txTimestamp);
 if(r.associated&&r.observation.clockValid) {
   mulPair_t pair={.reference=s->observations[0],.follower=s->observations[1]};int64_t dt;
   if(mulDownlinkRelation(&r.relation,&pair,now,captureAge,&dt)) {
     snprintf(txSep,sizeof(txSep),"%" PRId64,dt);
     snprintf(rxSep,sizeof(rxSep),"%" PRId64,mulTimestampDifference(pair.follower.source.rxTimestamp,pair.reference.source.rxTimestamp));
     snprintf(referenceTx,sizeof(referenceTx),"\"%010" PRIX64 "\"",pair.reference.source.txTimestamp);
   }
 }
 if(r.associated&&r.observation.status==mulDownlinkAvailable){snprintf(tdoa,sizeof(tdoa),"%" PRId32,r.observation.tdoaDtu);snprintf(difference,sizeof(difference),"%" PRId32,r.observation.rangeDifferenceMm);}
 if(r.associated&&r.observation.rateState==mulRateStationary)snprintf(rate,sizeof(rate),"%" PRId32,r.observation.ratePpb);
 int n=snprintf(out,capacity,"{\"broadcast\":%s,\"associated\":%s,\"reference\":%u,\"follower\":%u,\"reference_sequence\":%" PRIu32 ",\"follower_sequence\":%" PRIu32 ",\"reference_generation\":%" PRIu32 ",\"follower_generation\":%" PRIu32 ",\"follower_acquisition_epoch\":\"%016" PRIX64 "\",\"clock_valid\":%s,\"status_code\":%u,\"tdoa_dtu\":%s,\"range_difference_mm\":%s,\"receiver_rate_ppb\":%s,\"twr_complete\":%s,\"twr_raw_tof_dtu\":%" PRIu32 ",\"twr_corrected_tof_dtu\":%" PRIu32 ",\"twr_rejected\":%" PRIu32 ",\"twr_timeouts\":%" PRIu32 ",\"unknown_components\":%u,\"tx_separation_ref_dtu\":%s,\"rx_separation_local_dtu\":%s,\"reference_tx\":%s,\"broadcast_tx\":%s,\"source_role\":%d,\"source_order\":%u}",
 r.broadcast?"true":"false",r.associated?"true":"false",r.observation.reference[0],r.observation.follower[0],r.observation.referenceSequence,r.observation.followerSequence,
 r.referenceGeneration,r.followerGeneration,r.observation.followerEpoch,r.observation.clockValid?"true":"false",r.associated?r.observation.status:255,tdoa,difference,rate,r.twrComplete?"true":"false",r.twrRaw,r.twrCorrected,s->twrRejected,s->twrTimeouts,r.observation.unknownComponents,txSep,rxSep,referenceTx,broadcastTx,r.broadcast?(int)mulAlrRoleForAddress(r.packet.source):-1,(unsigned)r.order);
 return n>0&&(unsigned)n<capacity?1:-1;
}
