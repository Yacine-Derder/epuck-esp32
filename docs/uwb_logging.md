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

`cfg set clock_estimator MEDIAN_5|KALMAN` requires
RF OFF. Use `clock show` for a saved/live snapshot. Both clock streams can be
replayed with the canonical production-core tool in lps-node-firmware:

```
python3 tools/mul_clock_replay.py --summary ../uwb_logs/alr133.log
python3 tools/mul_clock_replay.py --summary ../uwb_logs/EXPERIMENT-compact.jsonl
```

The tool compares the actual Median-5 and Kalman, plus held-out offline
linear/quadratic forecasts. It never applies a host correction to recorded TDoA
and does not access devices. Fallback to a valid median when Kalman is unreliable is
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

## Replay-complete TDoA diagnostics

This extension targets the existing multi/generated-plan receiver on STM32.
It changes diagnostic telemetry only; UWB frames, active estimators, calibration,
RF timing, stored configuration, Wi-Fi connection policy and control commands are
unchanged. No LPS firmware update is required for this feature. The capture source
is the STM32 receiver, not a direct PC serial connection to it.

### Path and bounded storage

STM32 takes native40 RX markers and decoded observations in the existing radio
owner. A snapshot is frozen before cycle rollover and completed with the original
vector result. Existing production downlink intermediates are copied through a
trace-only link wrapper; they are not calculated by a replacement firmware path.
Serialization runs after radio processing, with no extra radio SPI reads or heap
allocation. The fixed 152-byte CRC16 UWB3 envelope becomes a 309-character hex line
on the existing 115200-baud UART3. STM32's USB and Wi-Fi queues each hold four lines;
the UART link has one pending line. Existing one-second expiry/backpressure applies.

The ESP owner validates type 15 and transparently forwards it as optional telemetry
through its existing eight-slot, 320-byte line queue and TCP port 3333. It retains
control priority, TTL, disconnect purge and UWBSTAT loss/error reporting. The PC's
existing `uwb_collect.py` validates CRC, shared telemetry sequence and epoch, and
reassembles snapshots while retaining existing session/gap/health records. Compact
output flushes each JSONL row. Unknown diagnostic schemas become explicit errors;
older UWB1/2/3 measurement records remain supported. Raw/debug mode retains fragments
rather than projecting a completed snapshot; use default compact mode for validation.

A pair snapshot is 384 bytes in four fragments. At one pair/second this adds
1240 B/s with LF, or 1244 B/s with CRLF: at most 10.8% of UART payload capacity,
plus existing telemetry. STM32 holds four snapshots and seven prepare buffers;
new snapshots are dropped when full. Queued snapshots expire after one second
(the downstream UART/FIFO freshness bound), before admission and emission; this
prevents a disconnected capture from draining ancient data ahead of current cycles.
Diagnostic IDs advance even for drops, and
subsequent fragment headers expose the cumulative diagnostic drop count. Each
fragment requires two free normal queue slots, cannot evict essential records,
and is emitted at most once per owner-loop iteration. ESP slots are unchanged.
PC reassembly holds at most eight partial snapshots (3072 payload bytes plus Python
metadata), and reports eviction, integrity failure or unfinished connection data.
A newer snapshot ID closes an older partial on the same ordered device stream, so
an initial partial is reported immediately rather than mislabeled at shutdown.
Large rosters/high cycle rates may lose diagnostics; this is not a lossless N8
streaming guarantee. Completely silent/disconnected tails cannot report a future
counter; capture boundaries are explicitly part of the completeness limit.

### Type-15 schema 1

All multibyte integers are little endian; native timestamps use five bytes, modulo
2^40, with nominal DTU frequency 63897600000 Hz. Host UTC is reception time only.
Every fragment retains the common node/radio/mode/role, boot/radio epoch, telemetry
sequence, cycle and emission uptime. Type 15 is diagnostic evidence, not a radio
packet or a separate measurement. The wire header is:

