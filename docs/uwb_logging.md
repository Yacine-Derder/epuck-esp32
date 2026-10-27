# Untethered onboard RLR TDoA logging

The STM32 now emits its actual `mulLocalizationObserveNetwork()` result over
Wi-Fi, including invalid associated observations. The PC does not recalculate
TDoA. An RLR distance **difference** is not an ALR–ALR TWR range, an absolute robot
range, or a unique two-anchor 2D position. Normal `hybrid` has no independent mobile
receiver rate and retains NO_RATE. Use `hybrid_static` only while stationary;
AVAILABLE does not establish antenna calibration, bounded uncertainty or accuracy.

## Firmware and configuration

Update **both STM32 and ESP32**: the additive `UWB3` record type7 uses the same
152-byte binary/309-character text/310-byte UART line as existing types, but older
ESP32 validators reject the new type. Existing UWB1/2/3 logs/types remain readable.
The legacy two-ALR path retains its radio frames, TDMA, TWR and synchronization.
The additive multi-ALR profile described below requires matching updated ALR firmware.
The canonical revision is recorded in ThirdParty/mul_core/UPSTREAM.json;
checked-in snapshots verify/build offline without fetching.

Build (these commands do not program robots):

```sh
cd ~/Documents/epuck-stm32
cmake -S . -B build/Universal -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake \
  -DCMAKE_BUILD_TYPE=Release -DMUL_UNIVERSAL=ON -DMUL_UWB_ONLY=ON \
  -DMUL_TWR_ROLE=0 -DMUL_DIAG_ROLE=0 -DPHY_INTEROP_ROLE=0 \
  -DMUL_TIMING_TRACE=ON -DMUL_ADS_TRACE_TIMES=ON
cmake --build build/Universal
python3 tests/runtime_config/check_elf.py build/Universal/EPuck_STM32.elf

cd ~/Documents/epuck-esp32
source ~/.espressif/tools/activate_idf_v6.1.sh
export IDF_TARGET=esp32s3
python3 "$IDF_PATH/tools/idf.py" build
```

Operator-only flashing, when ready (not executed during development):

```sh
~/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI \
  -c port=SWD -d ~/Documents/epuck-stm32/build/Universal/EPuck_STM32.elf -v -rst
cd ~/Documents/epuck-esp32
source ~/.espressif/tools/activate_idf_v6.1.sh
# Replace the example port with the actual ESP32 programmer port.
python3 "$IDF_PATH/tools/idf.py" -p /dev/ttyUSB0 flash
```

Alternatively load the same STM32 ELF in STM32CubeProgrammer over ST-LINK/SWD.
Do not perform a mass erase: configuration sectors must be retained. ESP32 artifact
is `~/Documents/epuck-esp32/build/comm_with_stm32.elf`; existing ESP-IDF flash
arguments choose the required binary offsets. Never reuse compile-test ADS session
IDs for a physical ranging experiment.

On the STM32 USB console, send one command at a time and wait for acknowledgement:

```text
off
addr 0x8d
reference 0x84
follower 0x85
peer list
```

If either ALR is missing, add it with `peer add 0x84` / `peer add 0x85`; otherwise
use `peer enable 0x84` / `peer enable 0x85`. Do not clear unrelated peers or
calibration settings. Then:

```text
role rlr
mode hybrid_static
wifi on
tlm on
cfg set telemetry_ms 1000
show
status
```

Keep RF off while unplugging USB and positioning the RLR. Once stationary, use
the existing Wi-Fi command client to enable RF, then exit that client before
starting the collector (the bridge has one TCP session owner):

```sh
cd ~/Documents/epuck-esp32
python3 tools/wifi_client.py 192.168.1.141 --plain
```

Wait for SESSION READY, type `on`, wait for acknowledgement, then Ctrl-C. The
client sends no automatic ARM/movement; its final STOP is the robot safety command,
not UWB `off`. The collector remains read-only and never enables RF or configures
robots itself. If RF was already on while carrying the RLR, use `off` then `on`
after placement to reacquire a fresh stationary rate; do not treat moving/warm-up
samples as valid stationary measurements. Preserve robot STOP/watchdog behavior.

## Record and monitor

