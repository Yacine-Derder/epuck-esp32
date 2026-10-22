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
~14.9ppb). Its comment calls that result ppm, while its ratio macro and Qorvo API
example use a dimensionless ratio: conversion must follow actual API/examples,
not that wording. At500ms,1ppb rate error corresponds to~0.15m; COE quantization
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

Implementation will retain ordinary hybrid NO_RATE and hybrid_static as the
stationary baseline. First provide an offline conditional interpolation/envelope
using existing UWB3 raw frames and shared C association/relation inversion. No new
on-air packet, radio transmit, persisted setting or mobile-valid flag is justified
without explicit assumptions and physical tests. Conditional interval records
must stay separate from the existing point-measurement/localization API. Two ALRs
still cannot supply a unique unconstrained2D position.