| Bytes | Meaning |
|---|---|
| 0–23 | Existing MT/version3/type15/device/role header, schema=byte11, sequence, epoch |
| 24–27 / 28–31 | Diagnostic snapshot ID / cumulative snapshot drops (uint32) |
| 32–39 | Cycle / emission uptime (uint32) |
| 40 / 41 / 42–43 | Fragment index / count4 / blob size384 |
| 44–47 | FNV-1a32 digest of complete blob; association/integrity, not authentication |
| 48 / 49 | Payload length100,100,100,84 / follower slot1..7 |
| 50–149 | Fragment payload with zero padding |
| 150–151 | Existing CRC16-CCITT, initial0xffff |

Association uses device/radio/epoch/diagnostic ID, with cycle/slot/digest agreement
on every fragment. Per-fragment sequence is retained in compact `fragment_sequences`.
Every complete compact row contains the original `blob` hex, its digest, schema,
identity and decoded fields. This is the authoritative lossless baseline input.
The blob layout is:

| Offset | Contents |
|---|---|
| 0–15 | Schema, planned/stationary flags, pair slot, cycle, seen mask/count, vector source-version flag, reserved0, cycle-start uptime |
| 16–79 | Full local 64-byte generated plan, or 32-byte schedule plus32 zero bytes |
| 80–177 | Reference and follower observations, 49 bytes each |
| 178–219 | Complete decoded on-air follower relation, 42 bytes |
| 220–259 | Applied receiver mapping (origins, epoch, sequence, uptime, signed ppb, status, baseline/count/index, period and sample count) |
| 260–271 | Existing 12-byte clock configuration codec |
| 272–309 | Fit epoch/rate/RMS/uncertainty/counters, count/inliers/readiness/applied/fallback/compensation |
| 310–326 | Two capture ages, existing RX status words, evaluation uptime and admission-mismatch flag |
| 327–367 | Five signed64 production intermediates and an explicit captured flag |
| 368–380 | Original signed32 DTU/mm, status, clock-valid, rate-state, rejection reason, provenance |
| 381–383 | Reserved0 |

Each observation preserves full source and receiver clock identities, sequence,
TX, raw RX, corrected RX, generation, acceptance uptime, power estimate/delta/status/
table, validity, remote-epoch-known and sequence-known flags. TX belongs to that
ALR's native hardware clock; both RX markers belong to the RLR hardware clock.
Stale cached observations may remain in absent slots: only `received_this_cycle`
and the seen mask identify this cycle's actual reception. Missing observations
are not encoded as fabricated timestamps.

The complete relation includes reference/follower origins, mapping epoch, signed
skew ppb (follower relative to reference), reference and TWR sequences, origin/
delay/remaining/max ages, delay DTU, availability, power-corrected TWR flag, relation
version, delay state, external-pose flag and advertised period. Its two origins
have explicit reference/follower clock domains. `received_this_cycle` means it was
decoded from this cycle's follower packet; absent followers do not supply a usable
current relation. Provenance and validity are separate: an unavailable relation's
flags are not evidence of an applied correction.

Captured intermediates are TX separation in the reference domain, raw and corrected
arrival separations in that domain, raw TDoA and corrected TDoA. They are the actual
production return values. An early unavailable result may leave intermediate fields
zero; the downlink/result status controls interpretation. Admission failures that
never reach downlink have `intermediates_captured=false`. Converted follower TX is
reference TX plus TX separation modulo2^40 when the relation is valid. Raw RLR arrival
separation is the signed native40 difference of the two captured raw RX markers.
The validator reports both reconstructed separations using production C.

