# Optional measured-input EKF

This is a software-tested experimental localization path, disabled by default.
It runs on ESP32 in a separate priority3 worker; bridge/control runs at priority5.
Basic UWB reception, raw forwarding and robot controls do not depend on the filter.
The canonical mathematics and assumptions are in
[EKF.md](../ThirdParty/mul_core/EKF.md). LPS firmware stays entirely ROS-agnostic;
no Raspberry Pi software, ROS/MCS subscriptions or physical experiments are part
of this implementation.

## Required input backend

The e-puck DATA step fields are documented as **commanded/phase counts**, not a
verified measured-wheel backend. DATA2 includes signed parser fields and a16-bit
source time, plus STM validation receipt time/sequence, but has no source boot
identity, declared producer counter width or measured-motion validity attestation.
Signed32-bit transport acceptance does not establish the width of the producer's
counter. This implementation never relabels those records or uses commanded speed
as measured motion. Existing DATA/DATA2 and raw UWB forwarding is preserved.

A real integration still requires a genuine measured-counter producer, explicit
counter width, source boot epoch, and justified acquisition-time alignment with
UWB and pose data. Its implementation is outside the available source/hardware
interface. The current software provides the defined input abstraction and actual
filter execution permitted by the campaign requirements. An external producer
must log all submitted commands/immutable inputs and keep ground truth separate.
It must supply valid metric TDoA with a justified receiver-rate model; ordinary
hybrid NO_RATE remains invalid. No MCU receipt clock, host wall clock or16-bit
clock is silently promoted to a common acquisition clock.

## Session and resource ownership

TCP port3333 intercepts only uppercase `EKF` commands after STM SESSION READY.
They never reach STM32 UART, ARM or motor commands. The original63-character
control input limit remains; the EKF namespace alone allows318 printable ASCII
characters per line. No task starts until explicit EKF use. Configuration and
initialization do not enable estimation; `EKF ON` is a separate command.
`EKF OFF` frees the filter/history; new INIT is required before ON. All filter
settings/state are volatile and cleared on disconnect/session change. There is
no flash journal or automatic resume for this experimental estimator.

Two bounded single-producer/single-consumer8-slot queues isolate commands/results.
The worker yields after at most four queued commands, including under continuous
input, so optional computation cannot monopolize the idle/watchdog task.
Full input refuses the command; optional result overflow is counted, never blocks
control. `UWBSTAT ekf_dropped` exposes worker queue losses, while ordinary telemetry
admission drops retain the existing counter. Results are optional telemetry and
can be lost on a slow client: archive raw commands on the producer, verify every
acknowledgement and stop/reinitialize a replay after a gap. A queued command can
finish during disconnect, but its generation-tagged output is discarded and the
worker clears its state before using the next session. No stale command/state is
adopted by a reconnect. Sending OFF/ON is not a robot stop/start operation.

The caller-owned stream is14240 bytes including3264 bytes of math workspace;
ESP allocates it only on INIT, frees it on OFF/session loss, and refuses allocation
failure. Replacement initialization can transiently hold two streams. Command
assembly is bounded to8 poses/8 measurements; stream history is8 odometry intervals
and4 joint batches. The worker has8192 stack bytes. Reviewed Xtensa static frames:
command1968, worker step704, stream init1264 plus core init1264; this deepest
reviewed application chain is about5.4KiB before library/kernel overhead. This is
static budgeting, not a runtime high-water or deadline guarantee. Query/measure
`EKF WORKER` reports actual minimum free stack bytes (ESP-IDF byte units),
free8-bit heap and largest block when invoked in the final operator campaign;
host execution explicitly reports UNKNOWN. No EKF source is linked
into LPS or STM32, so their radio RAM/flash budgets remain unchanged.

## Command interface

Decimal unsigned clock/epoch/frame IDs and timestamps are mandatory. Positions
and ranges use metres, angles radians, variances m²/rad². Full command syntax:

```text
EKF HELP
EKF CONFIG EXPERIMENTAL wheelBase metresPerCount varPerCount varFloor wheelCorr gatePerDof maxCounts maxIntervalUs lagUs maxPoseSkewUs maxTimeUncertaintyUs modes allowSynthetic
EKF LEVER bodyAntennaX bodyAntennaY fixedHeight
EKF INIT clock epoch frame timeUs x y theta Pxx Pyy Ptheta
EKF ON
EKF ODOM clock epoch timeUs sequence bits provenance leftUnsignedRaw rightUnsignedRaw timeUncertaintyUs
EKF BEGIN clock epoch frame group timeUs observationCount timeUncertaintyUs independentPoseTimes
EKF POSE HEX160
EKF OBS index sampleId TDOA|TWR anchorNode referenceNode valueM valid rateValid
EKF COVROW row c0 ... cN-1
EKF COMMIT
EKF STATE
EKF COV
EKF INNOV
EKF STATUS
EKF WORKER
EKF OFF
```

