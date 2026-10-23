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
ALR radio firmware, packet formats, TDMA, TWR and synchronization are unchanged.
Canonical core pin a73b7c5904dcf8088e713c69be3136410357e2f4 remains unchanged;
checked-in snapshots verify/build offline without fetching.

Build (these commands do not program robots):

```sh
cd ~/Documents/epuck-stm32
cmake -S . -B build/Universal -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake \
  -DCMAKE_BUILD_TYPE=Release -DMUL_UNIVERSAL=ON -DMUL_UWB_ONLY=ON \
  -DMUL_TWR_ROLE=0 -DMUL_DIAG_ROLE=0 -DPHY_INTEROP_ROLE=0 \
  -DMUL_HYBRID_SLOT_MS=500 -DMUL_TIMING_TRACE=ON -DMUL_ADS_TRACE_TIMES=ON
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
