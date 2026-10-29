#include "mul_clock_fit.h"
#include "mul_relation.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DT INT64_C(63897600000)
static void observation(mulClock_t *clock, mulClockFit_t *fit,
                        mulClockFitConfig_t *cfg, unsigned seq, uint64_t tx,
                        uint64_t rx, uint32_t now) {
  mulTestPacket_t p = {.sequence = seq, .txTimestamp = tx};
  memcpy(p.source, mulDefaultNetwork.addresses[0], 8);
  mulClockObserveFitted(NULL, clock, fit, cfg, &p, rx, now, true, NULL);
}
static void scenario(unsigned method, unsigned mode) {
  mulClock_t clock;
  mulClockReset(&clock);
  mulClockFit_t s;
  mulClockFitReset(&s);
  mulClockFitConfig_t c;
  mulClockFitDefaults(&c);
  c.estimator = method;
  int64_t tx = MUL_TIMESTAMP_MASK - DT / 2, rx = tx - 100000, phase = 0;
  unsigned now = UINT32_MAX - 2000, seq = 0, applied = 0;
  for (unsigned i = 0; i < 500; i++) {
    unsigned n = mode == 3 && i % 17 == 0 ? 2 : 1;
    double skew = mode == 1 ? -35000 : 15000;
    if (mode == 2)
      skew += .2 * i;
    if (mode == 4)
      skew += sin(i / 30.) * 30;
    if (mode == 5 && i > 250)
      skew += 150;
    int64_t step = n * DT;
    tx += step;
    phase += (int64_t)llround(step * skew / 1e9);
    rx = tx - 100000 + phase;
    int64_t noise = mode == 6 && i == 200 ? 3000 : 0;
    now += n * 1000;
    seq += n;
    observation(&clock, &s, &c, seq, mulTimestampMask(tx),
                mulTimestampMask(rx + noise), now);
    if (i > 450) {
      assert(clock.status == mulClockValid);
      assert(abs(clock.skewPpb - (int)round(skew)) < 20);
    }
    if (method == 2) {
      double x[3], p[3][3];
      assert(mulClockFitPredict(&s, 0, x, p));
      assert(isfinite(p[1][1]) && p[1][1] > 0);
      applied += s.applied;
    }
  }
  if (method == 2)
    assert(applied > 450);
  uint64_t epoch = clock.epoch;
  now += 10000;
  tx += 10 * DT;
  rx += 10 * DT;
  observation(&clock, &s, &c, seq + 10, mulTimestampMask(tx),
              mulTimestampMask(rx), now);
  assert(clock.epoch != epoch && clock.status != mulClockValid);
  for (unsigned i = 1; i < 12; i++)
    observation(&clock, &s, &c, seq + 10 + i, mulTimestampMask(tx + i * DT),
                mulTimestampMask(rx + i * DT + i * DT * 15000 / 1000000000),
                now + i * 1000);
  assert(clock.status == mulClockValid);
  if (method == 2) {
    memset(s.covariance, 0, sizeof(s.covariance));
    s.covariance[0][0] = 1024;
    s.covariance[1][1] = 1e10;
    s.covariance[2][2] = 1e12;
    observation(&clock, &s, &c, seq + 22, mulTimestampMask(tx + 12 * DT),
                mulTimestampMask(rx + 12 * DT + 12 * DT * 15000 / 1000000000),
                now + 12000);
    assert(!s.applied && s.fallback == mulClockFitUncertain &&
           clock.status == mulClockValid);
    s.covariance[1][1] = NAN;
    tx += 13 * DT;
    rx += 13 * DT + 13 * DT * 15000 / 1000000000;
    observation(&clock, &s, &c, seq + 23, mulTimestampMask(tx),
                mulTimestampMask(rx), now + 13000);
    assert(s.resetReason == mulClockResetNumerical);
    assert(!s.applied && s.fallback == mulClockFitNumerical &&
           clock.status == mulClockValid);
    // Corrupt Kalman covariance AND unavailable median: never manufacture
    // validity.
    mulClockReset(&clock);
    mulClockFitReset(&s);
    observation(&clock, &s, &c, 1, 1, 2, 1);
    assert(!s.applied && clock.status != mulClockValid);
    clock.epoch = 0;
    for (unsigned j = 1; j < 8; j++)
      observation(&clock, &s, &c, j + 1, 1 + j * DT, 2 + j * DT, 1 + j * 1000);
    assert(clock.status != mulClockValid && !s.applied &&
           s.fallback == mulClockFitNumerical);
  }
}
// Generated AA maintenance is skipped on cycle0 (AR); no clock observation
// is skipped. A6000ms stationary lifetime must bridge this interval without
// compensation toggles. Strict1000ms policy deliberately reseeds twice.
static void ar_cycle(void) {
  mulClock_t clock;mulClockReset(&clock);
  mulClockFit_t fit;mulClockFitReset(&fit);
  mulClockFitConfig_t c;mulClockFitDefaults(&c);c.estimator=2;c.compensate=1;
  mulClockDelay_t delay={.valid=true,.oneWayDtu=400,.maxAgeMs=MUL_PROP_MAX_AGE_MS,
    .units=mulClockDelayReferenceDtu};
  memcpy(delay.reference,mulDefaultNetwork.addresses[0],8);
  memcpy(delay.follower,mulDefaultNetwork.addresses[1],8);
  for(unsigned seq=1;seq<=30;seq++) {
    uint32_t now=seq*1000;
    // Previous AA completed80ms after its beacon. AR cycles provide no AA.
    if(seq>1&&(seq-1)%10) {
      delay.measuredMs=(seq-1)*1000+80;delay.referenceSequence=seq-1;
    }
    delay.epoch=clock.epoch;
    mulTestPacket_t p={.sequence=seq,.txTimestamp=mulTimestampMask(seq*DT)};
    memcpy(p.source,mulDefaultNetwork.addresses[0],8);
    mulClockObserveFitted(NULL,&clock,&fit,&c,&p,
      mulTimestampMask(seq*DT+1000+seq*10000),now,true,&delay);
    uint64_t arrival=mulTimestampMask(seq*DT+1000+seq*10000);
    uint64_t followerTx=mulTimestampMask(arrival+10*UINT64_C(63897600));
    mulRelation_t relation=mulRelationPublish(&clock,&delay,followerTx,arrival,now,false);
    if(seq>6){
      bool expected=MUL_PROP_MAX_AGE_MS>1000||seq%10!=1;
      assert(relation.available==expected);
      assert(relation.version==(MUL_PROP_MAX_AGE_MS==1000?1:2));
      mulTestPacket_t f={.sequence=seq,.txTimestamp=followerTx,.slotMs=10};
      memcpy(f.source,mulDefaultNetwork.addresses[1],8);
      uint8_t wire[MUL_RELATION_FRAME_LENGTH];mulRelationEncode(wire,&f,&relation);
      mulTestPacket_t decoded;mulRelation_t r;
      assert(mulBroadcastDecode(wire,sizeof(wire),&decoded,&r));
      assert(r.available==expected);
      // A continuous-v2 receiver must still accept a fresh strict-v1 peer.
      if(expected){relation.version=1;relation.maxAgeMs=1000;relation.delayAgeMs=100;relation.remainingMs=800;
        mulRelationEncode(wire,&f,&relation);
        assert(mulBroadcastDecode(wire,sizeof(wire),&decoded,&r)&&r.version==1&&r.available);
      }
    }
    if(seq==1)assert(fit.resetReason==mulClockResetStartup);
    if(seq>10&&seq%10==1) {
      if(MUL_PROP_MAX_AGE_MS==1000)assert(fit.resetReason==mulClockResetCompensation&&!fit.compensated);
      else assert(fit.resets==1&&fit.compensated&&fit.applied);
    }
  }
  assert(fit.resets==(MUL_PROP_MAX_AGE_MS==1000?5u:1u));
}
int main(void) {
  ar_cycle();
  for (unsigned method = 0; method <= 2; method += 2)
    for (unsigned mode = 0; mode < 7; mode++)
      scenario(method, mode);
  mulClockFitConfig_t c;
  mulClockFitDefaults(&c);
  uint8_t b[12];
  assert(mulClockFitConfigEncode(b, &c) && b[0] == 2);
  c.estimator = 2;
  assert(mulClockFitConfigEncode(b, &c));
  assert(mulClockFitConfigDecode(b, &c) && c.estimator == 2);
  b[0] = 1;
  b[1] = 1;
  b[2] = 24;
  b[3] = 8;
  assert(mulClockFitConfigDecode(b, &c) && c.estimator == 0 && c.window == 5);
  b[0] = 2;
  b[2] = b[3] = 5;
  b[1] = 255;
  assert(mulClockFitConfigDecode(b, &c) && c.estimator == 0);
  b[11] = 1;
  assert(!mulClockFitConfigDecode(b, &c));
  printf(
      "Median5/Kalman: constant/ramp/nonlinear/negative skew, variable "
      "intervals/loss, wrap, outliers, step, PSD, outage/reacquisition, "
      "fallback/unavailable and codec migration passed; state=%zu config=%zu\n",
      sizeof(mulClockFit_t), sizeof(c));
}
