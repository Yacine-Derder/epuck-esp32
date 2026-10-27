# Portable MUL core API 1

Canonical source: `Yacine-Derder/lps-node-firmware:portable/mul_core`.
LGPL-3.0-or-later, as the extracted MUL implementation. Only edit this canonical
folder. STM32 carries a deterministic committed vendor snapshot. No remote or
package manager is added. Firmware builds require no sibling checkout/network.

`mul_common.h/.c` preserve the legacy34-byte type1 codec, complete ALR identities,
sequence state and signed modular40-bit wire helpers. Their existing
void encoder/arithmetic pointer preconditions remain unchanged. LPS `inc/src`
forwarding files preserve build/test discovery. Legacy clock/relation/TWR/observation/localization APIs now forward to shared
radio-independent implementations here; their record semantics and DW1000
scheduler remain unchanged. Hardware RX power capture stays in LPS.

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


## Custom TDMA hybrid receiver

`mul_clock`, `mul_relation`, `mul_observation`, `mul_downlink` and the pure part of
`mul_localization` are extracted original implementations, not an alternative
protocol. `mul_legacy_twr` is the exact84-byte inter-ALR codec/reference-unit math;
LPS retains its hardware state machine. `mul_power_types` contains metadata only,
without linking DW1000 bias tables into DWM3000. `mul_legacy_timing` avoids the
STM32 CPU-tracing header's name and retains500/50/10ms compile-time slots.

`mul_hybrid` composes original passive-RLR association/rate/mapping behavior and
observes (never answers) directed inter-ALR TWR. Current full identities come from
the legacy protocol's roster; they are not a limitation of the generic host solver.
Receiver epochs/generations must not be confused with the follower's acquisition-
local epoch. No globally unique ALR boot UUID exists in these packets. Stationary
rate opt-in remains conditional; default behavior refuses geometric TDoA without
an identifiable receiver rate. Physical antenna/power/motion uncertainty remains
unknown. Details and compatibility matrix: docs/mul_heterogeneous.md in the root repo.

Telemetry v3 retains152 bytes and309 printable characters before newline, with
CRC16. Types1..5 retain their existing layouts; old modes encode v2. Type6 stores
an untouched34/74/84-byte legacy frame at40..123, local RX40 at124, length at129,
capture-age16 at130, inferred reference/follower generations32 at132/136,
rate policy8 at140, slot16 at141, PHYerrors32 at143 and reserved zeros147..149.
Common header0..39 retains local radio epoch and sequence. These records do not
contain range-counter fields in their payload; ordinary summaries supply those.
V1/v2 remain readable. A captured frame's TX remains in its source's own domain.


## Explicit legacy TDMA roster (checkpoint C foundation)

`mulNetwork_t` binds ordered full reference/follower identities. Network APIs
accept an explicit roster; NULL retains the default0x84/0x85 network. A supplied
invalid roster fails closed. IDs0..254 with canonical full suffix are supported;
duplicate participants and broadcast255 are rejected. `mulHybridInitNetwork`
copies the roster into each receiver; there is no mutable global network state.
Network variants cover TWR endpoints, relation decoding/publication, clock delay,
observation association and metric validation. Legacy APIs retain default behavior.
Frame lengths, bytes, timing policies, rate admission and math are unchanged.
The two-participant protocol remains; this adds configuration, not extra slots.

Sanitized tests interleave independent swapped/custom rosters across500/50/10ms,
check publication/clock/geometry and NO_RATE, reconstruct complete passive TWR,
and reject wrong destinations, spoofed full IDs, duplicate/self/broadcast roles.
Platform persistence and commands are subsequent integration work; these APIs
alone do not make board roles runtime configurable or validate physical operation.

### Onboard RLR TDoA telemetry (UWB3 type7)

This additive record remains152 bytes (309 ASCII bytes,310 with newline), with
unchanged common header0..39 and CRC16 at150..151. It contains the actual
`mulLocalizationObserveNetwork` result, not a new estimator or an ALR-ALR range.
Existing types1..6 retain their layouts and raw diagnostic frames.

