# Optional planar EKF

`mul_ekf` and `mul_ekf_stream` are portable, caller-owned software. Neither runs
in the constrained LPS production image or STM32 radio owner. There are no ROS,
MCS, USB, motor or source acquisition dependencies. The ESP adapter owns execution.
The filter is disabled after initialization and requires explicit enablement.

State is `[x metres, y metres, theta radians]` in a declared known common frame.
The fixed antenna height and body-to-antenna XY lever arm are configured; the
provider supplies ALR antenna positions in that frame. No extrinsics or frame
alignment are estimated. Differential-drive mean uses the exact constant-curvature
arc, analytic state/wheel Jacobians and wrapped heading. Wheel covariance is an
explicit experimental white per-count plus per-interval floor model with declared
left/right correlation. It is not a calibrated slip or wheel error model.

Input odometry must attest measured counters, their actual16/32-bit width, source
clock/epoch, monotonic aligned64-bit acquisition microseconds, sequence and time
uncertainty. Commanded and unknown provenance are rejected; synthetic measured
records require an extra explicit test opt-in. Modular half-turns, implausible
configured deltas, backwards/duplicate times, epoch changes and overlong gaps are
rejected. No speeds are converted into measurements; no16-bit clock unwrapping or
receipt-to-acquisition alignment is inferred. A missing valid interval is not
extrapolated. After a long gap, explicitly reinitialize using a justified prior.

A joint batch contains up to8 TDoA differences and/or AR TWR ranges at one declared
source time, immutable noisy ALR observations and a full positive-definite radio
noise covariance in m² (excluding ALR uncertainty). TDoA convention is distance
to follower minus distance to reference. NO_RATE, invalid ranges, unknown age,
unknown covariance, mismatched domains/frames, excessive skew/time uncertainty,
non-PSD poses and singular geometry are rejected. Simultaneous ALR pose covariance
is propagated with shared-anchor Jacobians, including cross-measurement terms;
radio cross-correlation must be supplied explicitly. Both noise sources and wheel
noise are assumed independent of the prior state and of each other.

The current model requires independently noisy robots AND independent pose errors
across accepted source times. The latter is a separate batch attestation, not
implied by pose flag4. Temporally correlated/unknown pose noise is rejected.
An already used ALR observation cannot be reused in another accepted batch as
independent noise; put related TDoA/TWR observations in a single joint batch.
Retained per-node sequence/time ledgers enforce this across history eviction for
up to8 ALRs per initialization. More general temporal/cross-robot correlations
would require an augmented-state filter or another explicit covariance model.

Innovation covariance uses Cholesky solves and a joint normalized innovation
squared (NIS) gate, `NIS <= gatePerDof * measurementCount`. This tunable experimental
threshold is not a claimed chi-square coverage guarantee. Covariance updates use
Joseph form, symmetric/PSD/finite checks; a rejected outlier leaves core state and
covariance unchanged. Accepted and gated group/sample IDs are retired in bounded
32-group/64-ID rings. Providers must never relabel one physical sample with new
IDs/time. Raw inputs and prefilter localization remain the caller's responsibility.

The stream retains8 measured intervals and4 joint batches. It uses measured
endpoints to interpolate constant-curvature motion, splitting configured interval
variance proportionally as a white-noise approximation. This is a documented
experimental interpolation model, not extra measured samples. Pending batches
wait for measured coverage; delayed batches are inserted by acquisition time,
then group ID, and replayed from a checkpoint. No batch is applied at receipt
time. Retention is the smaller of configured lag (up to2s) and8 intervals; the
actual checkpoint time is exposed. Over-capacity batches are explicitly rejected,
not silently dropped or treated as independent. Checkpointing retains earlier
posterior and pose-use/ID ledgers. Numeric failure makes the stream unhealthy and
requires reinitialization. Revisions distinguish revised historical estimates.

Host `tests/test_ekf.c` verifies Jacobians numerically, wheel/sequence wraps,
provenance and timestamp rejection, correlated measurement covariance, gating,
Joseph updates and chronological versus late-arrival replay equivalence under
ASan/UBSan. These checks establish software behavior, not physical calibration,
observability, hardware counter semantics or localization accuracy. The3x3 filter
has no bias/slip state; systematic errors must be evaluated separately.
