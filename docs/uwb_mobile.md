# Passive moving-RLR feasibility (checkpoint B)

Checkpoint A's age/relation defect is physically accepted from the user's retest,
not metric TDoA. STM docs/hybrid_uwb.md retains exact results and residual PHY/TWR
errors. This document defines the moving estimator before implementation; C/D
role/address/command refactors and RLR transmissions are out of scope.

## What is observable

Write reference emission time e, receive time t and propagation tau_A(t):
`t = e + tau_A(t)`. Receiver clock S(t) has rate alpha(t), so a received marker is
`h_A(e) = S(t(e)) + receive_bias`. For fixed affine clocks and constant radial
velocity, `h_A' = alpha/(1 - v_radial/c)`. A timestamp cadence fit measures this
APPARENT reference-arrival rate, not alpha alone. Changing alpha and motion can
produce exactly identical packet timestamps. In particular, put both anchors on
one line behind a receding receiver: motion v and clock alpha are indistinguishable
from a stationary receiver with clock alpha/(1-v/c), for transmissions from BOTH
anchors, including every directed TWR packet. Tests encode this exact example.
In general two apparent slopes cannot determine clock rate plus two velocity
components. Inter-ALR TWR measures the anchors, not robot motion or robot ranges.
A five-sample median or longer fit cannot remove this structural ambiguity.