`calculation_revision` identifies the original conversion model at core
`506913dc676d0a3c0d43c48977ef11be2085aa34` and STM32 6000ms propagation-age build.
The validator checks the six conversion-source SHA256 hashes against that baseline
and verifies the complete current vendor manifest before compilation. The new
estimator does not reinterpret historical logged rates. It does not
trust hash equality as full schedule proof: the full local plan/schedule bytes are
retained, including roster, period, spacing, timing budgets, priorities and rates.
This captures the receiver's configuration; it cannot prove an ALR's undisclosed
complete stored plan from the on-air fingerprint. Preserve the operator's full
read-only plan dumps and firmware revisions for all nodes as experiment metadata.

### Baseline replay and intervention scope

`validate_tdoa_replay.py` reads compact JSONL, checks domains/configuration/integrity,
reconstructs the exact evaluation state, and compiles a temporary host oracle from
the matching STM32 checkout. The oracle uses the production receiver admission/
evaluation code, relation conversion, modular timestamps, integer rounding and
DTU-to-mm calculation. It compares DTU, mm, status/reason, clock validity, rate state,
provenance and captured intermediates; separately recorded vectors are also checked.
No floating-point approximation or alternate production estimator is introduced.
Missing-reference/follower/rate/relation cases remain replayable invalid results.
There is no replay snapshot for a completely unobserved cycle: the receiver itself
never starts that cycle. Inspect cycle/time gaps as UWB absence, not transport proof.

`baseline_passed` describes exact arithmetic/status replay. `passed` additionally
requires no incomplete assembly; `replay_complete` requires all captured vector
pairs, current replay/vector association, contiguous current diagnostic IDs and
zero observed diagnostic-drop increments. `transport_complete` separately describes
all transport traffic, including redundant optional raw packets. A dataset may have
complete baseline replay inputs despite optional raw-trace losses. These losses
remain in `transport_missing`; the validator never reclassifies them as filtering.

Loss scopes matter:

| Counter | Scope |
|---|---|
| Diagnostic ID and wire diagnostic drops | Boot-cumulative; IDs advance for dropped snapshots; configure/reset purges add to drops |
| STM32 health `dropped` | USB plus Wi-Fi queue drops and encode failures, cumulative since runtime diagnostic reset; not a Wi-Fi-only loss count |
| STM32 `tx_dropped` | UART pending-slot drops since boot/diagnostic reset |
| ESP `bridge_dropped`, corrupt, UART errors | ESP boot-cumulative, including disconnected purges/discards |
| `transport_reports` | Capture-local count of gap rows plus counter-report rows; not a lost-message count |
| `transport_missing` | Capture-local missing shared sequence numbers, detected before compact filtering |
| Validator `diagnostic_dropped` | Observed delta from the first captured counter sample; first value is a baseline |
| `diagnostic_dropped_boot_last`, `diagnostic_counter_samples` | Cumulative last samples and per-session baseline/last/delta; startup history is explicit |
| `diagnostic_id_gaps` | Current measurement window; historical queued blobs are listed separately |

The first counter observation cannot assign losses before that observation to this
capture. Counter wrap/reset is handled explicitly. Full snapshots whose acquisition
uptime is more than two periods before the first captured vector are reported as
historical backlog; every such snapshot is still mathematically replayed. This does
not discard invalid calculations or remove a current missing pair. Missing initial/
final pairs are annotated as possible boundaries, not silently certified complete.

Compact filtering omits native raw rows only AFTER shared-sequence accounting, so it
does not create sequence gaps. New gap rows retain the exact missing sequence range.
Old compact logs lack these ranges and discarded payloads: their lost message types
cannot be reconstructed exactly. No TCP/collector bug can be inferred from a gap alone.

The 2026-10-28 physical smoke capture in
`uwb_logs/replay-smoke-20261028T125627/20261028T115627597356Z-robot-0-compact.jsonl`
contains120 exact baseline matches. Cycles68/69/70 were ancient queued snapshots;
current cycles are283..402, with no completed cycle/diagnostic ID for290 or353. The historical ID
jump70->284 accounts for213 skipped IDs; the diagnostic-drop counter stays223,
so its observed capture delta is zero. Bridge counters stay at1 drop/0 corrupt/
0 UART errors; `tx_dropped` stays1. There are115 real gap events/207 shared sequence
numbers missing. The original file cannot identify those lost payloads exactly.
Paced host reproduction isolates pre-bridge optional raw AA FIFO shedding (with the
same losses when replay is disabled); it is not a compact-filtering artifact.