```sh
cd ~/Documents/epuck-esp32
python3 tools/uwb_collect.py --robot 192.168.1.141:3333
```

Default output is `~/Documents/uwb_logs/<UTC>-robot-0-compact.jsonl`, automatically
created. The live display shows connection state, latest difference in meters,
status, valid/invalid counts, running mean and sample standard deviation in meters.
Statistics are per endpoint over this capture; invalid/duplicate/old measurements
are excluded. No USB is required. Renderer I/O runs on a daemon thread and cannot
hold the acquisition lock. Ctrl-C stops recording. Options:

```sh
# No live display; suppress periodic health rows.
python3 tools/uwb_collect.py --robot 192.168.1.141:3333 --no-live --health-seconds 0
# Complete diagnostic capture, unchanged historical collector structure.
python3 tools/uwb_collect.py --robot 192.168.1.141:3333 --raw
# --debug is an alias for --raw; --output can override the directory.
```

Repeat `--robot` for multiple endpoints; each gets its own file. The tool sends
only CONTROL_HB, reconnects after interruptions, and does not replay commands.
A stalled filesystem can still delay acquisition (as with the previous logger);
existing firmware safety timeouts are unchanged. New host display rendering adds
no console I/O to the socket-read path.

## Compact schema (version1)

One `capture` header defines the endpoint, unique capture ID and time/source
semantics. `session` rows assign short `s` IDs to node/radio/firmware epoch/mode/role;
`roster` rows bind ALR IDs and stationary/unknown policy. They are emitted only when
needed, not repeated on each measurement. Keep these rows when merging files.

Available TDoA (status4, valid clock and stationary rate are implicit):

```json
{"t":"2026-10-23T14:25:12.123Z","s":"0123456789abcdef-1","type":"tdoa","ref":3666,"fol":3676,"u_ms":324487,"dt_dtu":-28,"dd_mm":-131}
```

Unavailable associated observation:

```json
{"t":"2026-10-23T14:25:13.123Z","s":"0123456789abcdef-1","type":"tdoa","ref":3667,"fol":3677,"u_ms":325487,"dt_dtu":null,"dd_mm":null,"status":1,"clock":true,"rate":0}
```

`t` is host receipt UTC; `u_ms` is the onboard follower-capture coarse uptime,
wrapping uint32. Exact native40 RX timestamps remain in raw mode. `ref`/`fol` are
independent32-bit sequences, not equal transaction IDs. Status:0 no_relation,
1 no_rate,2 stale_rate,3 inconsistent,4 available. Receiver rate-state:0 unavailable,
1 stationary,2 stale. Invalid numeric results are null; INT32_MIN is never a distance.
The unit of `dt_dtu` is nominal reference DTU, not host time.

Roster rows additionally include `slot_ms` (10,50 or500), emitted again when the
runtime delay changes. Existing compact fields retain their meaning.

Other rows: `twr` has `peer`, `u_ms`, `mm` and optional `calibrated:true` (otherwise
raw distance); `connection`/important `message`/`error` capture state changes;
`gap` counts missing **full telemetry-stream** records, including raw packets
omitted from compact files; `ordering` reports duplicate/old records without
counting them again. Compact `health` defaults to30s; `transport` is emitted when
bridge error/drop counters change. Counters are cumulative, not current delivery
rates. STM `dropped` combines USB and Wi-Fi queues: disconnected/unconsumed USB
queues can increase it without Wi-Fi loss. Prefer sequence gaps and bridge counters
for transport diagnosis, while accounting for reconnects and logging windows.

Raw/debug retains `kind`, endpoint, host UTC/monotonic time, complete `legacy_frame`,
native RX, source/receiver epochs, sequence tracking, association generations,
transport/radio counters and all bridge lines. New raw `tdoa` rows also retain
actual onboard clock/rate/status, rate ppb, propagation ages/state, power flags
and unknown-component mask. No received UWB frame is duplicated into a TDoA row.
Use `--raw` for existing `uwb_hybrid.py`, `uwb_mobile.py`, `uwb_tdoa.py` and
`uwb_localize.py` reconstruction tools; compact files intentionally lack those inputs.

Small standard-library loader for plotting (no new runtime dependency):

