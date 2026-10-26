#include "mul_telemetry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv) {
 (void)argv;
 mulTelemetry_t r={.type=mulTlmPlanRange,.quality=4,.radio=2,.remoteRadio=1,.mode=4,.role=2,.node=141,.peer=33,.flags=9,.epoch=5,.transaction=UINT32_MAX,.uptimeMs=UINT32_MAX-20,
  .planRange={.cycle=UINT32_MAX,.fingerprint=0x12345678,.revision=7,.reportedMs=10,.initiator=33,.responder=141,.operation=31,.replyMs=5,.rawTof=2131,.tof=2131,.unknownComponents=255,.source=1,.version=1}};
 for(unsigned i=0;i<6;i++)r.stamps[i]=UINT64_C(0xffffffffff)-i;
 for(unsigned i=0;i<3;i++)r.planRange.power[i]=(mulPower_t){.powerCentiDbm=INT16_MIN,.status=mulPowerUnsupported};
 char line[MUL_TLM_LINE];mulTelemetry_t d;
 assert(mulTelemetryEncode(line,sizeof(line),&r)&&mulTelemetryDecode(line,&d));assert(d.planRange.rawTof==2131&&d.uptimeMs==r.uptimeMs&&d.planRange.operation==31&&!memcmp(r.stamps,d.stamps,sizeof(r.stamps)));
 if(argc>1)puts(line);
 mulTelemetry_t bad=r;bad.planRange.unknownComponents=0;assert(!mulTelemetryEncode(line,sizeof(line),&bad));bad=r;bad.planRange.tof++;assert(!mulTelemetryEncode(line,sizeof(line),&bad));
 bad=r;bad.planRange.responder=142;assert(!mulTelemetryEncode(line,sizeof(line),&bad));bad=r;bad.flags|=mulTlmCalibration;assert(!mulTelemetryEncode(line,sizeof(line),&bad));
 r=(mulTelemetry_t){.type=mulTlmPlanQuality,.quality=4,.radio=2,.remoteRadio=1,.mode=5,.role=2,.node=141,.peer=255,.epoch=5,.attempts=10,.completed=4,.aborted=6,.rejected=8,.timeouts=3,
  .planQuality={.fingerprint=0x12345678,.revision=7,.cycle=UINT32_MAX,.lastReason=mulArDeadline,.operation=31,.stage=mulTwrFinal,.enabled=1,.reference=1,.reset=4}};
 assert(mulTelemetryEncode(line,sizeof(line),&r)&&mulTelemetryDecode(line,&d));assert(d.attempts==10&&d.planQuality.reset==4&&d.planQuality.lastReason==mulArDeadline);if(argc>1)puts(line);
 r.planQuality.pending=1;assert(!mulTelemetryEncode(line,sizeof(line),&r));
 if(argc==1)puts("AR telemetry13/14: fixed152 bytes, mixed-clock raw stamps/power, initiator ToF, explicit unknown uncertainty, acquired/reported wrap, strict identity/flags and reset-tagged quality passed");
 return 0;
}