DW3000 exposes clock-offset/carrier-integrator measurements, but received carrier
frequency also includes Doppler. These are useful extra diagnostics, not a proof
of oscillator-only rate or a calibrated timing error. The bundled driver gives
positive COE as local RX slower than remote TX and ratio scaling 2^-26 (one count
~14.9ppb). Its comment calls that result ppm, while its local ratio macro uses a dimensionless ratio: conversion must follow actual API/examples,
not that wording. The2025 API guide also contradicts the bundled driver on COE
sign (guide: local faster; driver: local slower), so that path needs a controlled
sign/units audit before any use. At500ms,1ppb rate error corresponds to~0.15m; one COE code step
corresponds to~2.23m; that is resolution, not an accuracy or noise bound. No such value will silently become a mobile rate.
Primary source: [Qorvo DW3xxx API guide](https://forum.qorvo.com/uploads/short-url/xD3TlXKvkujjdUaJWXv2b4E7GQN.pdf),
dwt_readclockoffset / dwt_readcarrierintegrator; local bundled driver is authoritative
for the deployed API. New hardware diagnostics would need capture before RX rearm.

## Useful reformulation without pretending clock/motion separation

An apparent ARRIVAL mapping can still be useful. Buffer a valid follower reception
until the NEXT reference broadcast brackets its mapped reference emission time.
Interpolate h_A at that emission using the two reference RX markers, then compare
the follower RX with the hypothetical reference RX at the SAME emission time.
This avoids extrapolating a stale stationary clock rate across a500ms slot.
With locally affine reference-arrival h_A, the residual divided by its apparent
slope gives simultaneous range difference at the follower reception epoch, exactly
for affine radial propagation/clock; with general motion it needs an interpolation
error bound. This is a delayed observable mapping, NOT an identifiable hardware
oscillator estimate. No Kalman state is allowed to hide that distinction.

Required explicit assumptions for a conditional envelope:
- maximum receiver speed v and acceleration a;
- minimum distance r_min from the reference (distance curvature <=a+v^2/r_min);
- maximum change of relative clock rate per reference second;
- oscillator scale limits for the reference and receiver;
- maximum physical anchor separation, bounding simultaneous range difference;
- an externally established total timestamp/relation/bias error in metres.
Unknown bounds cannot be replaced by residual scatter, observed filter stability,
DW1000 power tables on DWM3000, zero motion, or an ADS additive calibration value.

For u=v/c, alpha_max from oscillator limits and relative-rate slew K,
`|h_A''| <= K/(1-u)^2 + alpha_max*(a+v^2/r_min)/(c*f_ref_min*(1-u)^3)`.
For bracketing interval T and query x, interpolation error in receiver seconds is
at most `|h_A''|*x*(T-x)/2`. Convert by the measured apparent rate to metres, add
explicit metrology error and conservative scale/Doppler normalization error.
At500ms within1s, radial curvature1m/s^2 alone contributes~0.125m; at50/10ms it
contributes~0.02375/0.00495m. Constant speed across a curved path is not zero radial
acceleration. Clock slew1ppb/s adds~0.0375m at500ms. These are conditional model
terms, not measured accuracy. Arbitrary unbounded clock drift/motion between
packets remains unobservable; no universally valid point estimator exists here.

Implementation retains ordinary hybrid NO_RATE and hybrid_static as the
stationary baseline. It provides an offline conditional interpolation/envelope
using existing UWB3 raw frames and shared C association/relation inversion. No new
on-air packet, radio transmit, persisted setting or mobile-valid flag is justified
without explicit assumptions and physical tests. Conditional interval records
must stay separate from the existing point-measurement/localization API. Two ALRs
still cannot supply a unique unconstrained2D position.

## Implemented offline conditional estimator

`tools/uwb_mobile.py` consumes the existing decoded UWB3 JSONL format. Its native
replay reuses the actual shared C codecs, association and mulDownlinkRelation:
new host-only JSON fields expose validated reference TX, mapped TX separation,
local RX separation, source role and source order. No radio or UART packet layout
changed. The source role comes from the canonical resolver, not a second guessed
ID table. Existing NO_RATE and stationary outputs remain unchanged.

The tool holds at most128 receiver-domain contexts and one pending pair per
context. A valid follower pair waits for the next consecutive reference; no output
is guessed at end-of-file or from an unclosed bracket. Same reference generation,
receiver/radio/epoch, exact reference sequence/TX, valid capture ages,900..1100ms
coarse/native cadence and an interior emission query are required. Duplicates/old
frames never refresh ages or erase an accepted candidate; gaps, restarts, late
captures and lost association suppress output. The whole next reference bracket
is used, not inter-ALR TWR stages. This adds up to one reference period of analysis
latency and allows motion/clock curvature across that window to be bounded.

For explicit oscillator bound p in fractional units on EACH source/receiver clock,
`alpha_min=(1-p)/(1+p)`, `alpha_max=1/alpha_min`, `f_ref_min=1-p`.
The reference clock is assumed affine across a bracket; any remaining source-map
error belongs in the external metrology bound. Relative clock slew K is a bound
on derivative of receiver/reference rate with respect to reference seconds.
Speed, acceleration and minimum reference distance must hold throughout the
window, not just at its endpoints. With measured apparent slope q, instantaneous
scale/Doppler normalization factors lie within
`[(1-p)*alpha_min/(q*(1+v/c)), (1+p)*alpha_max/(q*(1-v/c))]`.
Multiply the maximum deviation from1 by the physical baseline bound, then add the
curvature interpolation term, external metrology error, outward floating-point
allowance and integer representation allowance (two local plus two reference DTU).
The representation allowance is NOT DW3000 physical timestamp accuracy.

Output is `conditional_mobile_range_difference`, with apparent rate, nominal
estimate, conditional interval, assumptions and separate error terms. Positive
means follower distance minus reference distance at follower reception. Intersect
with the explicit physical baseline interval; disjoint bounds are inconsistent.
`oscillator_rate_identified=false`, `point_measurement_valid=false` and
`measurement=null` always: this cannot accidentally feed the existing point-based
2D solver. No calibrated confidence or physical validation flag is asserted. A
conditional interval is only as defensible as ALL its assumptions; violated
motion/clock/bias bounds need not be detectable from these same observations.

Run from epuck-esp32:

```sh
python3 tools/uwb_mobile.py captures/*-robot-*.jsonl --slot 500 \
 --bounds /absolute/path/to/justified-bounds.json --output mobile-conditional.jsonl
```

All seven bound fields are mandatory; none has a hardware default. The schema is
illustrated by tools/examples/mobile_bounds_synthetic_only.json. Those values are
for deterministic synthetic tests ONLY (not surveyed anchors, measured robot
limits or calibrated error). In particular its zero metrology error must not be
copied into a hardware experiment. Establish a bound for antenna/power/multipath,
source timestamp and transported ALR relation error separately; ADS correction_mm
is not such a bound. No bound means no justified metric envelope, and ordinary
firmware hybrid remains NO_RATE. To identify true oscillator rate rather than a
virtual arrival mapping needs independent path-rate/trajectory information or an
independent frequency reference. Two-ALR packet histories alone cannot supply it.

Synthetic verification includes identical all-packet histories with different
true oscillator rates/motion; different geometries hidden by smooth between-packet
clock curvature; exact linear radial-motion cancellation;2D motion/acceleration,
clock slew, signed geometry,500/50/10ms spacings,40/32-bit wrap, gaps/reorder,
duplicates, epochs, missing bounds and invalid/inconsistent inputs. Actual C
encoder→UWB3 collector→shared C relation→conditional estimator tests preserve
ordinary NO_RATE, missing closure and point-solver separation. These tests are not
hardware validation. Firmware hybrid_static remains the stationary baseline.

## Physical next step and limitations

No additional capture is needed to prove the observability limitation. No devices
need reflashing for this offline prototype: STM f1047b3 already emits needed UWB3
records when telemetry/Wi-Fi are enabled. Preserve settings/calibration and use
tools/uwb_collect.py with the existing collector workflow. For an optional physical
accuracy study, independently measured geometry/motion can supply validation truth;
this does not require externally surveyed infrastructure in deployment. Establish
metrology/clock-slew/motion bounds before claiming a conditional hardware envelope.
Compare simultaneous geometric truth at the follower reception epoch with each
conditional interval, account for delayed bracket closure, keep unobserved records
unavailable, and retain PHY/TWR/UART counters. The operator handles all hardware.
No motors, RF transmit, scheduler, calibration, EEPROM, packet or driver behavior
was changed by this prototype. It is not yet an on-device mobile mode or validated
mobile localization. Reducing the existing slot may reduce the conditional error,
but changing working ALR images is not necessary for this analysis. C/D remain
separate future work.

Validated software checkpoint: all53 Python tests and actual host C transport /
shared-replay builds pass, including12 focused moving-feasibility/interpolation
checks. The native ABI compiles with Wall/Wextra/Werror. No firmware source or
build configuration changes are part of this host-only checkpoint; no devices
were flashed. Shared core manifests remain pinned/unchanged.

Native replay context recreation (including capacity eviction) is explicitly
reported to the mobile wrapper and clears pending interpolation, independently
of any physical receiver epoch. A regression exercises loss of the native context
with a valid pending pair; its next reference cannot close the old candidate.


## Carrier-frequency feasibility audit (checkpoint B follow-up)

The user confirms no independent path-rate, external frequency reference,
integrated/calibrated encoder odometry, verified speed/acceleration/minimum-distance
limits, or measured oscillator/metrology bounds are presently available. Slow
robot motion is an expectation, not a numerical bound. The existing conditional
prototype consequently supplies no justified live metric result for this setup.

### Hardware support and deployed API

The bundled STM driver implements both APIs for DW3000:
- `ull_readcarrierintegrator`: DRX_DIAG3 (0x060029), three bytes, signed21-bit;
  CRC-good packet, captured before RX is reenabled.
- `ull_readclockoffset`: CIA_DIAG_0 (0x0C0020), low13 bits signed; requires CIA
  processing. The optional adapter enables CIA diagnostic logging and captures
  COE only when the original RX status contains CIADONE, without waiting for it.
  Missing CIADONE is marked unavailable; a present zero remains a real raw zero.

Our existing channel5, 6.8Mbps, PRF64, preamble128/code9, IEEE SFD, STS-off PHY
uses ordinary Ipatov acquisition. These diagnostics do not require an STS packet,
a DW3000 transmitter, or a new on-air field. Thus the driver/PHY supports trying
received DW1000 carrier measurements. Cross-generation packet reception is already
proven; availability, sign, bias and accuracy of these frequency values in this
particular hardware combination are NOT physically validated by that result.
See the primary [Qorvo API guide](https://forum.qorvo.com/uploads/short-url/xD3TlXKvkujjdUaJWXv2b4E7GQN.pdf),
sections dwt_readclockoffset/dwt_readcarrierintegrator, and deployed sources
`epuck-stm32/ThirdParty/qorvo_dw3/driver/{deca_device_api.h,dw3000/dw3000_device.c}`.

Bundled API algebra: CI count *3.719329833984375 gives Hz; the channel5 ppm
multiplier is negative. COE count *2^-26 is dimensionless (despite a misleading
ppm comment). COE guide/driver signs disagree, so the host labels raw values and
API algebra separately and does not choose a hardware-verified sign. One CI count
is about0.5731ppb at channel5; one COE count about14.9012ppb. At500ms, their respective
first-order range-equivalent steps are about0.0859m and2.2336m. Quantization steps
are neither calibrated errors nor a promise that averaging yields unbiased
sub-step accuracy. Multipath, PLL behavior, temperature and transmitter-specific
bias need investigation; no independent-noise or square-root averaging prior
is inserted. ADS distance offsets do not calibrate this frequency channel.

### Structural observability versus accuracy

In the ideal affine clock/constant radial-motion model already defined above,
let alpha be receiver/reference clock rate and positive v be recession. Arrival
cadence gives `q = alpha/(1-v/c)`. The normalized received carrier ratio is
`z = (1-v/c)/alpha = 1/q`, to the same propagation/oscillator model order. Therefore
adding perfect CFO to perfect packet times leaves the exact radial-motion/clock
counterexample intact. CFO can give a more instantaneous apparent rate rather
than a multi-packet average, potentially improving sampling and tracking, but
cannot independently remove Doppler. Different phase/group multipath errors are
not a reliable new motion observable. Even zero-noise carrier readings do not
establish an oscillator-only rate. More filtering improves precision only within
an identifiable model; it does not fix this rank deficiency.

For two fixed anchors with even KNOWN unit bearings n_A,n_B in2D, the first-order
apparent rates give two equations `y_i = epsilon + dot(n_i,v)/c` for three unknowns
(epsilon,v_x,v_y). Inter-ALR TWR supplies anchor clock/propagation information,
not a third independent robot velocity equation. Unknown bearings only add
unknowns. This is a general moving-model limitation, not a proof that every
constrained trajectory/geometry is unobservable. A deliberately restricted motion
model plus sufficient excitation may remove the ambiguity; it must be stated and
tested, not silently represented by a filter prior. Even identified clock rate
alone leaves range change during asynchronous ALR receptions to handle, and two
anchors alone do not yield a unique unconstrained2D position.

|Approach|Additional information needed|What it can justify|
|---|---|---|
|Bounded motion/clock interpolation|All seven explicit bounds above, valid over each bracket; physical metrology bound|Conditional range-difference intervals, not unique oscillator rate or an unconditional point|
|Encoder assisted|Calibrated wheel radii/track, heading/relative frame, slip uncertainty; relative anchor geometry/pose or an observable joint mapping model|Path-rate compensation if those states/uncertainties are observable; encoder counts alone are insufficient|
|Known1D motion direction|Known distinct bearing projections along that direction and valid1D constraint|Two apparent rates can solve scalar velocity plus clock; equal projections are degenerate|
|Independent radial-rate measurement|Actual path-rate to the relevant anchor and verified carrier sign/scale/bias|Doppler correction can identify rate; still needs accuracy/error admission and cross-time motion treatment|
|Stationary calibration then holdover|Explicitly verified stationary calibration interval, initial rate-error and oscillator slew bounds, finite expiry|Short conditional clock propagation; stationary calibration is not indefinitely valid while moving|
|External frequency reference|Independent known frequency scale|Separates clock from motion; unavailable in the intended deployment|

Infrastructure-free does not prohibit relative self-mapping/odometry: global
translation/rotation can remain a gauge. Such joint estimation needs enough
independent constraints, excitation and a stated motion/error model. Neither
wheel integration nor such a joint estimator exists here. Wheel counts zero or
a STOP command alone cannot prove physical stationarity if a robot is carried.
`hybrid_static` remains the explicit stationary baseline, not moving holdover.

For a proposed rate residual b_ppb and slot T seconds, first-order rate-to-range
sensitivity is `c*T*b_ppb*1e-9`. Unknown radial-speed residual u contributes `T*u`
metres through Doppler; stationary holdover has rate budget `b0 + slew*hold_time`.
These are separate scenario terms, not a combined statistical confidence bound.
Actual timestamp bias, reference-clock scale and relation uncertainty remain
additional errors. Shorter slots reduce sensitivity, not structural ambiguity.

### Implemented validated components and use

STM adds an explicitly opt-in raw snapshot, default OFF. It does not estimate a
clock, change validity/ranging math or make the RLR transmit. See
[STM adapter instructions](https://github.com/Yacine-Derder/epuck-stm32/blob/main/docs/hybrid_uwb.md#optional-raw-frequency-diagnostic-checkpoint-b).
The host standard-library tool supports:

```sh
python3 tools/uwb_frequency.py decode /absolute/path/to/serial.log --output frequency.jsonl
python3 tools/uwb_frequency.py sensitivity --scenario /absolute/path/to/scenario.json
```

The decoder preserves native40-bit RX,32-bit observation time, source/type/sequence,
raw CI/COE, CIADONE availability and raw status. It outputs labelled API algebra,
not oscillator calibration; `metric_rate_valid=false`, `measurement=null` and
`oscillator_rate_identified=false` on every sample. It never enters the point
solver. Snapshot slots are different packet types/times, not a complete exchange;
repeated commands may repeat snapshots. Do not treat repeated snapshots as new
independent samples. Decode invalid fields as explicit error records.

Sensitivity requires ALL six scenario fields: `radial_velocity_error_m_s`,
`frequency_error_hz`, `calibration_error_ppb`, `clock_slew_ppb_s`, `hold_seconds`,
`speed_m_s`. Units are explicit; finite nonnegative values only. No numerical
hardware defaults or automatic admission thresholds exist. For illustration ONLY,
not a measured prior, create JSON with0.2,1,2,1,10,0.2 respectively: a0.2m/s radial
uncertainty corresponds to0.1m at500ms, and2ppb initial error plus1ppb/s for10s
becomes12ppb. Output compares500/50/10ms and marks assumptions synthetic/user
assumed. A scenario calculation is not a metric measurement or hardware bound.

Deterministic tests cover exact clock/motion ambiguity with noiseless CFO plus
cadence, two-bearing rank deficiency, API widths/sign algebra/zero versus missing,
malformed snapshots, CLI persistence/explicit scenarios and slot/holdover/Doppler
sensitivity. STM tests execute actual bundled register-decoding functions (all8192
COE codes), pre-rearm capture and unchanged passive/age/safety behavior. These are
software validations only. No live moving metric estimator is admitted.

No more UWB-only logs are indispensable to establish this ambiguity. If the raw
frequency path is evaluated physically later, the narrow missing measurement is
per-packet raw CI/COE/status during a genuinely stationary receiver/ALR interval,
with an independent relative-frequency or reference timing comparison to establish
sign/scale/bias and repeated samples to characterize error. Stationary packet
timestamp cadence is a possible timing comparison once physical stationarity is
independently established; a sign check need not require a lab frequency standard.
A controlled known
radial motion can then assess Doppler. Such a validation fixture need not become
deployment infrastructure. This would validate a sensor channel, not by itself
supply the missing mobile observability. Further mobile admission needs one of
the explicit constraints above and its justified uncertainty. C/D stay deferred.


Checkpoint B coverage audit also adds complete stationary wire→UWB3 collector→
shared C receiver/replay geometry tests at500/50/10ms. Thirty-six synthetic
configurations cover positive, negative and zero distance difference; independent
receiver/follower skews of both signs; native receiver40-bit wrap; and both rate
policies. Propagation comes from Euclidean geometry, with simultaneous antenna
clock origins. Each stationary case produces six valid signed observations within
three DTU of geometric truth; the corresponding ordinary-hybrid case remains
NO_RATE with no metric measurement. These are distinct stationary fixtures, not a
moving experiment. All60 host tests and actual C transport/replay builds pass.
No production source, firmware build, core pin or hardware validity claim changes.


## Checkpoint C: explicit network roster in replay

Role and address configuration is now independent. The canonical shared core is
pinned to LPS0cb81e3 in both STM32 and ESP32 snapshots; its Network APIs and each
hybrid receiver context own an explicit ordered roster. Default legacy wrappers
remain84/85, with unchanged34/74/84-byte packets, timing/math and NO_RATE policy.
The DW1000 board integration is LPSd63a29d; STM32 schema3 stores reference/follower
IDs independently of local node/ADS role and preserves old records/settings.

The UWB3 wire schema stays unchanged, so historical captures remain readable.
It contains raw full source/destination addresses, but does not carry an
unambiguous complete configured roster. Record the two saved IDs with each
experiment and provide them explicitly for custom networks; there is no silent
role discovery or guessing from a source ID. Replay defaults are still84/85.
Run each network/slot profile separately:

```sh
python3 tools/uwb_hybrid.py capture.jsonl --slot 500 --reference 0x42 --follower 0x23 --output observations.jsonl
python3 tools/uwb_mobile.py capture.jsonl --slot 500 --reference 0x42 --follower 0x23 --bounds justified-bounds.json --output conditional.jsonl
```

Both CLIs accept decimal/0x IDs; invalid/duplicate/broadcast ALR IDs and an RLR
identity equal to an ALR are rejected. Source-role reporting and reference TX
conversion use that same roster, including native shared C relation validation.
Conditional outputs now label their actual reference/follower IDs and remain
`measurement=null`/not point-valid. No missing physical bounds are supplied by
this configuration work. `hybrid_static` remains stationary-only.

All62 host tests pass, including full-wire swapped/arbitrary rosters at500/50/10ms,
separate live default/custom contexts, CLI roster options and conditional-mobile
wrapper routing. The ESP32 firmware builds successfully with the new verified
vendor snapshot; its transport implementation, commands, UWB1/2/3 bytes and UART
policy are unchanged. Hardware custom-roster operation and moving metric accuracy
are unvalidated. STM PLAN.md tracks checkpoint C and D's remaining interface work.