```sh
python3 - ~/Documents/uwb_logs/REPLACE-WITH-CAPTURE-compact.jsonl <<'PY'
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1])]
valid = [r for r in rows if r.get('type') == 'tdoa' and r.get('dd_mm') is not None]
t = [r['t'] for r in valid]
difference_m = [r['dd_mm']/1000 for r in valid]
print(list(zip(t, difference_m)))
# Optional, if matplotlib is already installed:
# import matplotlib.pyplot as plt
# plt.plot(t, difference_m); plt.ylabel('distance difference (m)'); plt.show()
PY
```

## Old logs and validation

```sh
python3 tools/uwb_collect.py --migrate-logs
```

Only recognizable project JSONL/MUL logs from the old `~/uwb_logs` are copied;
original files/directories remain. Destination conflicts with different contents
use a `.collision-<SHA256>` suffix. Copies are exclusive, fsynced and hash-verified;
identical files are reused. Symlinks/unrecognized files are skipped, never followed
or overwritten. No symlink masks outdated scripts. Active tools now default to
`Path.home()/Documents/uwb_logs`; the old path exists only as a migration source.
The actual migration copied three files (one JSONL and ALR132/133 logs), verified
SHA-256 equality, and retained all originals; no collisions occurred.

## Delivery failure and bounded priority

The bridge previously captured `now` before UART reception, but timestamped a
completed telemetry record inside reception. A millisecond tick crossing made
`now - queued_at` underflow as uint32, treating a fresh record as almost 49 days
old and discarding it before TCP transmission. `UWBSTAT` was queued after the
expiry check, so transport reports could arrive while UWB records disappeared.
This mechanism is reproduced in the actual bridge owner test by advancing the
clock during fragmented UART input; refreshing time before expiry fixes it.
It explains missing records without CRC/UART errors. Physical confirmation is
still required; the existing capture alone cannot prove which records were lost.

There were also independent starvation paths: STM32 runtime queues admitted raw
records and TDoA with equal priority, and the UART telemetry slot waited for the
mixed control/debug FIFO to become empty. Runtime and bridge queues now let TDoA,
ranges and paired observations evict an optional record when full, retaining FIFO
order among survivors. Optional traffic cannot displace observations. UART control
responses take precedence over telemetry, which precedes optional LOG/DATA.
Partially transmitted bridge lines cannot be replaced or have disconnect notices
spliced into them. Queues remain four per STM sink and eight on ESP; the wire codec,
radio mathematics and USB verbosity behavior are unchanged. Five 310-byte records
per second consume about 13.5% of 115200-baud 8N1 capacity, so larger queues are not
needed to sustain this experiment's nominal load.

Counter interpretation:

- STM32 `TLM STATUS drops=USB/WIFI` reports independent runtime sink losses.
  The USB queue drains only through `TLM NEXT`; an unused/full USB queue is expected.
- STM32 `uart_drops` reports records accepted by the pending UART slot then
  expired/purged, separately from runtime Wi-Fi queue loss and UART errors.
- ESP32 `UWBSTAT dropped` / compact `bridge_dropped` reports bridge queue
  eviction, expiry, disconnected arrivals and session purge. It is not an STM32
  queue counter. Optional eviction can increase it under intentional overload.
- Full-stream gaps include optional raw/summary records; assess observation
  delivery with ref/fol progress during a stable connected interval too.

Software validation includes the shared-core sanitizer suite, STM32 runtime and
UART/console suites, and ESP32 C/Python host suite. Dedicated tests exercise ten
simulated minutes of saturated optional traffic with valid/invalid observations,
unconsumed USB, UART HAL_BUSY, control priority, uint32 wrap, full-essential
queues, fragmented input, partial TCP lines, socket backpressure and reconnects.
An end-to-end fixture feeds C-encoded records through the actual C bridge parser,
owner loop and TCP socket into the Python collector, checking every status in
compact/raw output across reconnects. It also compares codec source/header bytes
with sibling STM32 and canonical LPS checkouts when available. Existing tests
check sentinel rejection, native40 precision, old wire versions and compact/raw
schemas. No physical robots were flashed or contacted by these tests.

## Hardware acceptance after operator flashing

1. Build using the commands above and flash **both** STM32 and ESP32 with the
   listed operator commands. Do not mass-erase STM32 configuration sectors. No ALR
   update is required for this transport fix.