No valid wheel geometry/noise preset is assumed. CONFIG/LEVER/INIT require OFF.
CONFIG modes1=TDoA,2=TWR,3=both. `allowSynthetic=1` explicitly admits test-only
`SIMULATED`; normal `MEASURED` needs producer attestation. `COMMANDED` and `UNKNOWN`
are rejected. Wheel counters are raw unsigned16/32-bit values; rollover is handled
using declared width and configured half-period-safe maximum delta. The first
ODOM must match INIT acquisition time and establishes a baseline without motion.
Max interval is at most200ms; lag is at most2s and at least max interval. Missing
long intervals cause rejection, requiring an explicit justified reinitialization.
Wheel parameters/noise, gate and lag are independent of UWB radio settings.

A batch supplies immutable **historical** poses, not a latest-pose lookup. POSE is
the canonical80-byte USB pose record (same codec); source clock/epoch/frame must
match INIT and batch. Every observation index and covariance row must appear
exactly once before COMMIT. Radio covariance must be full, symmetric and strictly
positive definite; add only UWB noise, as the filter separately projects ALR
covariance with shared-anchor cross terms. For a TDoA observation the reference
node is mandatory and `rateValid=1`; TWR reference255 is ignored. All sample and
group IDs are nonzero and must identify physical observations, not retries.

`independentPoseTimes=1` is an explicit model assertion. Unknown/temporally
correlated ALR pose noise is rejected; pose flag4 only declares cross-robot
independence. Separately applying measurements that reuse an accepted ALR pose
would double-count its noise, so they are rejected. Include correlated TDoA/TWR
in a single joint batch with their full radio covariance. There is no hidden
assumption that repeated MCS/noisy poses are fresh independent observations.
Geometry is3D with known fixed receiver height; lever arm is configured, never
estimated. Uncertain frame alignment and slip/bias states are out of scope. A finite posterior
is conditional on its initial prior and the configured models; it does not certify
full geometric observability or unique localization from deficient geometry.

Raw logging retains the full `EKF1`/`EKF ERR` line; compact logging keeps
`type=ekf,source=esp32_ekf` records with that unchanged line and worker drops in
transport records. Responses start `EKF1`; state reports revision/time and calibration0/physical
uncertainty UNKNOWN. COV returns `Pxx,Pxy,PxTheta,Pyy,PyTheta,PthetaTheta`.
INNOV describes the chronologically last innovation after replay, which may
belong to a later group than the most recently received delayed batch. Retained
per-group diagnostics are also available through the portable history API.
Queued future batches wait for measured coverage. Out-of-history returns
`too_late`; capacity is explicit; numeric failure marks unhealthy and requires
INIT. Source epoch/frame changes require reinitialization. No host receipt-time
update, wheel-command extrapolation, pose-truth fallback or radio bias correction
is inferred. The NIS gate is a tunable experimental threshold, not calibrated
probability coverage.

## Host-only validation and replay

From this repository, these commands open no hardware or network:

```sh
sh tests/run_host.sh
python3 tools/ekf_replay.py --sanitize --output /tmp/mul-ekf-campaign-NEW
```

Output must be absent. The host compiles and executes the exact production
command service and canonical C filter. It writes seeded raw input command files,
separate pristine truth JSONL, result/innovation/covariance traces and report.json.
Straight/turn/spin trajectories, moving ALRs, independent30mm Gaussian pose noise,
shared-reference radio covariance, wheel-counter rollover, packet loss,200mm TWR
bias and3m NLOS outliers are included. Delayed delivery within retained history
must reproduce the final mean and full covariance of chronological replay.
Tests also cover stationary ALRs, other seeds/noise levels, malformed records,
unknown provenance/covariance, NO_RATE, pose reuse and session/queue isolation.

The report compares odometry only, TDoA+wheel EKF, TDoA+TWR+wheel EKF, and the
existing prior-independent unfiltered TDoA and TDoA+TWR solver. EKF all-tick RMSE
includes the initial transient; observation-time RMSE supports fairer comparison
with unfiltered available outputs. Unfiltered baseline projects the near-planar
synthetic geometry into2D and omits covariance/correlation modeling: its output
must not be interpreted as a covariance-aware solver. Availability is reported.
Empirical planar NEES is a simulation diagnostic, not a physical coverage claim.
The biased-range case intentionally exposes degradation rather than fitting it
away. These software checks cannot validate a measured-wheel backend, physical
noise calibration, RF timing or localization accuracy.

For exact replay of an archived `.commands.txt` through the same production C
service, write a separate output file:

```sh
python3 tools/ekf_replay.py --sanitize --commands /tmp/mul-ekf-campaign-NEW/turns.fused.commands.txt --output /tmp/turns-replay-NEW.jsonl
```

Every raw command/response is retained, including explicit input rejection.
WORKER reports are platform diagnostics; numerical replay cannot reconstruct
physical task watermarks or heap conditions.
No hardware input is necessary. The host fixture clears state on exit.
