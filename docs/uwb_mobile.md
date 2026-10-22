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
sign/units audit before any use. At500ms,1ppb rate error corresponds to~0.15m; COE quantization
alone is~2.23m before filtering. No such value will silently become a mobile rate.
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
tools/uwb_collect.py with the existing collector workflow. For a later physical
study, first survey the anchors and reference-distance envelope and establish
metrology/clock-slew bounds with a stationary baseline; only then use a controlled,
independently measured moving trajectory within explicit speed/acceleration limits.
Compare simultaneous geometric truth at the follower reception epoch with each
conditional interval, account for delayed bracket closure, keep unobserved records
unavailable, and retain PHY/TWR/UART counters. The operator handles all hardware.
No motors, RF transmit, scheduler, calibration, EEPROM, packet or driver behavior
was changed by this prototype. It is not yet an on-device mobile mode or validated
mobile localization. Reducing the existing slot may reduce the conditional error,
but changing working ALR images is not necessary for this analysis. C/D remain
separate future work.

Validated software checkpoint: all52 Python tests and actual host C transport /
shared-replay builds pass, including11 focused moving-feasibility/interpolation
checks. The native ABI compiles with Wall/Wextra/Werror. No firmware source or
build configuration changes are part of this host-only checkpoint; no devices
were flashed. Shared core manifests remain pinned/unchanged.