2. Inspect `show`, `peer list`, `hybrid status` and `tlm status`. Confirm address
   `0x8d`, reference `0x84`, follower `0x85`, enabled peers, role RLR,
   `mode hybrid_static`, `wifi on`, `tlm on`, `telemetry_ms 1000`. Keep the RLR
   stationary; use `off` then `on` after placement to reacquire its rate. Record
   counters after the collector connects, since disconnected losses are expected.
3. Run a ten-minute compact capture (or stop manually with Ctrl-C):

   ```sh
   cd ~/Documents/epuck-esp32
   timeout --signal=INT 600s python3 tools/uwb_collect.py --robot 192.168.1.141:3333 --no-live
   ```

4. Confirm initial unavailable observations retain `status` and null `dd_mm` /
   `dt_dtu`; stationary AVAILABLE records should have numeric signed `dd_mm`.
   To exercise NO_RATE deterministically, separately capture ordinary `mode hybrid`
   (after safe RF off); restore `hybrid_static` and reacquire while stationary.
   Do not expect metric observations in ordinary hybrid.
5. Repeat `tlm status` during capture. After initial draining, healthy Wi-Fi
   depth should not remain pinned at four; Wi-Fi/UART/bridge drop counts should
   not grow continuously at nominal load. USB drops may keep growing without
   `TLM NEXT`. Check ref/fol progress and observed counts across the sustained
   interval, along with corruption/UART errors. Disconnect/reconnect the collector
   and verify observations resume without changing verbosity or consuming USB.
6. Repeat with `--raw` (or `--debug`), and verify `kind:telemetry` rows include
   `type:tdoa` plus `hybrid_rx` legacy frames and summaries. Repeat with USB
   unplugged and with verbosity quiet/full; essential Wi-Fi delivery should
   continue. Raw diagnostics are best effort under actual transport overload.

Both native firmware builds and STM32 configuration-sector ELF validation passed.
Physical UART/Wi-Fi delivery, RF timing and stationary measurement accuracy remain
operator acceptance work; software tests do not establish hardware success.

## Runtime slot deployment

The STM32 and both ALRs now support persistent `cfg set slot_ms 10` (also50/500)
using one firmware build per platform. New factory settings use10ms; old settings
without a slot migrate read-only to500ms and require explicit operator selection.
The1Hz reference cycle and onboard TDoA sign/status/null semantics are unchanged.
Scheduled radio frames advertise their slot; mismatches fail closed. The UWB3
UART format/record length is unchanged, so the previous ESP32 bridge firmware
with the telemetry-delivery fix remains compatible and needs no scheduling flash.

Use the coordinated build, flash and timing-acceptance procedure in the sibling
`lps-node-firmware/docs/mul_commands.md`, section "Three-robot10ms deployment and
hardware acceptance". Flash both ALRs and STM32, configure all three while RF is
off, verify persistence, then enable stationary acquisition. `--raw` retains full
advertised frames; use `tools/uwb_hybrid.py ... --slot 10` for10ms replay. The
collector itself needs no slot flag and performs no robot configuration. Compact
roster rows expose active `slot_ms`; old compact captures without this optional
field remain readable. No hardware timing/accuracy is inferred from software tests.

### Multi-ALR vectors

UWB3 type8 (`tdoa_vector`) is additive; type7 (`tdoa`) and all existing logs retain
their interpretation. The collector accepts a coherent vector at approximately
1Hz and stores each follower's identity, slot, signed millimeter difference,
validity/status/reason/clock/rate. Unavailable numeric measurements are JSON null.
Each compact vector includes its ordered roster, cycle, schedule revision/hash,
spacing, period, seen/valid masks and receiver uptime, so partial vectors remain
self-describing. `--raw` also keeps optional type9 `multi_rx` packet evidence.
The bridge prioritizes vectors over raw packets and summaries in the same bounded
queues. Ordinary `hybrid` reports no-rate; `hybrid_static` is stationary opt-in.

Reproducible host end-to-end check, without devices:

```
cd ../epuck-stm32
MUL_MULTI_TRACE=/tmp/multi.uart sh tests/runtime_config/run.sh
cd ../epuck-esp32
python3 tools/verify_multi_replay.py /tmp/multi.uart
sh tests/run_host.sh
```

