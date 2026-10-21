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
 a.epoch++;assert(mulArrivalUpdate(&m,(mulTimeEvent_t){a,10,true},(mulTimeEvent_t){c,20,true})&&!m.valid&&m.samples==1);
 puts("Infrastructure: telemetry CRC/truncation/width, beacon codec, domain/epoch/wrap/drift/out-of-order tests passed");
}
