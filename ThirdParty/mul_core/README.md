# Portable MUL core API 1

Canonical source: `Yacine-Derder/lps-node-firmware:portable/mul_core`.
LGPL-3.0-or-later, as the extracted MUL implementation. Only edit this canonical
folder. STM32 carries a deterministic committed vendor snapshot. No remote or
package manager is added. Firmware builds require no sibling checkout/network.

`mul_common.h/.c` preserve the legacy34-byte type1 codec, complete ALR identities,
sequence state and signed modular40-bit helpers byte-for-byte. Their existing
void encoder/arithmetic pointer preconditions remain unchanged. LPS `inc/src`
forwarding files preserve build/test discovery. Clock/relation/TWR/correction/
localization records stay in their established LPS modules; no lifecycle or
observation fields are redefined here.

`mul_core.h/.c` add status-returning bounds-checked diagnostic encode/decode,
full84/85/86 source validation, resettable sequence accounting and caller-owned
observation construction. Null/bounds/format failures do not modify decoder
output. Counters wrap uint32; forward ordering is unambiguous below2^31.
Duplicates/old frames count as valid but do not advance the baseline or undo gaps.
Reset explicitly after a known boot/session change; packets have no boot epoch.
No allocation, logging, drivers or RTOS. Fixed scan lengths, no unbounded loops.
Source ABI only: padding/struct sizes are not a wire or binary ABI.

## Diagnostic v1 wire format

44 bytes excluding2-byte hardware FCS. This is a NEW type0x70, not legacy
broadcast type1, relation type2 or TWR. Stage3E decoders reject it and remain
unchanged. No mixed-radio clock/range/TDoA/correction availability is implied.
All multibyte fields little endian; no struct overlays.

| Offset | Size | Meaning |
|---|---:|---|
|0–20|21|Legacy extended MAC:41 dc, low sequence byte, PANcf bc, broadcast8xff, source8 bytes|
|21–23|3|ASCII MUL|
|24|1|Diagnostic type0x70|
|25|1|Version1|
|26–29|4|Current sequence32, resets0, wraps modulo2^32|
|30|1|Previous completed TX marker valid:0/1; other bits rejected|
|31|1|Timestamp width40|
|32|1|Unit1: nominal1/(499.2MHz*128) DTU, not calibrated physical SI time|
|33|1|Sender radio family:1 DW1000,2 DW3000; full source identifies the clock domain|
|34–37|4|Sequence of the reported previous completed TX|
|38–42|5|Previous actual hardware TX marker, source clock, full40 bits|
|43|1|Deterministic pattern0xa5|

Absent marker requires both marker fields zero. Valid marker must precede the
current sequence by1..2^31-1; zero timestamp can be valid. It need not be the
immediate predecessor if a TX completion failed. Immediate transmission cannot
embed its own future actual timestamp: first frame has no marker, completion
is logged locally and next frame carries that measured marker and its sequence.
Hardware TX values include configured antenna adjustment; physical calibration
is unverified. RX observations preserve raw hardware40-bit receiver markers,
receiver identity/family, message validity and unit/width contract. Application
power correction is not applied. Never subtract clocks of different sources.
Exactly-half-period signed differences map to-2^39; use only within half-period.
Encode requires capacity>=44; decode requires length EXACTLY44. Strict checks
cover MAC/destination/PAN/magic/type/version/flags/width/units/family/source and
marker ordering. Adapters additionally require the expected peer identity/family
and hardware CRC-good, and own sequence-baseline lifetime.

## Platform boundary and tests

Platform owns PHY, CRC/FCS, SPI/IRQ, timestamp capture, scheduling/rearm, storage,
monotonic host-ms timers and transport. The core never schedules the radio.
Use `sh portable/mul_core/tests/run.sh` in LPS or
`sh ThirdParty/mul_core/tests/run.sh` in STM32 for sanitizer/golden/20000-input
malformed tests. Same sources compile on host/Cortex-M0/Cortex-M4.

## Pinned export/update

Publish canonical changes to LPS origin/master FIRST, then from LPS run:
`python3 tools/mul_core_vendor.py <full-published-LPS-SHA> <empty-destination>`.
Review/replace STM32's `ThirdParty/mul_core` with that export and commit it there.
UPSTREAM.json records commit, canonical path and every file hash. STM32 build
and native core tests verify all files offline and reject drift (also added files).
Run both core tests, cross-repo byte/hash comparison and firmware regressions
before publishing the consumer. Record compatible firmware SHAs in PLAN/handoff.
Fresh clone: committed snapshot is sufficient; standard LPS driver submodule
checkout is still required, no new submodule. Recovery: checkout the matching
firmware commits; do not update a core implicitly during configure/build.

## ADS-TWR foundation (separate protocol v1)

`mul_ads.h/.c` own fixed-buffer serialization, one-peer exchange state and
modular arithmetic. They have no driver, timers, allocation or logging. No
Stage3E or diagnostic v1 bytes change. Initiator and responder can be reversed.

All ADS frames are56 bytes excluding FCS: extended MAC0–20 with complete
unicast destination/source, MUL21–23, type24 (0x71 POLL,0x72 RESPONSE,0x73 FINAL,
0x74 REPORT), version1 at25, nonzero little-endian session64 at26–33,
transaction32 at34–37 (nonzero; low byte also MAC sequence), three40-bit stamps
at38–52, width40/unit1/pattern0xa5 at53–55. Only REPORT has stamps; all other
stamp bytes must be zero. REPORT carries responder t2,t3,t6. Strict decoders
reject unknown types/version/identity/length/unused fields and leave output
unchanged on failure. Hardware FCS protects arbitrary payload corruption.