The first command drives real STM32 adapter/math/queues using synthetic hardware
registers and geometry for N2/3/4/8. The replay passes its80 real UART vector
records through the ESP owner, loopback TCP reconnect and compact logger, and
compares all follower values and identities. Neither command accesses hardware.
The live display shows each follower ID/value and valid-count for multi vectors;
legacy scalar mean/deviation statistics retain their existing meaning.
Optional `--metadata experiment.json` copies an experiment JSON object (up to64KiB)
into the capture header, or a raw experiment record. Example fields are
`experiment_id`, `alr_positions_m`, and `rlr_ground_truth_m`; metadata never affects
measurements or robot configuration.

### Clock-estimator campaign records

With the updated STM32/ESP pair, `cfg set samples on` enables optional UWB3 type10
`clock` records. Compact JSONL retains raw reference TX and local RX as exact
40-bit integers, reference sequence, applied/fitted ppb, residual RMS, window,
count/inliers/rejections, fit-ready versus actually-applied, fallback reason,
mapping age/status and clock-epoch identity. Unknown sigma becomes null; conditional
fit scatter excludes drift, propagation and geometry and is not physical accuracy.
Default samples-off queue behavior and all previous record types remain unchanged.

`cfg set clock_estimator median|regression` and `cfg set clock_window 8..32` require
RF OFF. Use `clock show` for a saved/live snapshot. Both clock streams can be
replayed with the canonical production-core tool in lps-node-firmware:

```
python3 tools/mul_clock_replay.py --summary ../uwb_logs/alr133.log
python3 tools/mul_clock_replay.py --summary ../uwb_logs/EXPERIMENT-compact.jsonl
```

The tool compares the actual median and robust regression, plus held-out offline
linear/quadratic forecasts. It never applies a host correction to recorded TDoA
and does not access devices. Fallback to median during changing apparent rate is
explicit in the record. Slot/device bias remains confounded without permutation
and independently measured geometry; a slope is not proof of a clock root cause.

Experimental numerical timing uses quality marker3 in UWB3 vector/raw envelopes.
Decoded vectors and compact logs include `schedule_profile: 3`,
`experimental_timing: true`, numerical `slot_ms` and actual `cycle_ms`100..2000.
Unmarked records retain10/50/500ms spacing and1000ms cycle validation. This marker
is a protocol identifier, not signal-quality evidence.

Clock type10 v2 includes `sample_period_ms`, `capture_age_ms` and `capture_valid`
(age<=15ms), preserved in compact JSONL alongside raw reference TX/local RX and
mapping epoch. V1 records remain readable with null age/validity/period; replay must
state its assumed legacy period. Valid capture age does not establish clock or
propagation accuracy. `physical_uncertainty` remains unknown.

Software-only cross-platform checks:

```sh
# In epuck-stm32; production adapters with simulated RX/time and no devices:
MUL_TIMING_TRACE=/tmp/timing.uart MUL_MULTI_TRACE=/tmp/compat.uart sh tests/runtime_config/run.sh
# In epuck-esp32; sockets bind only to loopback:
python3 tools/verify_multi_replay.py --experimental /tmp/timing.uart
python3 tools/verify_multi_replay.py /tmp/compat.uart
```

The experimental check samples a bounded subset from the full five-profile adapter
trace and exercises the actual ESP parser/queues/TCP reconnect/compact collector.
Full STM32 scenarios cover2/3/5/7/17ms and100/200/500/1000/2000ms, injected rate/skew,
RF-OFF persistence, frame preservation and zero RLR transmissions. Simulation is
not physical DW1000/DW3000 interoperability or short-slot reliability evidence.

The earlier shared-core update `e7f3518` added fingerprint-bound experimental AA
reply20/10/5ms selection and explicit short-reply TWR math. Telemetry layouts are
unchanged: schedule fingerprints distinguish timing settings, and raw records
retain the original radio bytes. The ESP bridge remains passive; this update does
not enable RLR TX. Production STM32 trace replay checks matching5ms schedules
across numerical broadcast spacings, through the bridge and compact logger.