Cycle283 has a vector but no snapshot; the full ancient FIFO explains the startup
admission failure. It is at the capture boundary, not proof of ongoing replay loss.
The partial cycle67 contains parts1/2/3 and was already incomplete at connection
opening; `connection_end` was only the delayed flush time. There is no incomplete
final current snapshot. The physical file still fails strict completeness after
corrected accounting; it is never rewritten or retrospectively repaired.

Reference TX/RLR RX, acceptance time, generation, mapping resets and exact applied
clock/fit context support future receiver-only estimator comparisons. The intervention
holds the actually received on-air follower relation and propagation metadata fixed.
Changing the ALR estimator on air also changes that relation; its full historical fit
inputs are not captured here. That larger intervention cannot be inferred from this
dataset. Existing clock fit windows are not serialized as internal arrays: baseline
TDoA replay uses the exact applied mapping; shadow estimators must warm up on recorded
observations and exclude missing transport intervals rather than inventing history.

### Software verification and operator checkpoint

Software-only commands (no serial/device/RF access):

```sh
cd ~/Documents/epuck-stm32
bash tests/runtime_config/run.sh
bash tests/esp_link/run.sh
bash tests/uwb_policy/run.sh
bash tests/epuck_console/run.sh
bash ThirdParty/mul_core/tests/run.sh
cd ~/Documents/epuck-esp32
bash tests/run_host.sh
python3 tools/verify_tdoa_replay.py
python3 tools/verify_tdoa_replay.py --paced
```

The last command generates actual STM radio/runtime/UART fixtures and runs them
through the actual ESP owner and TCP loopback, disconnect/reconnect, existing compact
collector and production C oracle. It accepts no physical endpoint. The 45-cycle
fixture gives 45/45 exact DTU/mm/status matches, 43/43 available captured-intermediate
comparisons, zero observed gaps/drops and `replay_complete=true`. Other tests cover
malformed/unknown-version frames, fragment loss, integrity/association conflicts,
bounded reassembly, unavailable inputs, wrap, signed rates/TDoA, propagation delay,
queue pressure and unchanged vectors with instrumentation OFF versus ON.

The paced mode simulates200 seconds with60 seconds disconnected,27 ms UART frames,
AA bursts and real STM queues, then the real ESP owner/TCP reconnect/collector. It
checks the complete boundary-cut file fails strict completeness and its explicitly
selected synthetic interior has140/140 exact pairs, zero current diagnostic ID gaps/
drop increments, and complete replay despite254 optional/auxiliary sequence losses.
The producer identifies252 raw type12 losses plus one summary and one quality row.
At the recorded86ms status-report phase it instead identifies128 raw,127 quality and one summary
loss, with identical results when replay is OFF. The fixture uses the exact full
local plan fields (revision23, fingerprint826414CC), not merely hash equality.
The disabled replay fixture has identical shedding: no FIFO enlargement, UART-rate
change or speculative prioritization is applied. UART average bandwidth is below
capacity; short AA bursts exceed the four-line optional FIFO's instantaneous budget.

STM32 build/image and RAM/stack details are in
`../epuck-stm32/docs/hybrid_uwb.md` (replay diagnostics). Both trace ON/OFF Release
builds and reserved-configuration-sector checks pass. ESP32 build succeeds with
854288-byte application binary in its 1048576-byte app partition, image sections
854168 B, `.data + .bss` 53343 B; IDF reports DIRAM120874 B including executable RAM.
The additive frame validator uses a bounded152-byte buffer and adds no persistent
queue. Runtime whole-stack high-water, CPU latency and physical RF availability are
not established by host tests. Existing unrelated compiler/SDK warnings remain.