POLL→RESPONSE→FINAL is the ranging exchange; a fourth timestamp REPORT returns
measured responder times to the initiator, which has measured t1,t4,t5. This
explicit follow-up avoids predicting FINAL TX time. REPORT's own timestamps
are irrelevant to ToF. No unverified intended TX stamp enters the calculation.

State: idle→POLL TX completion→RESPONSE RX→FINAL TX completion→REPORT RX→idle
at initiator; idle→POLL RX→RESPONSE TX completion→FINAL RX→REPORT TX completion
→idle at responder. TX must be taken once and completed before accepting its
reply. Any abort/deadline clears the in-flight state. Per-stage timeout100ms,
unsigned host-ms wrap; adapters retry whole exchanges at1Hz with a NEW ID.
No overlapping exchange, retransmission of individual frames or heap queue.
Matching includes both identities/session/transaction/type/state. Wrong/stale/
duplicate frames do not advance state or extend its deadline. Responder rejects
non-forward transactions within its current session and accepts a new session
only when idle; no persistent replay protection across old sessions/reboots.
Initiator stops at UINT32_MAX until reinitialized with a fresh session.

Adapters require a deliberately chosen fresh nonzero MUL_TWR_SESSION for each
initiator boot/run. Reusing it after a restart can correctly cause rejection;
there is no silent EEPROM update, automatic epoch guess or cryptographic nonce.
Reset both devices or rebuild the initiator with a new session for recovery.

`mulAdsCalculate` uses independent within-device modulo40 differences only.
Intervals must be positive and <=100ms (<half wrap). Binary64 conversion precedes
products, which can exceed uint64; bounded cancellation error is <0.00001 DTU.
The standard formula is (Ra*Rb-Da*Db)/(Ra+Rb+Da+Db). Responder/initiator rate
ratio=(Rb+Db)/(Ra+Da) must lie within +/-1000ppm. For constant clock rates fa,fb
and reciprocal fixed propagation p seconds, output equals 2*fa*fb/(fa+fb)*p;
it is nominal mixed-clock DTU, not automatically reference DTU or calibrated SI.
Constant offsets cancel; changing clock rate, motion, antenna bias and multipath
do not. Fractional and negative results are retained to expose bias. Outputs are
unchanged on failure. No cross-platform bias table or arbitrary offset is added.

Counters wrap uint32. `attempts` counts begun/accepted POLLs; initiator completed
means all six timestamps passed arithmetic checks, responder completed means
REPORT TX completed (not proof initiator received it). `aborted` counts active
exchange cancellations and `rejected` counts decoded but unaccepted packets.
Decode failures are separate adapter `invalid`; PHY errors/timeouts are separate
from delivery. No single counter proves physical accuracy or packet delivery.

Diagnostic sequence `valid` still counts every decoded expected-peer frame,
including duplicates/old. Accepted count is valid-old-duplicates modulo2^32.
`mulCoreSequenceBeginEpoch` discards only the ordering baseline and retains
cumulative counters; source/session lifetimes belong to callers. Diagnostic v1
has no boot identity. Adapters log an idle epoch after >=5s without a decoded
expected peer, never infer losses across that boundary; rapid restarts remain
ambiguous and require receiver reset. RX `order` is0 first,1 forward,2 gap,
3 duplicate,4 old. It makes decode success versus acceptance explicit.

### Measurement validity and classified failure diagnostics

The ADS equation,56-byte wire format and six timestamp ownerships are unchanged.
`completed` still counts mathematically complete initiator exchanges, including
raw negative ToF, and responder REPORT TX completion. `rangeValid` is true only
for finite nonnegative ToF; `invalidRanges` separately counts negative computed
measurements. No raw ToF is clamped/replaced. Zero and small positive values are
retained; this validity flag does not establish calibration or physical accuracy.
A plausible maximum distance is an explicit offline experimental selection rule,
not an invented universal range limit or calibration constant in firmware.

`mulAdsAbortWithReason` records cause and the failed stage before clearing active
state. Causes:0 explicit cancel,1 deadline/missing completion,2 radio error,
3 RX frame-wait timeout,4 TX start/write failure,5 encoding failure,6 timestamp
ordering/interval/clock-envelope/math rejection. `failedStages[0..6]` correspond
to stages1 POLL_TX,2 RESPONSE_RX,3 RESPONSE_TX,4 FINAL_RX,5 FINAL_TX,6 REPORT_RX,
7 REPORT_TX. They saturate at65535; existing uint32 counters retain modular wrap.
Idle radio errors/timeouts do not count as aborted exchanges or failed stages.
The104-byte portable state plus DW adapter fits the existing128-byte scratch.

Adapters log session on TIMES and STAT, range_valid on RESULT, cumulative
bad_range on STAT, and a compact ABORT with cause/stage/cumulative by_stage
counts only on active cancellation. This is diagnostic metadata, not a packet
version change. Associated malformed measurement candidates can still emit
TIMES before a math abort. `MUL_ADS_TRACE_TIMES=0` disables six-stamp logging
(default1); keep it enabled for research captures. DWM optional MUL_TIMING_TRACE
now also supports ADS, reporting MULH TIME after startup. Maxima do not identify
the instant of a UART overrun or establish interrupt WCET.


ADS-TWR addressing: the complete source/destination extended address is
`ID:00:00:00:00:00:CF:BC`, with ID any byte 0x00..0xFF. No ID is reserved;
0xFF here is not the all-FF broadcast MAC. Source must differ from destination.
Deployments must provision unique IDs; the protocol does not discover collisions.
This widening applies only to ADS; legacy observation/diagnostic source policies,
wire length/version and timestamp arithmetic remain unchanged. Existing ADS
adapters still select their configured local/peer; old restricted firmware must
be updated to communicate with peers outside its former allowlist.
