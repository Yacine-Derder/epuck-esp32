# Configurable e-puck sensor streaming

Normal streaming now defaults to1Hz with light-free DATA2 Wi-Fi telemetry.
Use compatible STM32 and ESP32 bridge builds and the updated Python client;
no dsPIC firmware update is needed. UWB-only experiments disable ESP entirely.
No hardware was flashed by Codex.

## Defaults and commands

`Core/Inc/app_config.h` in epuck-stm32 now contains:

```c
#define APP_EPUCK_STREAM_PERIOD_MS 1000U
```

Use 0 to default to stopped automatic DATA, or an integer 50..10000 ms.
The default is1000ms (1Hz). Feature defaults are unchanged:
EPUCK=1, ESP=1, UWB=0. This is a boot default; runtime commands are not saved
across STM32 resets. No periodic `R\n` requests remain, including when stopped.
PING continues once per second, with an additional PING after configuration
for acknowledgement ordering.

USB console and Wi-Fi both accept these exact commands:

```text
STREAM 100
STREAM 50
STREAM 0
STREAM_STATUS
```

Periods are milliseconds: 100 requests 10 DATA records/s; 50 requests 20/s;
0 requests no automatic DATA. Missing/extra arguments, negative numbers and
values outside 0 or 50..10000 are rejected. USB keywords are uppercase; the
Python client also accepts lowercase/mixed-case keywords and normalizes them.
Streaming configuration is independent of motor ownership. Either interface
may configure it while the other owns motors; it never ARMs, acquires motor
ownership, or refreshes either motor watchdog or CONTROL_HB.

## Launch the client

```bash
cd /home/derder/Documents/epuck-esp32
python3 -m venv .venv
.venv/bin/python -m pip install -r tools/requirements.txt
.venv/bin/python tools/wifi_client.py 192.168.1.35 --ui
```

Wait for SESSION READY, then type each desired command above and press Enter.
`help` lists the commands. `--logs` shows optional LOG messages and `--verbose`
also displays motor heartbeat acknowledgements, which remain hidden by default.
For a dependency-free line-oriented client:

```bash
python3 tools/wifi_client.py 192.168.1.35 --plain
```

At 20 Hz, DATA replaces the latest sensor slot; it does not fill event history.
Rendering remains throttled to 10 Hz and is separate from the network thread;
intermediate screen frames may be coalesced. The wire still forwards newly
accepted samples, subject to the existing bounded queues and fault safeguards.
No cached sample is relabelled as a new record or repeatedly forwarded after a
successful enqueue. The client sends CONTROL_HB every 100 ms only after READY.

## Acknowledgements and recovery

Example Wi-Fi or USB responses:

```text
[STREAM] desired=50 ms queued locally; not acknowledged
[STREAM] OK STREAM acknowledged=50 ms desired=50 ms
[STREAM] desired=50 pending=0 in_flight=0 acknowledged=50 ack_valid=1 confirmed=1 attempts=0 recovering=0 waiting_ack=0 fence_pending=0
[STREAM] age_ms=24 records=120 stale=0 late_ack=0 (age=4294967295 means no sample)
```

Desired, in-flight and last acknowledged periods are separate. `ack_valid=0`
means the actual peer setting is uncertain. `confirmed=1` requires an actual
OK STREAM for the desired period; a local queue message is never confirmation.
`in_flight=0` can mean either a stop request or no request: inspect
`fence_pending`. A confirmed setting may still have `fence_pending=1` while
older responses drain before the next STREAM is allowed. `waiting_ack=0`
also occurs after a timeout; it does not imply success.

The scheduler sends STREAM only after receiving PONG. STOP, due motor HB,
ARM and motor speeds have higher priority than STREAM and PING. The active
transmit buffer remains intact until UART completion. Multiple configuration
changes coalesce to the newest desired setting, while the active request and
its response destination remain immutable. Acknowledgements go to the USB or
Wi-Fi session that requested that particular transmitted setting, even if a
newer request came from the other source.

