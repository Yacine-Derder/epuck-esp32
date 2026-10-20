# Wi-Fi robot link protocol

Wi-Fi modem sleep is disabled and the application/second-stage bootloader
console uses USB Serial/JTAG exclusively, keeping UART1 GPIO43/44 for STM32.
See [latency configuration and hardware comparison](docs/wifi_latency.md)
for power tradeoffs, defaults/effective-config checks, build/flash/monitor
commands, and the ping/client comparison. This addresses suspected causes;
hardware verification is still required.

## Interactive Wi-Fi terminal

Install the optional terminal dependency in a local virtual environment:

```bash
cd /home/derder/Documents/epuck-esp32
python3 -m venv .venv
.venv/bin/python -m pip install -r tools/requirements.txt
.venv/bin/python tools/wifi_client.py 192.168.1.35 --ui
```

The full-screen interface keeps command editing separate from a live raw
sensor panel and the last 200 events. Enter sends a command; PgUp/PgDn browse
events while input retains focus. Up/Down navigate the last 100 commands;
arrows, Home/End, Backspace/Delete and selection retain their normal behavior.
Resizing keeps the command row at the bottom. Keywords accept either case (`arm`,
`m 200 200`, `stop`, `help`, `motor_status`, `stream 100`, `stream_status`). Blank input is ignored. Local
queue confirmations explicitly say **not acknowledged**; actual motor
acknowledgements/errors appear when received. No command is accepted before
SESSION READY and no movement is replayed after reconnecting.

```bash
.venv/bin/python tools/wifi_client.py 192.168.1.35 --logs
.venv/bin/python tools/wifi_client.py 192.168.1.35 --verbose
.venv/bin/python tools/wifi_client.py 192.168.1.35 --logs --verbose
python3 tools/wifi_client.py 192.168.1.35 --plain
python3 tools/wifi_client.py 192.168.1.35 --plain --logs > wifi-session.log
```

`--logs` includes optional LOG records; `--verbose` also shows routine
successful `[MOTOR] OK HB` records, hidden by default only at the display layer.
Plain mode needs no third-party dependencies and is selected automatically
for redirected input/output or unsupported terminals. Small terminals keep the
UI; missing UI dependencies produce an installation error instead of silently
switching to scrolling output. `--ui` requires the persistent interface.
Ctrl-C/Ctrl-D (EOF) exits with best-effort STOP; a network disconnect exits promptly even if no input is being typed. See
[client interface details and tests](docs/wifi_client.md) for age semantics,
display limits, and terminal requirements.

Architecture: computer TCP/Wi-Fi → ESP32 UART1 → STM32 USART3 → e-puck
USART1. The STM32 owns all robot parsing, motor ownership, acknowledgements
and watchdog decisions. ESP32 only transports records. E-puck firmware is
unchanged. All existing pin assignments and 115200 8N1 configurations remain.

## Sensor streaming commands

Build compatible STM32 and ESP32 firmware for compact DATA2 telemetry; the
Python client runs directly. The dsPIC firmware remains unchanged. Wait for READY, then type:

```text
STREAM 100
STREAM 50
STREAM 0
STREAM_STATUS
```

100 ms is 10 Hz; 50 ms is 20 Hz; 0 stops automatic DATA without falling back
to polling. Valid periods are 0 or 50..10000 ms. Configuration does not acquire
motor ownership. A queued response is not an acknowledgement. See
[streaming configuration and recovery](docs/sensor_streaming.md) for the STM32
boot setting, state fields, timeout/retry rules and tests. Sensor transmission
rate does not necessarily equal physical sampling rate.

## Framing and sessions

Records are printable ASCII terminated by LF; CRLF is accepted on input.
TCP commands are at most 63 characters, UART transport records at most 318
characters. Reject control characters, embedded CR, oversized records and
invalid framing. TCP commands are always wrapped, never treated as transport
framing. Thus a client typing `OPEN`, `CLOSE` or `LOG` cannot inject a session
boundary. STM32 validates commands again after unwrapping.