|Bytes|Type7 meaning (little endian)|
|---|---|
|40..47|Reference/follower sequences, u32 each|
|48..59|TDoA DTU, distance difference mm, receiver rate ppb, i32 each|
|60..65|Reference ID, follower ID, status, clock-valid, rate-state, stationary policy|
|66..70|Native40 follower RX timestamp in receiver domain|
|72..79|Combined STM USB/Wi-Fi queue drops and UART errors, u32 each|
|80..87|Reference/follower acquisition generations, u32 each|
|88..95|Queue depth/high, capture age ms, slot ms, u16 each|
|104..114|Delay age u32, delay DTU/clock age u16, delay-state/unknown-components/power flags u8|
|118..129|Radio errors, timeouts, UART telemetry TX drops, u32 each|

Bytes71,96..103,115..117,130..149 must be zero. Header epoch identifies the local
boot/radio session; exchange carries the follower relation epoch. Header uptime
is the observation's coarse follower-capture uptime, not host time. Status0..4
is no_relation/no_rate/stale_rate/inconsistent/available. Rate-state0..2 is
unavailable/stationary/stale. Available requires stationary mode5, valid clock
and rate, and non-sentinel results. Otherwise both results must be INT32_MIN;
consumers expose null, never a sentinel distance. Physical uncertainty remains
UNKNOWN even for an available stationary result. Native timestamps preserve40-bit
wrap; sequences remain independent32-bit counters.

Old ESP firmware rejects unknown type7; update both consumers. New decoders keep
reading historical UWB1/2/3 types. No record size, UART queue or PHY change is
required. Emission is bounded/best effort; transport drop counters remain relevant.

For an authorized local-only integration, the exporter supports explicit
`--allow-local REV DEST` to export a committed unpublished source without any
push/fetch. The default still requires origin/master ancestry. UPSTREAM.json pins
the exact commit and file hashes in either case; unpublished pins are available
from the local canonical repository until the owner authorizes publication.

## Deterministic ranging planner (adapter integration pending)

`mul_ranging_plan.[ch]` supplies a bounded, stateless planner for reference-to-
follower AA and every ALR-to-RLR pair, with up to8 ALRs/8 RLRs. Per-pair targets
use integer milliHz; disabled types create no operations. Supported candidate reply
intervals are20/10/5ms, subject to explicit lead+processing+airtime+guard feasibility.
An aggregate integer quota distributes fractional requests across cycles; weighted
AA/AR selection and pair rotation bound class fairness to one operation. Within a
cycle, reservations spread over the usable interval after broadcast data and before
the20ms next-reference preparation guard. Peak-cycle demand must fit the selected
10..80% utilization cap (preview default70%) and32-operation implementation bound.
Infeasible rates fail closed. Cycle32 rollover reconstructs the same reset quota
phase on every participant; it is not an extended64-bit schedule epoch.

The budget includes initial setup, four replies, final RESULT airtime/processing and
guard. This is a conservative software model, not a measured radio turnaround bound.
It provides deterministic ownership and intervals; actual TX/timeout/cancellation,
configuration persistence and scheduled RLR participation remain to be connected.
Do not enable this planner alongside the existing fixed AA window owner.

Its separate64-byte codec v1 carries outer header0..3, verified broadcast schedule
4..35, RLR count36/IDs37..44, reserved45, AA/AR target milliHz46..49, reserved50..59,
and CRC60..63. The broadcast timing subrecord has AA flags cleared in preview:
the planner owns ranging flags separately and does not activate existing windows.
This codec is not yet an on-radio frame or an EEPROM schema. Existing firmware
continues using its current pinned schedule codecs and fixed maintenance windows.

Read-only production-C preview:

```sh
python3 tools/mul_schedule_preview.py tools/examples/mul_ranging_plan.json --cycles 3
```

Output includes the fingerprint, guarded reservation duration/capacity, per-pair
requested rates, per-cycle utilization and each operation's kind/participants/
start/poll/end. `radio_integration:false` explicitly identifies the current scope.
The example's3 AA pairs at0.5Hz and8 AR pairs at2Hz request17.5 exchanges/s;5ms
replies fit its stated budgets, while20ms replies are rejected. Default preview
budgets already differ from conservative live defaults and require later hardware
characterization; the tool never connects to devices.

### Plan EEPROM journal