OK STREAM carries no period or request ID. The inspected existing dsPIC code
queues whole replies in command order. STM32 therefore sends a PING after
each STREAM and waits for its ordered PONG fence before sending any subsequent
STREAM. It counts all sent PINGs and received PONGs; dropped PONGs delay a fence
instead of allowing early completion. A long outage with many lost PONGs can
conservatively delay subsequent configuration/recovery until that response
count catches up. The implementation favors an unconfirmed/waiting state over
attributing an old acknowledgement to a new period. This ordering mechanism
depends on the existing FIFO reply behavior; it is not safe for a different
peer that reorders or manufactures PONGs/acknowledgements.

The STREAM acknowledgement deadline is 500 ms. A late reply is counted and
ignored, rather than confirming a newer request. For enabled streaming, failed
requests retry at most three times per batch (100 ms between attempts, each
after its fence); another batch may start after a 5 s cooldown and a fresh
PONG. While a fence is unresolved, PING continues but STREAM does not overlap.
In normal mode a `STREAM 0` request gets one attempt; if unacknowledged it remains uncertain,
without automatic retries or fallback polling. Explicitly enter STREAM 0 again
to retry. A subsequent positive-period command enables recovery again.

Staleness uses **3 × desired period + 500 ms**, measured from the newer of the
configuration acknowledgement or latest accepted DATA. Thus a valid 10 s
period is not incorrectly treated as stale after a short quiet interval.
Confirmed enabled streaming is re-established after stale DATA, an observed
BOOT record or a UART receive fault, with fresh PONG readiness and the same
ordering safeguards. Disabled streaming does not run stale-data reconfiguration.
Recovery never re-arms motors. Optional verbose USB diagnostics retain
counters and add completed `stream_tx` plus streaming state; default output
is quiet apart from important faults and explicit status queries. STREAM_STATUS
also reports latest sample age, accepted-record count and stale status;
`4294967295` age means no sample. Getter APIs are in epuck_link.h.

DATA parsing is unchanged: exactly 22 typed numeric fields from the e-puck,
but Wi-Fi publishes compact DATA2 without the eight light values, adding
STM32 sequence and reported age. Latest samples are coalesced at most1Hz,
independently of a faster requested raw acquisition rate. Readings
are raw. Commanded motor steps are not measured wheel motion. Accelerometer
readings require JE1 bridges 33–34, 35–36 and 37–38. **Sensor transmission rate
does not necessarily equal physical sensor sampling rate**; repeated raw values
can occur even in distinct accepted records.

## Build and tests

```bash
cd /home/derder/Documents/epuck-stm32
cmake --preset Debug
cmake --build --preset Debug
tests/epuck_console/run.sh
tests/esp_link/run.sh
cd /home/derder/Documents/epuck-esp32
source .venv/bin/activate
tests/run_host.sh
```

STM32 normal ELF: `build/Debug/EPuck_STM32.elf`. For the UWB-first profiles,
exact build commands and CubeProgrammer ELF paths are in the canonical
[LPS guide](https://github.com/Yacine-Derder/lps-node-firmware/blob/master/docs/mul_core.md#next-user-experiments--uwb-first).
Normal DATA2 requires the updated ESP bridge (`idf.py -B build/UwbReview build`
after activating ESP-IDF6.1); no dsPIC flash. Generic STREAM/STREAM_STATUS
commands retain the63-character bound and session filtering. Legacy DATA is
still accepted by the bridge/client. Hardware timing remains pending.

Default USB diagnostics suppress routine arrays and HB ACKs; use LINK_STATUS,
STREAM_STATUS and MOTOR_STATUS for explicit snapshots. APP_DEBUG_TELEMETRY=1
restores verbose diagnostics in normal mode. MUL_UWB_ONLY=ON forces ESP off,
stream0 and quiet telemetry, issues startup STOP, rejects ARM/movement and
stream changes, and keeps watchdog/STOP handling active. Only in this mode,
an unacknowledged idempotent STREAM0 may retry after5s and a fresh PONG fence.
Normal ordered-ACK recovery rules above remain unchanged.