| Direction | Record | Meaning |
| --- | --- | --- |
| ESP32 → STM32 | `OPEN <id>` | New session boundary; cancel any old Wi-Fi motor session. |
| STM32 → ESP32 | `SESSION <id> READY` | Transport ready; not armed. |
| ESP32 → STM32 | `CMD <id> CONTROL_HB` | Client-originated keepalive; never arms or moves. |
| ESP32 → STM32 | `CMD <id> <command>` | HELP, MOTOR_STATUS, ARM, M left right, STOP. |
| ESP32 → STM32 | `CLOSE <id>` | Client/Wi-Fi/control-queue loss; invalidate session. |
| STM32 → ESP32 | `SESSION <id> LOST` | Invalid framing, UART loss, response overflow or client-heartbeat expiry. |
| STM32 → ESP32 | `DIAG <id> <fields>` | Best-effort invalidation diagnostics; also available on STM32 USB. |
| STM32 → ESP32 | `RSP <id> <text>` | Source-specific command response, acknowledgement or error. |
| STM32 → ESP32 | `DATA2 ...` | Compact telemetry; legacy DATA also accepted, described below. |
| STM32 → ESP32 | `LOG <text>` | Debug/Qorvo output; never a command or telemetry record. |

IDs are 16 lowercase hexadecimal characters, generated from 64 random bits
on each TCP accept. ESP32 accepts one connection and rejects competitors.
It waits for matching READY before forwarding client commands. Unknown/stale
IDs cannot refresh heartbeats or control motors; old RSPs are discarded.
Every OPEN invalidates the previous Wi-Fi session; duplicate IDs are refused
and cannot reopen an expired session. On
replacement the ESP32 drops queued TCP output and unsent UART commands,
delimits any partially written UART record, sends CLOSE before a new OPEN,
and clears partial input. UART bytes already transmitted cannot be recalled.
An invalidated session requires reconnecting and a new explicit ARM.

TCP clients see DATA, optional LOG, SESSION READY, and the text of matching
RSP records (the ESP32 strips the RSP/session envelope). For example:

```text
[MOTOR] ARM queued; wait for OK ARM
[MOTOR] OK ARM
```

The first is a local queue confirmation, not evidence the robot accepted
ARM. Only the second represents a matched actual e-puck acknowledgement.

## Motor ownership and timeouts

USB and Wi-Fi share the existing STM32 command validator and USART1
scheduler. Exactly one source owns an ARMING/ARMED session. Competing ARM/M
requests are rejected; HELP and MOTOR_STATUS remain available to both; STOP
from either source cancels queued ARM/M/HB and takes the next free UART slot.
Acknowledgements go to the command's source, retaining its original Wi-Fi ID.
USB disconnection only loses a USB-owned session, not a Wi-Fi-owned one.

Wi-Fi ARM requires CONTROL_HB received within the last 500 ms. The computer
sends it every 100 ms. ESP32 never manufactures it. The STM32 timestamps UART
reception, so delayed/buffered heartbeats cannot become fresh on parsing.
STM32 checks owner validity before motor scheduling even if esp_link_task
has stopped running. At 500 ms without CONTROL_HB it cancels movement,
disables e-puck motor heartbeats, requests STOP and loses ownership until
STOP acknowledgement. Expired sessions cannot be revived by buffered commands.

After actual OK ARM, the STM32 sends e-puck HB every 100 ms nominally. The
existing 150 ms motor-ACK deadline, 200 ms armed-session heartbeat/ACK guard,
250 ms e-puck watchdog and STOP priority remain. PING and R do not refresh
the robot watchdog. UART loss, missing ACKs or ERR NOT_ARMED require a fresh
explicit ARM after OK STOP. No startup/reconnect/error path automatically
arms or resumes movement. A main-loop pause may fail closed and lose the
session; the peer's independent watchdog remains active.

