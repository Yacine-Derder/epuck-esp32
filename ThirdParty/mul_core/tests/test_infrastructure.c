#include "mul_telemetry.h"
#include "mul_beacon.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
int main(void){
 assert(mulTelemetryCrc((const uint8_t *)"123456789",9)==0x29b1);
 mulTelemetry_t r={.type=mulTlmRange,.node=0,.peer=255,.radio=2,.mode=1,.role=1,.epoch=UINT64_MAX,.exchange=1,.rawMm=INT32_MIN,.calMm=INT32_MAX,.flags=31,.stamps={MUL_TIMESTAMP_MASK,1,2,3,4,5}},d;
 char line[MUL_TLM_LINE];assert(mulTelemetryEncode(line,sizeof(line),&r));assert(mulTelemetryDecode(line,&d));assert(d.rawMm==INT32_MIN&&d.calMm==INT32_MAX&&d.epoch==UINT64_MAX&&d.stamps[0]==MUL_TIMESTAMP_MASK);
 for(unsigned i=0;i<309;i++){char old=line[i];line[i]='?';assert(!mulTelemetryDecode(line,&d));line[i]=old;}
 for(unsigned i=0;i<309;i++){char old=line[i];line[i]=0;assert(!mulTelemetryDecode(line,&d));line[i]=old;}
 /* Decode a genuine shorter v1 record, not a v2 prefix alias. */
 uint8_t old[128]={0};for(unsigned i=0;i<126;i++){unsigned v;assert(sscanf(line+5+2*i,"%2x",&v)==1);old[i]=(uint8_t)v;}
 old[2]=1;uint16_t crc=mulTelemetryCrc(old,126);old[126]=(uint8_t)crc;old[127]=(uint8_t)(crc>>8);
 char v1[262];memcpy(v1,"UWB1 ",5);for(unsigned i=0;i<128;i++)snprintf(v1+5+2*i,3,"%02X",old[i]);
 assert(mulTelemetryDecode(v1,&d)&&d.stamps[0]==MUL_TIMESTAMP_MASK&&d.rawMm==INT32_MIN&&d.radioErrors==0&&d.remoteEpoch==0);
 mulBeacon_t b={.source=255,.radio=2,.epoch=123,.sequence=0,.previousSequence=UINT32_MAX,.previousTx=MUL_TIMESTAMP_MASK,.markerValid=true},bd;uint8_t f[56];
 assert(mulBeaconEncode(f,sizeof(f),&b));assert(mulBeaconDecode(f,sizeof(f),&bd)&&bd.epoch==123&&bd.previousTx==MUL_TIMESTAMP_MASK);
 for(unsigned i=0;i<56;i++){f[i]^=0x80;if(i!=13&&i<28)assert(!mulBeaconDecode(f,sizeof(f),&bd));f[i]^=0x80;}
 mulClockDomain_t a={1,1,10},c={2,2,20};mulArrivalModel_t m={0};
 uint64_t start=MUL_TIMESTAMP_MASK-1000000;for(unsigned i=0;i<20;i++){
  mulTimeEvent_t tx={a,mulTimestampMask(start+i*MUL_TIME_HZ),true};
  mulTimeEvent_t rx={c,mulTimestampMask(900000+(uint64_t)llround(i*MUL_TIME_HZ*1.00001)),true};
  assert(mulArrivalUpdate(&m,tx,rx));
 }
 assert(m.valid&&fabs(m.rate-1.00001)<1e-10);uint64_t mapped;
 assert(mulArrivalMap(&m,(mulTimeEvent_t){a,m.sourceAnchor,true},&mapped)&&mapped==m.targetAnchor);
 int64_t delta;assert(!mulTimeDelta((mulTimeEvent_t){a,100,true},(mulTimeEvent_t){c,90,true},&delta));
 assert(!mulArrivalUpdate(&m,(mulTimeEvent_t){a,m.sourceAnchor,true},(mulTimeEvent_t){c,m.targetAnchor,true}));
 assert(!mulTimeDelta((mulTimeEvent_t){a,100,false},(mulTimeEvent_t){a,90,true},&delta));
 assert(!mulTimeDelta((mulTimeEvent_t){a,MUL_TIMESTAMP_MASK+1,true},(mulTimeEvent_t){a,90,true},&delta));
 assert(!mulTimeDelta((mulTimeEvent_t){a,MUL_TIME_MAX_DELTA+1,true},(mulTimeEvent_t){a,0,true},&delta));
 a.epoch++;assert(mulArrivalUpdate(&m,(mulTimeEvent_t){a,10,true},(mulTimeEvent_t){c,20,true})&&!m.valid&&m.samples==1);
 r=(mulTelemetry_t){.type=mulTlmHybridRx,.node=33,.peer=0x84,.radio=2,.mode=4,.role=2,.epoch=123,
 .legacyLength=84,.legacyRx=MUL_TIMESTAMP_MASK,.captureAgeMs=15,.legacySlotMs=50,.referenceGeneration=7,.followerGeneration=8,.ratePolicy=1};
 for(unsigned i=0;i<84;i++)r.legacyFrame[i]=(uint8_t)i;
 assert(mulTelemetryEncode(line,sizeof(line),&r)&&!strncmp(line,"UWB3 ",5));
 assert(mulTelemetryDecode(line,&d)&&d.legacyLength==84&&d.legacyRx==MUL_TIMESTAMP_MASK&&d.captureAgeMs==15&&d.referenceGeneration==7&&d.legacySlotMs==50&&d.ratePolicy==1&&!memcmp(r.legacyFrame,d.legacyFrame,84));
 r.legacyLength=85;assert(!mulTelemetryEncode(line,sizeof(line),&r));
 r=(mulTelemetry_t){.type=mulTlmTdoa,.node=141,.peer=133,.radio=2,.mode=5,.role=2,.epoch=123,.exchange=77,
  .ratePolicy=1,.legacySlotMs=500,.tdoa={.reference=132,.follower=133,.referenceSequence=UINT32_MAX,.followerSequence=0,
  .rx=MUL_TIMESTAMP_MASK,.status=4,.clockValid=1,.rateState=1,.tdoaDtu=-28,.rangeDifferenceMm=-131,.ratePpb=-1234}};
 assert(mulTelemetryEncode(line,sizeof(line),&r)&&strlen(line)==309&&!strncmp(line,"UWB3 ",5));
 assert(mulTelemetryDecode(line,&d)&&d.tdoa.tdoaDtu==-28&&d.tdoa.rangeDifferenceMm==-131&&d.tdoa.referenceSequence==UINT32_MAX&&d.tdoa.followerSequence==0&&d.tdoa.rx==MUL_TIMESTAMP_MASK);
 for(unsigned i=0;i<309;i++){char c=line[i];line[i]=0;assert(!mulTelemetryDecode(line,&d));line[i]=c;}
 r.tdoa.rangeDifferenceMm=INT32_MIN;assert(!mulTelemetryEncode(line,sizeof(line),&r));
 r.mode=4;r.ratePolicy=0;r.tdoa.rateState=0;r.tdoa.ratePpb=INT32_MIN;r.tdoa.tdoaDtu=INT32_MIN;
 for(unsigned status=0;status<4;status++){r.tdoa.status=status;assert(mulTelemetryEncode(line,sizeof(line),&r)&&mulTelemetryDecode(line,&d)&&d.tdoa.status==status&&d.tdoa.rangeDifferenceMm==INT32_MIN);}
 r.tdoa.status=4;assert(!mulTelemetryEncode(line,sizeof(line),&r));
 puts("Infrastructure: telemetry CRC/truncation/width, beacon codec, domain/epoch/wrap/drift/out-of-order tests passed");
}