The planned receive checkpoint used published LPS `f5bbbec`, adding planned telemetry11/12
to full-plan codec2/RF4 and passive receiver APIs. STM32 schema9 selection connects
the RF4 receive path. The bridge prioritizes planned vectors and the collector
labels `schedule_profile=4`, `fingerprint_scope=ranging_plan`; fingerprints cover
the full plan. Existing type8/9 profiles remain unchanged. Type11/12 requires
quality4; unknown/mixed markers and raw RF4/fingerprint disagreement are rejected.
The bridge remains passive and no hardware operation accompanied this update.


Generated-plan software replay (no device endpoint; loopback sockets only):

```sh
# In epuck-stm32:
MUL_PLAN_TRACE=/tmp/plan.uart sh tests/runtime_config/run.sh
# In epuck-esp32:
python3 tools/verify_multi_replay.py --planned /tmp/plan.uart
```

This feeds bounded representative actual STM32 adapter output through the ESP
UART parser, priority queues, TCP reconnect and collector. Raw debug output retains
native frame bytes/full-plan identity; compact vectors preserve profile4 and
fingerprint scope. Independent host producer/parser tests coverN2..8 and raw40/80/90.
Firmware receive tests useN2/4/8 and100/200/1000ms including faults/wrap, with zero
RLR TX in that receive-only fixture. The AR fixture below covers assigned TX.


Shared core `9935db3` adds AR range13 and quality14. These strict quality4
records retain profile4/full-plan scope. Essential range13 takes priority over
optional debug traces in STM32 and ESP queues. Debug and compact JSON preserve
raw/corrected initiator-DTU ToF, nominal range conversion, six mixed-clock stamps
and their node identities, power status, acquisition/report uptimes, reply/pair/op
and UNKNOWN physical uncertainty. `calibrated` is false; no ADS-only correction
is inferred. `ar_quality` exposes responder failures/drop/reset counters.
`reference_seen` means a reference was observed, not that its map is fresh.

Type12 planned traces now explicitly identify RX versus verified TX and
`marker_dtu`; TX has `tx` set and `rx` null. Acquisition/report times are receiver
uptime; host UTC remains reception time. Compact output retains the raw range
evidence for later analysis. Older decoders reject13/14 rather than interpreting
them as passive TDoA. Ordinary hybrid NO_RATE is unchanged.

```sh
# STM32 host simulation; no devices:
MUL_AR_TRACE=/tmp/ar.uart bash tests/runtime_config/run.sh
# ESP actual transport + collector, loopback only:
python3 tools/verify_multi_replay.py --ar /tmp/ar.uart
```

The AR fixture configures, saves, enables and reboots mixed AA/AR plans with
multiple RLR assignments, then simulates five-message exchanges and queue loss.
It verifies26 delivered records through UART framing, priority queues, TCP
reconnect and both log formats. This is software verification, not physical
interoperability or calibrated range-accuracy evidence.


Current shared pin13fcdeb additionally supports64-byte full-plan codec3 for opt-in
freshness/priority scheduling. Fair plans remain byte-compatible codec2. RF4 and
UWB3 envelopes stay unchanged: the full-plan fingerprint covers policy/priorities
and prevents mismatched reconstruction. Host scheduler previews report the actual
policy/version; telemetry age continues to describe acquired data, not a planned
deadline. Physical freshness or noise bounds are not inferred from this setting.


Core f51877d adds external pose propagation provenance. Vector payload byte149=1
marks source-aware packed entries:0missing,1TWR,2external,3none. TDoA payload
115=1/116=1or2 identifies external/none. Legacy zero subversions remain unchanged;
older strict parsers reject new markers. Debug and compact output preserve
`propagation_metadata_version`, `propagation_source` (per follower for vectors),
and UNKNOWN physical uncertainty. A none source cannot produce a corrected
clock/available metric observation. Supplied covariance is never a physical bound.

`verify_multi_replay.py --geometry TRACE` tests actual LPS-generated external/none/
stale radio frames after the STM32 fixture reconstructs them.35 records retain
raw frames and source provenance through ESP UART/priority/TCP reconnect/debug and
compact JSON. `--planned` also covers external relations acrossN2/4/8. These are
software simulations; no physical link or Pi/ROS/MCS bridge is involved. See the
LPS pose codec/host generator and command docs for source time/age/frame contracts.