`mul_plan_store.[ch]` stores the full64-byte ranging plan plus selected-profile
state in two96-byte records at offsets128 and256. It preserves the legacy0..99
region. Layout: magic0..3, version4, selected5, zero6..7, generation8..11,
plan12..75, CRC76..79, zero80..91, commit92..95. The inactive commit is invalidated,
then the body written, then commit last; readback must match exactly. Generation
comparison handles32-bit rollover and rejects ambiguous/tied conflicting states.
Uncommitted slots are ignored; no valid committed record means absent unless a
committed but invalid record is detected. A valid older slot provides interrupted-
write rollback. A false save after final readback failure may still be committed.

Callbacks require EEPROM byte-rewrite semantics, bounded synchronous I/O and
exclusive access to the caller-owned192-byte workspace. The LPS cfg backend shares
its existing writer lock, requires acknowledged RF OFF for physical loads/saves,
and marks storage uncertain on failed save. This is not a NOR-flash journal.
Boot activation and CLI/generated-radio integration remain pending; current
firmware does not scan/select this journal automatically. No physical extension
region is written by current runtime paths. See the engineering handoff before
connecting it to the scheduler; never run fixed and generated owners concurrently.


`mulMultiReceiverInitPlan` optionally reconstructs passive TDoA cycles from the
full-plan RF4 broadcast envelope. The caller retains an immutable plan until
reinitialization; completed cycles carry the full-plan fingerprint. The legacy
initializer still accepts only its prior RF2/3 envelopes. Receiver initialization
does not authorize any RLR transmission. Link `mul_plan_protocol.c` and
`mul_ranging_plan.c` when using `mul_multi_receiver.c`, including legacy users.
Telemetry type8/9 still describes the prior profiles; adapters must add explicit
versioned generated-plan labels before forwarding RF4 observations as telemetry.


Generated-plan telemetry uses new UWB3 types11(vector)/12(raw), with header quality4
required. Payload layouts remain152 bytes and match the existing vector/raw fields,
but fingerprints bind the full plan. Type12 preserves40/80/90 native bytes and
checks RF4 marker/full fingerprint (and0..31 operation index on TWR). Legacy type8/9
accept only prior markers0/3; old decoders reject11/12 as unknown. This is a new
record type rather than reinterpreting legacy10ms timing. Adapters must preserve
cycle observation uptime, prioritize type11, and label profile4 in host logs.


UWB3 type13 is a planned AR result, quality4, receiver radio2/role2, remote radio1,
flags9 (raw valid + verified stamps). Uptime is acquired P2 receiver uptime;
reported uptime is separate. Payload: cycle40/hash44/revision48, pair52/53, op54,
reply55, raw/power-adjusted initiator ToF56/58, report uptime60, six5-byte mixed-clock
stamps64, three5-byte power records94, unknown255/source1/version1 at109..111,
queue/loss metadata112..127 and completion/abort/rejection counters128..139; zero140..149.
P1/P4/P5 belong to the initiator; P2/P3/P6 belong to the receiver. No calibrated SI
range or finite physical uncertainty is implied. Old parsers reject the new type.

Type14 is reset-tagged AR quality, quality4/flags0/peer255. Hash40/revision44/cycle48,
eleven counters52..95 (attempts/completed/aborted/rejected/math-invalid/result-loss/
raw-loss/late/radio-errors/timeouts/reset-generation), active/pending/reference96..98,
zero99, last-reason100/op101/stage102/enabled103/version1 at104; zero105..149.
Reason enums are explicit, counts are per receiver session/reset generation, and
busy reset must not erase transaction identity. Both records remain152 bytes.


Full-plan codec2 remains byte-for-byte compatible for policy fair (zero priorities).
Opt-in codec3 retains64 bytes and uses50=policy1 freshness,51=AA priority,52=AR
priority;53..59 stay zero. CRC/fingerprint cover all settings. Old decoders reject
codec3; full RF4 plan hashes prevent mixed ownership. Journal and radio envelopes
retain their sizes. `mulRangingPlanVersion` reports the actual selected codec.

Freshness merges independent AA/AR virtual-deadline streams derived from requested
per-pair rates and reference cycle. Equal deadlines use the larger class priority
(0..255), then AA on equality. Pairs rotate fairly within each class; opportunity
times spread across the usable interval. Independent coincident deadlines reserve
the sum of both class ceilings, with the same setup/airtime/utilization limits.
It never overbooks to honor a priority, and it never promises achieved success.