For the human operator only; none of these device operations was executed during
implementation:

1. Build STM32 using the exact `build/Universal` command in its guide, with
   `-DMUL_TDOA_REPLAY=ON`. Build ESP32 using the existing installed SDK:

   ```sh
   cd ~/Documents/epuck-esp32
   source ~/.espressif/tools/activate_idf_v6.1.sh
   python3 "$IDF_PATH/tools/idf.py" build
   ```

2. Use the established ST-Link procedure, selecting the replay ELF and preserving
   configuration sectors; use the existing identified ESP port, not a guessed port:

   ```sh
   cd ~/Documents/epuck-stm32
   source ~/.espressif/tools/activate_idf_v6.1.sh
   openocd -s "$OPENOCD_SCRIPTS" -f interface/stlink.cfg -f target/stm32f4x.cfg \
     -c 'program build/Universal/EPuck_STM32.elf verify reset exit'
   cd ~/Documents/epuck-esp32
   python3 "$IDF_PATH/tools/idf.py" -p "$ESP32_PORT" flash
   ```

   `$ESP32_PORT` must be identified by the operator using the existing procedure in
   `docs/disconnect_diagnostics.md`. Preserve the current plan, RF state, calibration,
   Wi-Fi credentials and estimator. For the estimator refactor, ALRs also require the updated LPS image; see its
   `docs/mul_commands.md`. No plan rewrite is required.
   Confirm full plan fields on each node; fingerprint826414CC alone is insufficient.

3. In the existing stationary campaign, confirm telemetry/Wi-Fi are already enabled
   and the generated plan selected. Start the existing collector on the established
   RLR endpoint; choose a new output directory to retain prior experiment files:

   ```sh
   cd ~/Documents/epuck-esp32
   timeout --signal=INT 600s python3 tools/uwb_collect.py \
     --robot 192.168.1.141:3333 --output ~/Documents/uwb_logs/replay-next --no-live
   ```

   Use `--metadata YOUR_EXISTING_METADATA.json` to retain ground truth, full plan
   dumps and firmware/image identities. The stationary geometry is AA2 m, AR1 m
   each, expected range difference0 mm; this is comparison truth, not calibration.

4. Inspect the new file for `"type":"tdoa_replay"`, approximately one pair row per
   observed cycle including invalid results. Check `diagnostic_dropped`,
   `tdoa_replay_incomplete`, `gap`, `transport` and `error` records. Require zero
   unexplained losses for estimator comparison; stop and inspect backpressure if
   counters grow. Verify unchanged RF cadence, availability, radio error rates and
   service/stack headroom before the full campaign. Match source/receiver domains,
   schema and complete local configuration; do not infer them from host timestamps.

5. After capture, set `RECORDING` to the exact new compact JSONL path printed by the
   collector and run:

   ```sh
   python3 tools/validate_tdoa_replay.py "$RECORDING" --sanitize
   # Optional detailed per-cycle production intermediates:
   python3 tools/validate_tdoa_replay.py "$RECORDING" --details
   ```

   Require `passed=true`, all reconstructed outputs exactly matched, empty mismatch/
   invalid/missing lists, and inspect `replay_complete`, `transport_complete`, counter deltas and capture-boundary
   losses. Optional raw gaps alone do not invalidate complete snapshot inputs. Archive the checkout/image identities with the capture. Legacy captures
   without type15 remain readable but cannot acquire missing replay inputs afterward.

Software status: **READY FOR HARDWARE VALIDATION**. Physical latency, stack watermark,
UART margin and unchanged RF performance remain operator validation checks.


### Median-5 / Kalman compatibility

