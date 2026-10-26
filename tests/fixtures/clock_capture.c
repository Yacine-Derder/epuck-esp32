#include "mul_telemetry.h"
#include <stdio.h>
int main(void) {
 mulTelemetry_t r={.type=mulTlmClock,.node=141,.radio=2,.role=2,.mode=5,.epoch=77,.exchange=9,.uptimeMs=50000,.transaction=200,.captureAgeMs=16,
 .clock={.reference=132,.referenceTx=UINT64_C(0xffffffffff),.rawRx=1,.referenceSequence=UINT32_MAX,
 .appliedPpb=-15,.fittedPpb=-38,.rmsDtu=4052,.fitSigmaPpb=UINT32_MAX,.ageMs=2,.rejected=7,
 .method=1,.window=24,.count=24,.inliers=22,.status=2,.ready=1,.fallback=2}};
 char line[MUL_TLM_LINE];if(!mulTelemetryEncode(line,sizeof(line),&r))return 1;puts(line);return 0;
}