Motor speeds are **commanded steps/s**, integer -1000..1000, not measured
wheel speed. Steps counters are commanded steps, not measured wheel motion.

## Sensor telemetry

Normal acquisition defaults to1000ms; no periodic R polling is used. PING and
safety control retain their independent deadlines. The latest valid new sample
is offered at most once per1000ms; faster acquisition is coalesced. Sequence gaps
can therefore be intentional and do not measure network loss.

```text
DATA2 sequence age_ms epuck_time_ms prox0 ... prox7 acc_x acc_y acc_z steps_left steps_right
```

Exactly16 decimal numeric fields follow DATA2: uint32 sequence and age_ms,
uint16 peer time_ms, eleven int16 proximity/accelerometer readings, and two
int32 commanded-step counters. Sequence/tick arithmetic wraps modulo2^32 and
peer time modulo2^16. Age excludes later queue/network delays. Steps are not
measured motion. No light-sensor values are transmitted. Raw dsPIC DATA still
contains its original22 numeric fields, including lights; that parser is unchanged.

The updated ESP bridge/client also accept legacy DATA with24 numeric fields
(the same metadata/readings plus8 lights). New STM32 DATA2 needs the updated
ESP allowlist; an old bridge drops that prefix. The client omits light display.
Accelerometer readings require JE1 bridges33–34,35–36,37–38. Inspect age,
sequence progress and client arrival time; raw values can repeat.

## Bounded buffering and failures

STM32 USART3 uses a 1024-byte interrupt receive ring, 128-byte task budget,
16 transmit slots of 320 bytes, and a separate persistent active TX buffer.
Four slots are reserved from telemetry/log traffic for control responses.
Queued control overflow invalidates the Wi-Fi session. Telemetry/log overflow
is counted/reported as LOG dropped_records; sequence gaps remain observable.
Fault recovery drops uncertain input through a newline. No UART/USB transmit
or command parsing happens in receive interrupt callbacks.

All USART3 writes, including Qorvo/debug logs, use this queue. Debug lines are
prefixed/reframed as LOG; generated peripheral configuration is untouched.

ESP32 has one task owning sockets, parsers and two eight-record queues. This
ownership serializes queue access and close/replacement without shared-socket
races. All socket IO and UART FIFO writes are nonblocking and retain offsets
on partial writes. Receive work is bounded. A full output queue or 500 ms
stalled TCP write closes/invalidate the connection instead of blocking UART
reception. Input/control queue overflow or UART faults also invalidate it and
send CLOSE best effort. The STM32 timeout is authoritative if CLOSE is lost
or the ESP crashes. LED activity uses a deadline, not a sleep in UART RX.

## Configuration and testing

STM32 Core/Inc/app_config.h final defaults:

```c
#define APP_EPUCK_ENABLED 1
#define APP_UWB_ENABLED   0
#define APP_ESP_ENABLED   1
```

Disabled link init/task functions perform no link activity; ESP task also
returns safely if initialization is absent or the initial HAL receive fails.
USART1 and USART3 share single HAL callback definitions with port dispatch.
UWB source files, board ID/role, and guarded interrupt wrapper are retained.

In the sibling epuck-stm32 repository, run `cmake --preset Debug && cmake --build --preset Debug`.
STM32 mocks: `tests/epuck_console/run.sh` and `tests/esp_link/run.sh`.
ESP32 portable protocol/queue tests: `cc -std=c11 -Wall -Wextra -Werror -Imain main/bridge_protocol.c tests/test_protocol.c -o /tmp/bridge-test && /tmp/bridge-test`.
Run **all** portable transport/queue and client tests with `tests/run_host.sh`.
The actual transport code is also tested with host platform mocks for slow
clients, session filtering, Wi-Fi/UART faults and partial nonblocking writes.
ESP32 native builds are verified with the installed Linux ESP-IDF **v6.1 ESP32-S3** environment.
See the diagnostics guide for activation, Python launcher, flash and monitor commands.