New clock type10 version3 uses method0=Median-5 or2=Kalman, fixed history5.
`applied_estimator` explicitly reports `median_5`, `kalman` or `unavailable`.
Fallback0/1/2/3 means used/warmup/uncertain/numerical. The former RMS slot carries
absolute innovation for Kalman: JSON exposes `innovation_abs_dtu` and leaves
`residual_rms_dtu` null. Conditional Kalman covariance is not a physical bound.
Historical clock versions1/2 retain their regression labels and RMS meaning;
there is no production regression estimator in the new core.

Type15 and its384-byte snapshot use schema2 with the same fragment sizes/offsets
and complete raw inputs. Schema1 recordings still decode. The production-C
validator restores the logged mapping and applied rate; it never re-estimates a
historical clock with today's selected filter. It verifies the current vendor
manifest plus baseline conversion-source hashes, allowing the estimator to change
without silently changing historical TDoA math. Both consumers pin core
`bdfe0a10a41368efb5592466be266142e9938d09` from the LPS canonical package.
The ESP firmware change is limited to the shared clock-envelope validator accepting
version3; forwarding, control behavior, queues and Wi-Fi are unchanged.

Portable verification (no devices):

```sh
cd ~/Documents/epuck-esp32
bash tests/run_host.sh
python3 tools/verify_tdoa_replay.py --paced --kalman
python3 tools/validate_tdoa_replay.py ../uwb_logs/replay-full-20min/20261028T130040462965Z-robot-0-compact.jsonl --sanitize
```

The host suite includes old/new envelope decoding, persisted selection,45 new exact
Kalman pairs, queue overflow and reconnect boundaries. The paced200-second virtual
capture verifies140 complete interior pairs and explicitly reports the partial
opening boundary. Its intentionally filtered auxiliary sequence gaps are retained
as transport accounting, not hidden or confused with missing replay inputs.
The old20-minute capture remains1190/1190 exact matches, complete, no mismatches.
No physical RF accuracy, embedded deadline margin or stack watermark is established.
Use the operator-only programming procedure above with the Universal ELF and updated
ESP binary, preserve saved plans/calibration, then select each estimator only with
RF OFF and record firmware revisions and selected method for all nodes. The ALRs
need the updated LPS firmware as well. No device operation was run for this refactor.

## Per-device antenna calibration and availability audit

LPS mode5 now uses a per-device EEPROM TX/RX pair. Legacy absence retains
TX14498/RX18453. The journal occupies384..439, in two32-byte slots after the
existing plan slots; the old100-byte image and both plan slots are untouched.
A CRC32 protects each versioned pair; an inactive-slot commit-last write plus full
readback admits either the previous pair or the new pair after power loss.
Read/verification failures block RF until recovery/reboot; malformed committed
calibration does not trigger factory erase. No-op writes avoid EEPROM wear.
`cal reset` explicitly commits the legacy pair; general configuration reset does
not silently erase calibration. All commands require acknowledged RF OFF.

Operator-only LPS commands (existing USB console, optionally colon-prefixed):

```text
:off
:cal show
:cal set tx <0..65535> rx <0..65535>
:cal show
```

Successful settings persist immediately. `:on` reapplies both hardware registers
and the TX driver cache during mode5 initialization. Readback validation checks
against the selected pair, including after delayed-TX setup. `:cal reset` restores
legacy antenna values explicitly. Hardware timestamp formulas are unchanged.

STM32 already provides persistent `cfg set tx_ant_dtu N` and
`cfg set rx_ant_dtu N`, with0..65535 validation. Its existing `cal show` also
reports an **ADS-only additive** calibration; that additive parameter must not be
mistaken for hardware antenna delay or applied to passive TDoA. No duplicate
STM32 antenna parameters were added. Set both existing parameters with RF OFF,
inspect `cfg show saved`, and only then resume with `on`. Antenna changes disable
the old additive calibration through its existing workflow; retain the historical
values separately, do not silently restore or reinterpret them.

Physical procedure, performed by the operator only:

