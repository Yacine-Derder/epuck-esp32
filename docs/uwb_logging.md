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
Canonical core pin a73b7c5904dcf8088e713c69be3136410357e2f4 is a local unpublished
commit; checked-in snapshots verify/build offline without fetching or pushing.

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

Software validation covers actual onboard USB-result/telemetry equality, fixed
record length, invalid/sentinel/status validation, native40/uint32 precision,
STM UART/control priority and overflow, real ESP parser/socket forwarding, C→Python
vectors, compact/raw schemas, metadata deduplication, slow display isolation,
loopback reconnects/heartbeats, directory creation, migration hashes/collisions.
The shared-core sanitizer suite, STM runtime/configuration suite and ESP host
suite passed (72 Python tests plus C protocol/transport tests). Both firmware
builds passed; the STM ELF layout check passed. STM usage is 36,552 bytes RAM
and 128,776 bytes flash.

No physical robots were flashed or contacted. Sustained Wi-Fi packet delivery,
UART faults, RF timing and stationary metric accuracy still need operator tests.
Queues remain4 records per STM sink and8 on ESP; every association gets a publish
attempt ahead of its raw frame, but a saturated/disconnected transport can drop
records, with existing counters. At1Hz this adds310 UART bytes/s (~2.7% of115200
baud8N1 capacity), not another raw UWB frame or a new blocking radio operation.