Hardware verification (no hardware was flashed by this change):

1. Build and review both firmwares; flash them yourself using existing tools.
2. Confirm the existing Wi-Fi station obtains an IP; connect with
   `python3 tools/wifi_client.py <ESP32-IP> --logs` from epuck-esp32.
3. Wait for SESSION READY and client heartbeats. Verify DATA progresses with
   USB unplugged; HELP/MOTOR_STATUS should work without arming.
4. With wheels lifted, explicitly ARM, wait for OK ARM, then `M 200 200`,
   `M -200 200`, MOTOR_STATUS and STOP. Verify requested versus acknowledged
   speeds and OK STOP. Sensor streaming must continue.
5. Repeat on USB. While one source owns the session, verify the other's ARM/M
   are rejected and STOP is accepted from either. Unplug USB during a Wi-Fi
   session and verify it continues; unplug it during a USB session and verify
   STOP/disarm.
6. Kill the client or remove Wi-Fi/ESP power while moving. Verify STOP/session
   loss by the STM32 deadline. Reconnect and verify no motion until a new
   explicit ARM. Stop client heartbeats while keeping TCP open to test the
   authoritative timeout separately from disconnect notification.
7. Enable UWB for a separate test and check LOG records, ranging, sensor data,
   LED behavior and control watchdogs together. Never infer reversed wiring
   from missing replies alone.

The Python client keeps standard-library networking in a dedicated thread;
its optional prompt_toolkit UI cannot delay heartbeats with input or rendering.
It sends nothing before a valid SESSION READY. Ctrl-C/EOF cancels queued commands and sends STOP
best effort before closing. It never reconnects, arms or replays movement
implicitly. `--logs` shows debug records; default output suppresses LOG.

## Changed files and verification

STM32 files:

- Core/Inc/app_config.h, epuck_link.h, esp_link.h, stm32f4xx_it.h.
- Core/Src/epuck_link.c, esp_link.c, main.c, stm32f4xx_it.c, dwm3000_port.c.
- ThirdParty/qorvo_dw3/platform/qorvo_port.c; top-level CMakeLists.txt.
- tests/epuck_console/test.c; tests/esp_link/run.sh, test.c and stubs/main.h,
  stubs/usbd_cdc_if.h; docs/epuck_link.md and docs/wifi_protocol.md.

ESP32 files:

- main/main.c and main/CMakeLists.txt.
- main/bridge_protocol.c/.h and main/bridge_transport.c/.h.
- tools/wifi_client.py; tests/test_protocol.c, test_transport.c,
  bridge_test_platform.h, test_client.py and run_host.sh; README.md.

STM32 Debug builds pass for all eight EPUCK/ESP/UWB switch combinations.
USB motor regression, sensor boundaries/rejection/recovery, Wi-Fi ownership,
STOP from either source, independent heartbeat expiry, stale/duplicate session
rejection, UART/queue faults and telemetry/log framing host tests pass.
ESP32 portable framing/queue and actual transport host mocks pass, as do
Python socket-pair client tests. These are host tests, not hardware verification.

Both native builds pass: STM32 Debug and Linux ESP-IDF v6.1 ESP32-S3.
Hardware Wi-Fi/UART timing remains unverified. No firmware was flashed.

STM32 artifact: build/Debug/EPuck_STM32.elf.
ESP32 artifact: build/comm_with_stm32.bin (also build/comm_with_stm32.elf).
The client waits for a validated SESSION READY before any commands or heartbeats,
with a one-second handshake bound. Disconnect diagnostics do not extend the
500 ms network-heartbeat or existing motor watchdog deadlines.

Linux build/flash/monitor commands, example failure messages and reproduction steps: [docs/disconnect_diagnostics.md](docs/disconnect_diagnostics.md).