1. Record firmware revisions, full saved plans, TX/RX registers, additive-calibration
   state, power policy and RF state for132/133/141. Preserve these as the rollback
   baseline. Use the named stationary profile on both ALRs and Universal on STM32.
2. Fix all three nodes in LOS, same antenna orientation/height. Measure antenna
   phase-centre separations for132–133,132–141,133–141 independently. Do not infer
   distances from TDoA or these captures. Avoid very short/saturated links.
3. Collect at least30 successful TWR ranges per pair, keeping addresses, channel5,
   power policy and antenna registers unchanged. AR at0.05Hz needs about10minutes
   per30 samples; the3-minute availability check below is not a calibration data
   substitute. Use one direction/policy consistently and retain raw/power metadata.
   Exclude failed exchanges, not inconvenient successful ranges. Investigate
   multipath/outliers before trusting a fitted offset.
4. Supply a JSON file to `python3 tools/antenna_calibration.py measurements.json`:
   Set `additive_correction_removed:true` only after ensuring ADS additive
   correction is disabled or subtracted from the supplied ranges. `devices` maps string IDs `132`,`133`,`141` to **actual current** integer `tx`
   and `rx`; `pairs` has exactly three objects containing `nodes:[132,133]` (and
   the other pairs), measured `known_m`, successful `ranges_m:[...]` and one
   consistent `power_policy` (`applied`, `raw` or `unavailable`). This read-only
   helper prints proposals, sample counts, medians and MAD; it never sends commands.
5. Review proposals, then use RF-OFF commands above on each node to apply both
   directions. Reboot and read back each pair before RF resumes. Do not overwrite
   old calibration files. Recollect at a **different independently measured
   distance** and preferably another orientation/power level before acceptance.

The assumed model is `range_bias_ij = c/(2*f_DTU) * (delta_A_i+delta_A_j)`,
where A is aggregate TX+RX hardware delay. Define `e_ij=(median_range-known)*f_DTU/c`;
then `delta_A_i=e_ij+e_ik-e_jk`. Positive excess range therefore increases programmed
aggregate delay. The helper rounds aggregate DTU and splits the **increment**44/56
between TX/RX, preserving the historical baseline when the increment is zero.
Three pairs identify aggregate node corrections only under reciprocity and
constant-bias assumptions; they do not identify six independent TX/RX delays.
The44/56 split is an initial assumption, especially unverified for DWM3000, not a
measured calibration. Receive-power bias, NLOS, clock residuals and phase-centre
error can all contaminate the fit; changing power/distance can invalidate it.
No calibrated values were inferred from the existing stationary tests.

Read-only loss diagnosis:

```sh
python3 tools/tdoa_availability.py ../uwb_logs/clock-tests/*/*.jsonl
```

This separates unavailable follower relations, explicit v2 propagation expiry,
receiver-clock invalidity, reference/follower packets not observed, and gaps in
host transport. V1 unavailable relations clear their delay fields: a row alone
cannot prove whether expiry or follower-clock failure caused unavailability.
Missing RF observations also do not prove antenna-level loss. Diagnostic drop
increments are capture-local differences; boot cumulative counters and intentional
transport filtering are not treated as missing TDoA packets. Use the production
replay validator separately for integrity/completeness and exact mathematical replay.

Three-minute validation after operator programming:

1. Keep stationary geometry/calibration, plan826414CC, AA1Hz/AR0.05Hz,10ms spacing
   and all timing budgets unchanged. Confirm both ALRs report CH5, age6000,
   speed0/relation v2; confirm STM32 stationary rate policy and selected estimator.
2. Begin with Median-5 on follower and receiver. Capture180seconds of JSONL and
   both ALR USB logs, starting before RF resumes if possible. Exclude documented
   startup warmup from a separately reported steady-state fraction, not from the
   raw capture. Expect approximately180 vectors and9 AR periods per link.