Freshness is planned-reservation age, not distributed feedback about measurement
success. Packet loss cannot change a local participant's slot ownership. For a
nonzero per-pair target r milliHz, the nominal period is1000000/r ms, with cycle
quantization; a conservative planned gap allowance is that period plus two cycles.
Actual committed age/validity remains explicit and may be unbounded after loss.
No adaptive rescheduling or measurement-noise claim follows from this policy.


### Portable external pose and propagation input

`mul_pose` is source-agnostic and has no transport/ROS/MCS/noise-generation code.
Pose version1 is80 little-endian bytes:0version,1node,2flags,3zero;4clock32,8epoch32,
12frame32,16sequence32,20source timestamp64 microseconds;28position3×signed32mm;
40covariance6×signed32mm² orderedxx,xy,xz,yy,yz,zz;64ageUpperMs16,66TTL16,
68timeUncertaintyMs16;70..75zero;76CRC32 of0..75. Flags1position-valid,2covariance
provided,4cross-robot position-noise independence explicitly declared. Unknown
age/time uncertainty=65535. Clock/frame IDs must be nonzero. Positions are bounded
to±1000000mm; covariance components to±1e9mm², with nonnegative diagonals and PSD
checks (normalized determinant relative tolerance1e-12). TTL1..6000ms.

Source timestamp/clock/epoch and common frame are distinct from local reception
uptime. The supplied age upper bound must cover observation age at local
acceptance INCLUDING transit/alignment uncertainty; UNKNOWN cannot correct a
clock. Nominal source timestamp skew is checked separately (policy0..1000ms).
Unknown timestamp uncertainty never becomes a physical simultaneity guarantee.
Stream begin explicitly clears old observations and binds clock/epoch/frame.
Duplicate/old/half-period-ambiguous sequence numbers and repeated valid timestamps
are rejected without refreshing age. Invalid observations explicitly withdraw a
pose. Max8 identities, no eviction that could silently replace another robot.

Geometry uses immutable snapshots, Euclidean3D distance and nominal reference-DTU
conversion. First-order variance projects both supplied full3D covariance matrices
only when both observations declare independent cross-robot noise; otherwise it
is UNKNOWN. Co-located positions also leave linearized variance unknown. This
statistical variance is never substituted for a hard bound. Delay objects retain
UNKNOWN measurement/motion physical bounds; clock scale remains uncalibrated.
`mulPoseDelay` validates ordered identities and binds to the CURRENT clock epoch,
while preserving the geometry acquisition age. Source timestamps never determine
a hardware delayed-TX target. Existing mapping and regression APIs accept this
delay object; no fallback to pristine truth or TWR is implicit.

Relation subversion3 retains74-byte base/80-byte multi body lengths, with source
external1/none2 in flag bits6..7; TWR retains exact v1/2 bytes. External/none cannot
claim power-corrected TWR. External TTL can be1..MUL_PROP_MAX_AGE_MS; propagation
state fresh/reused is based on age versus broadcast period, not a fabricated TWR
completion sequence. Field48 is the reference-sequence binding for external
geometry, not a TWR transaction. None/missing external input stays unavailable.
Old decoders reject3. Relation remains48 bytes; no inactive-state overlay added.

Source-aware vector payload subversion1 uses byte149=1 and packed follower bits
6..7:0missing,1TWR,2external,3none. Seen entries require nonzero provenance; unseen
entries require0. Subversion0 retains all old bytes/zero reserved bits. TDoA7
subversion1 uses115=1,116source external1/none2,117zero; old TWR zero bytes remain
unchanged. These explicit markers make older strict decoders reject new semantics
instead of labeling geometry as TWR. Raw radio copies preserve source/version too.
The UWB3 envelope stays152 bytes; all physical uncertainty remains UNKNOWN.


Pose banks provide periodic expiry latching: expired entries retain identity,
sequence and source timestamp but replace local TTL with a zero tombstone. Zero
is forbidden on wire. They cannot revive at a full local uptime wrap or after
pair retargeting; only a newer valid source observation can replace them. Call
`mulPoseStreamExpire` regularly, before a full uint32 local-time wrap. Runtime
cache queries also latch expired status. Pure snapshot geometry cannot infer
multiple wraps from a32-bit timestamp alone; periodic bank service is required.
