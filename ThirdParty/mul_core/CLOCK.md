# Selectable MUL clock estimation

One implementation is shared by the DW1000 follower and DWM3000 receiver. The
reference defines the domain and does not call the estimation update. Production
methods are MEDIAN_5 (selector0) and KALMAN_3_STATE (selector2). Selector1 is retired;
it is never interpreted as Kalman. No clock regression remains in production.

Median-5 retains the existing integer interval-rate estimator: validated native40
TX/RX differences, nearest signed-ppb rounding, five most recent valid intervals,
and at least three within1000ppb of their median. Five intervals are required.
Duplicate/reordered observations do not refresh age. Gaps>=3000ms, implausible
sequence/timestamp intervals or rates outside±200000ppb reset the mapping epoch.
The existing arrival-aligned mapping expires after2000ms. No delay is inferred.

Kalman uses double precision on integer-unwrapped local-minus-reference phase.
No absolute timestamp becomes a float. State units are DTU, ppb, ppb/s. For actual
reference elapsed time d seconds and k=63.8976 DTU/s/ppb:

```
F = [[1,k*d,k*d^2/2], [0,1,d], [0,0,1]]
Q = q * [[k^2*d^5/20,k*d^4/8,k*d^3/6],
         [k*d^4/8,d^3/3,d^2/2], [k*d^3/6,d^2/2,d]]
H = [1,0,0]; R=1024 DTU^2; q=1 ppb^2/s^3
```

Initialization is x=0, P=diag(R,1e10,1e4), matching the experiment prototype.
Innovation weighting is min(1,4*sqrt(H P H'+R)/abs(innovation)), with effective
R/weight^2 and Joseph covariance update. Symmetry is restored by averaging the
off-diagonals. Finite checks and a three-state LDL covariance test reject corrupt state.
Nonpositive leading LDL pivots fail closed; final-pivot negative roundoff up to
1e-12 times its covariance diagonal is tolerated; this is a numerical
PSD tolerance, not permission for negative rate variance. No filter tuning uses
the held-out segment. Missing observations advance F/Q over the measured next
interval; public prediction supports up to the existing2s mapping horizon. It
never refreshes the hardware observation age or fabricates a missing reference.

Applied ppb is rounded endpoint frequency plus drift times half the configured
base beacon spacing. This is the reference prototype's average rate over the next
10ms slot in the current setup. Raw mapping origins remain the actual TX/RX pair;
phase is a latent filtering state, not a new synchronized timestamp intercept.
Optional supplied propagation compensation uses the existing delay-validity API
and reinitializes the filter when correction availability changes.

Kalman application requires five accepted intervals, a consistent nonzero mapping
epoch, finite/PSD covariance, abs(rate)<=200000ppb and conditional rate sigma<=1000ppb.
The sigma ceiling deliberately reuses the preexisting Median-5 one-ppm stability
scale; it is a permissive oscillator-trust ceiling, not a physical accuracy bound.
Its consequence is bounded startup confidence and rejection of a corrupt/unbounded
filter, not a claim of millimetre accuracy. After startup, a phase innovation
larger than the existing one-ppm interval gate marks the current raw mapping
origin unavailable even if the robust frequency state remains finite. This
protects against a timestamp spike being reused as a trusted affine origin;
it does not change R/q, Huber weighting or the latent Kalman update. If KF is unreliable, the independently
maintained median applies only when its existing validity gate succeeds; otherwise
mapping remains unavailable. Numerical failure reseeds KF and exposes reason3.
Reasons:0=used,1=warmup,2=uncertain,3=numerical. Huber downweighting increments rejected
count but need not disable an otherwise reliable KF. Both methods retain the
existing2s stale gate; a mathematical prediction does not authorize stale RF use.

The12-byte configuration codec is now version2. Bytes1/2/3 are selector/5/5;
period, historical gate field and compensation flag retain their positions. The
gate field is retained for EEPROM compatibility and does not tune KF. Version1
median/regression settings migrate to Median-5; unknown selectors also become
Median-5. Structural corruption remains an error handled by platform persistence
safety; it must not erase addresses, plans or calibration. Obsolete regression
window controls are retired. Configuration writes are platform-owned and require
RF OFF, as before.

Validation: portable sanitizer tests cover both methods, signs, drift/nonlinearity,
variable/missing intervals, native40/uptime32 wrap, outliers, steps, covariance
failure, fallback/unavailable and outage/reacquisition. The read-only
`tools/test/verify_clock_model.py` loads only the original analysis's Kalman function
and compares production C states/rates with fixed R/q. Use a Python environment
with NumPy (on the development host `/usr/bin/python3`). Follower REF_RX logs lack
MCU uptime: that replay labels coarse age as inferred1Hz cadence; it is not a
validation of follower outage handling or physical accuracy.


## Bounded missing-reference recovery

Median-5 retains its original3000ms reset and five-interval warmup behavior.
Kalman retains a previously ready, same-epoch latent model for up to6000ms from
its last real reference observation. This is **not** a mapping-lifetime extension:
the existing2000ms freshness gate still rejects mapping/TDoA with an old origin.
No synthetic reference or timestamp is generated. Prediction uses actual elapsed
hardware TX time, with unchanged F/Q/R, Huber weighting and Joseph covariance.

On a returning packet after3..6s, the wrapper checks reference identity, increasing
modular sequence bounded by the configured period, TX separation consistent with
that sequence within the existing10% interval tolerance, positive local RX
separation and the original±200000ppb rate gate. Covariance/innovation/uncertainty
checks still run. Only a valid real observation replaces the mapping origin. The
independent median history restarts; it cannot become a ready fallback merely
because KF was retained. KF keeps its prior maturity across a validated gap.
The latent phase/rate/drift and covariance are not reseeded on that path.

After6s, nonmonotonic sequence, implausible timestamp, epoch change, numerical
failure or compensation-coordinate switch, the existing reset/rejection behavior
remains. PSD/finite checks, the1000ppb conditional sigma ceiling and gross RAW-origin
innovation gate remain enforced. AA propagation keeps its original measurement
age and epoch: a temporary radio-search state no longer invalidates a still-fresh
measurement when the clock model is eligible for recovery, but an expired/changed-
epoch measurement is never refreshed or used by this mechanism. TWR operations
are still canceled on reference loss. RF scheduling and packet formats are unchanged.

`recovered` occupies the last padding byte (fit state remains160 B). It marks a
retained gap history, not guaranteed validity; inspect ready/applied, fallback,
map status/age and reset reason together. LPS prints `hold` on loss and `rc` in KF
records; STM32 CLOCK shows holdover/recovered. Existing raw sequence/TX/RX telemetry
identifies observed gaps, without claiming they were antenna-level RF losses.
No EEPROM/configuration field or telemetry schema changed.

Host tests cover1..6s intervals (up to five missed1Hz references), drift with
repeated bursts, wrap, stale mapping before return, covariance failure, timestamp
corruption and>6s reset/reacquisition. The receiver comparison tool replays old and
new C estimators on identical captured snapshots, keeping original follower
relations fixed and explicitly cold-starting at capture entry. Follower log replay
uses real TX/RX but inferred1Hz coarse age and no propagation compensation; it is
not physical accuracy validation. Missing packets/relations remain missing.