3. Run `tdoa_availability.py` and `validate_tdoa_replay.py ... --sanitize`. Require
   no systematic cycle-ending-in1 holes, v2 fresh/reused relations across AR cycles,
   no unexplained in-capture replay losses, and exact matches without mismatches.
   Target the previous approximately96.8% valid fraction (report numerator and
   denominator);180 samples are a smoke check, not a statistical guarantee.
4. Repeat with Kalman using the same RF-OFF workflow. Inspect `rst` deltas and
   `rr` last-reason codes; a compensation reset on every AR boundary is a failure.
   Numerical resets, stale acceptance or scheduling/deadline changes require
   investigation. Record stack high-water and service latency before long runs.

Nothing in these procedures was executed on physical hardware by the assistant.


Observed October29 vector availability was130/180,144/175,126/174 and156/179
(72.2%,82.3%,72.4%,87.2%). All four use relation v1; the October28 baseline uses
v2 with6000ms lifetime. All captured cycle-ending-in1 vectors are invalid. Test4's
23 invalid vectors have both RF beacons present and an unavailable follower
relation. Restoring the explicit stationary lifetime addresses this demonstrated
failure path; it does not promise the same physical success fraction.

All complete snapshots still replay exactly:179,174,173,178 matches respectively,
no mismatches. None of the four files is replay-complete: Tests1/4 have possible
closing-boundary omissions; Test2 lacks fragment3 of opening-cycle53 (initial
boundary possible); Test3 lacks fragment3 of interior cycle105. These are separate
from RF/TDoA availability. Capture-local diagnostic-drop increments are zero;
sequence gaps are23/26/37/26 and may include intentionally filtered auxiliary
traffic. Do not label all of those gaps as lost TDoA. No speculative forwarding
optimization or counter suppression was implemented.

### Bounded clock recovery comparison

Shared core08db4d8 changes KF recovery only; ESP forwarding, queues and wire schemas
are unchanged. Existing raw sequence/TX/RX, mapping age/status, fit readiness,
fallback and reset metadata remain compatible. STM32 CLOCK text adds recovered/
holdover; LPS KF text adds rc and loss text hold. A retained model never authorizes
a stale mapping. Exact logged baseline validation still uses recorded rates.

For a deliberate receiver counterfactual, run:

```sh
python3 tools/compare_clock_recovery.py ../uwb_logs/clock-comparison/kalman/20261030T144807019903Z-robot-0-raw.jsonl --alr ../uwb_logs/clock-comparison/kalman/alr133-kalman.log
python3 tools/compare_clock_recovery.py ../uwb_logs/clock-comparison/median-A/20261030T143930318029Z-robot-0-raw.jsonl
```

The tool compiles old4060df1 and current canonical C on identical complete
snapshots and asserts exact Median output equality. The optional follower replay
uses real TX/RX with inferred1Hz coarse time, **without propagation compensation**;
it does not reproduce follower hardware/TWR behavior or establish accuracy.
Receiver replay cold-starts at capture entry and holds the recorded follower
relations fixed. It does not synthesize absent references, relations or snapshots,
use host reception time as UWB time, or infer ground truth from measurements.
Use its scope labels rather than equating snapshot results to all294 physical
cycles. Detailed numerical results, resource limits and operator-only hardware
checks are in `../lps-node-firmware/docs/mul_experiment_campaign.md`.

The physical Kalman capture's263 complete snapshots give224→225 valid receiver
counterfactual pairs,8→4 filter resets,27→14 unapplied evaluations, and zero change
on224 common valid TDoAs. Median-A's285 snapshots give264→264 valid and1→1 resets.
All old/new Median results match. This supports a bounded recovery improvement,
not a measured physical RMSE improvement. The ordinary exact replay validator
still matches1190/1190 October28 results, and the paced actual STM/ESP/TCP/collector
Kalman fixture matches140/140 interior results with boundary/loss accounting intact.
