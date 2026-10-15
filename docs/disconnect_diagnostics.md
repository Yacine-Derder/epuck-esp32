# Diagnose Wi-Fi session disconnects

This change adds diagnostics, not longer watchdogs. The network CONTROL_HB
limit remains **500 ms**, the ESP handshake limit remains **1000 ms**, and the
e-puck motor watchdog remains **250 ms**. The client now also waits at most
one second for a valid `SESSION <16 lowercase hex digits> READY`, sends its
first CONTROL_HB immediately afterward, then repeats every 100 ms. Commands
typed before readiness are rejected locally and must be entered again.
There is no automatic reconnect, ARM or replay of movement.

## Linux builds and tests

Use the installed ESP-IDF Installation Manager activation script, not a
Windows SDK path. In every ESP-IDF/monitor terminal:

```bash
source /home/derder/.espressif/tools/activate_idf_v6.1.sh
export IDF_TARGET=esp32s3
```

This sets IDF_PATH, IDF_PYTHON_ENV_PATH and tool paths for the installed v6.1
SDK. Invoke idf.py through `python3` to match this project's existing CMake
cache (`venv/bin/python3`). The `idf.py` shim launches `venv/bin/python` and
can cause an interpreter/cache mismatch; no fullclean is necessary.

```bash
cd /home/derder/Documents/epuck-stm32
cmake --preset Debug
cmake --build --preset Debug
tests/epuck_console/run.sh
tests/esp_link/run.sh

cd /home/derder/Documents/epuck-esp32
source /home/derder/.espressif/tools/activate_idf_v6.1.sh
python3 "$IDF_PATH/tools/idf.py" build
tests/run_host.sh
```

Artifacts:

- STM32: `/home/derder/Documents/epuck-stm32/build/Debug/EPuck_STM32.elf`.
- ESP32: `/home/derder/Documents/epuck-esp32/build/comm_with_stm32.bin` and
  `build/comm_with_stm32.elf`; bootloader/partition files are in `build/`.

All eight STM32 EPUCK/ESP/UWB switch combinations build. Final defaults remain
APP_EPUCK_ENABLED=1, APP_ESP_ENABLED=1, APP_UWB_ENABLED=0. Both native builds
and host regression/diagnostic tests pass. ESP-IDF emits upstream Kconfig
notes; they do not prevent compilation or linking. No hardware was flashed
or hardware timing verified as part of this implementation.

## Flash commands (operator action, not run during implementation)

First close any monitor using the target port. Identify ports; numbers may
change on reconnect. Do not use the ESP port for the STM32 USB console:

```bash
ls -l /dev/serial/by-id/ /dev/ttyACM*
export ESP32_PORT=/dev/ttyACM1   # replace if enumeration differs
export STM32_PORT=/dev/ttyACM0   # replace with the STM32 CDC port after connecting its USB cable
```

ESP32 (existing ESP32-S3 target and Wi-Fi credentials are retained):

```bash
cd /home/derder/Documents/epuck-esp32
source /home/derder/.espressif/tools/activate_idf_v6.1.sh
python3 "$IDF_PATH/tools/idf.py" -p "$ESP32_PORT" flash
```

STM32 with an ST-Link SWD probe, using the installed OpenOCD and its scripts:

```bash
cd /home/derder/Documents/epuck-stm32
source /home/derder/.espressif/tools/activate_idf_v6.1.sh
openocd -s "$OPENOCD_SCRIPTS" -f interface/stlink.cfg -f target/stm32f4x.cfg \
  -c 'program build/Debug/EPuck_STM32.elf verify reset exit'
```

These commands intentionally reset/flash the targets when the operator runs
them. They are separate from the following monitor-only reproduction steps.

## Reproduce with three terminals

1. Connect both USB cables. In terminal A, open the **ESP32** log monitor.
   Re-export ESP32_PORT if this is a new shell. `--no-reset` avoids creating a
   new disconnect by resetting the ESP while attaching the monitor.

   ```bash
   cd /home/derder/Documents/epuck-esp32
   source /home/derder/.espressif/tools/activate_idf_v6.1.sh
   export ESP32_PORT=/dev/ttyACM1
   python3 "$IDF_PATH/tools/idf.py" -p "$ESP32_PORT" monitor --no-reset --timestamps
   ```

   Exit IDF Monitor with Ctrl-]. It owns only the ESP32 USB log port, not the
   TCP connection or STM32 UART. No credentials are included in new logs.

   The current local SDK configuration has INFO logging and secondary USB
   Serial/JTAG console output enabled, so the built-in USB port carries these
   ESP_LOG messages. Retain those console settings on a fresh configuration.

2. In terminal B, open the **STM32** USB CDC console at 115200. Use the
   installed SDK Python, which includes pyserial. `--eol LF` makes Enter send
   the expected command terminator; `-e` enables local command echo.

   ```bash
   source /home/derder/.espressif/tools/activate_idf_v6.1.sh
   export STM32_PORT=/dev/ttyACM0
   python3 -m serial.tools.miniterm "$STM32_PORT" 115200 --eol LF -e
   ```

   Type `ESP_STATUS` and press Enter. This prints current session state,
   last invalidation reason, heartbeat age at loss, and fault counters even
   when the ESP UART is broken. Ctrl-] exits miniterm. Opening/closing this
   console does not terminate a Wi-Fi-owned motor session.

3. In terminal C, run the client against the address printed by the existing
   Wi-Fi station startup log (replace the example address):

   ```bash
   cd /home/derder/Documents/epuck-esp32
   python3 tools/wifi_client.py 192.168.1.100 --logs
   ```

   Wait for SESSION READY; DATA should continue. Do not ARM to reproduce the
   reported idle telemetry disconnect. Run `HELP` or `MOTOR_STATUS` only if
   desired. When a disconnect occurs, capture the ESP32 `disconnect` line
   and STM32 `[ESP] lost`/`[ESP] faults` lines with the same session ID.
   Type `ESP_STATUS` on STM32 afterward to recover the last reason/counters.
   Data records alone do not prove that CONTROL_HB is reaching STM32.

4. For a deterministic heartbeat-timeout diagnostic without motor commands,
   use this standard-library probe instead of the interactive client. It
   waits for READY, sends exactly one CONTROL_HB, then keeps TCP open:

   ```bash
   python3 - 192.168.1.100 <<'PY'
   import socket, sys
   with socket.create_connection((sys.argv[1], 3333), timeout=5) as sock:
       sock.settimeout(3)
       stream = sock.makefile('rb')
       for raw in stream:
           print(raw.decode('ascii', errors='replace').rstrip(), flush=True)
           fields = raw.strip().split()
           if len(fields) == 3 and fields[0] == b'SESSION' and fields[2] == b'READY':
               sock.sendall(b'CONTROL_HB\n')
               break
       for raw in stream:
           print(raw.decode('ascii', errors='replace').rstrip(), flush=True)
   PY
   ```

   Expected: STM32 `reason=control_hb_timeout hb_ms=500` (or slightly later
   according to task scheduling), then ESP32 `reason=stm32_session_lost`
   with cached STM32 detail. This probe never ARMs. Starting it while a
   controlling client is connected will be rejected; close the first client.

## Read the diagnostics

Examples (IDs/counters/times vary):

```text
I (...) bridge: accepted session=0123456789abcdef socket=54 uart_q=0 tcp_q=0
I (...) bridge: handshake_ready session=0123456789abcdef elapsed_ms=12 uart_q=0 tcp_q=0
[ESP] lost sid=0123456789abcdef reason=control_hb_timeout hb_ms=501 seen=1
[ESP] faults uart=0 bits=0x0 rx_ovf=0 tx_full=0 q=0 fault_q=0 mask=0x0 invalidations=1
W (...) bridge: disconnect reason=stm32_session_lost session=0123456789abcdef detail=0 uart_q=0 tcp_q=1 age_ms=620 stall_ms=20 stm_diag=reason=control_hb_timeout hb_ms=501 ...
DISCONNECT 0123456789abcdef reason=stm32_session_lost detail=0 stm=reason=control_hb_timeout hb_ms=501 ...
```

The final line is a best-effort TCP notice and is displayed without --logs.
If TCP output is stalled, reset or in a partial record, it may not arrive.
There is one nonblocking attempt and no teardown delay/retry. ESP_LOG and
STM32 USB remain the authoritative diagnostics. DIAG is a bounded tagged UART
record emitted before SESSION LOST when queue space permits; ESP caches it
so that receiving LOST in the same UART packet does not erase the reason.

ESP32 reasons:

| reason | Meaning / relevant detail |
| --- | --- |
| wifi_lost | Wi-Fi connection bit cleared; detail is the SDK Wi-Fi disconnect reason code. |
| stm32_session_lost | Matching SESSION LOST received; stm_diag contains the preceding tagged DIAG if available. |
| uart_fifo_overflow / uart_buffer_overflow | UART FIFO or driver RX buffer overflow; detail is the UART event type. |
| uart_framing_error / uart_parity_error | UART frame/parity event, distinct from buffer overflow. |
| uart_malformed_record / uart_oversized_record | Bad ASCII/CR framing or UART record beyond the existing bound; detail is captured line length. |
| tcp_malformed_record / tcp_oversized_record | Malformed or oversized TCP command rejected; no command is forwarded. |
| uart_tx_queue_full | Bounded UART output queue exhausted while submitting a control/session record. |
| tcp_tx_queue_full | Bounded client output queue exhausted; often a slow/non-reading client. |
| tcp_receive_eof | Peer closed its sending side / connection. |
| tcp_receive_error / tcp_send_error | Socket syscall failed; detail is captured errno, before logging can change it. |
| tcp_send_zero | Unexpected zero-byte socket send. |
| tcp_output_stalled | Output could not progress for the existing 500 ms bound. |
| session_handshake_timeout | Matching READY absent for the existing 1000 ms bound. |
| uart_read_error / uart_write_error | UART API returned an error; detail is its return code. |

ESP logs also include queue occupancy **before clearing**, connection age and
send-progress age. Acceptance and successful handshake are logged once.
Idle disconnect checks do not produce repeated logs. Listener setup errors
are reason-coded and repeated identical failures are rate-limited. A second
TCP controller is rejected without replacing or closing the first session.

STM32 reasons:

| reason | Meaning |
| --- | --- |
| control_hb_timeout | No recent accepted CONTROL_HB for 500 ms; seen=1 and hb_ms show the age. |
| stale_input | A buffered UART input record was already at least 500 ms old. |
| framing_invalid / line_malformed | Bad transport opcode/framing, control characters, CR handling or oversized line. |
| session_invalid / cmd_malformed / close_malformed | Invalid ID or malformed CMD/CLOSE envelope. |
| explicit_close | ESP explicitly closed the matching session. |
| session_replaced | New/duplicate OPEN invalidated an active prior session. |
| uart_fault | HAL UART error captured in ISR; bits holds the last HAL error mask. |
| rx_overflow | Software receive ring filled; rx_ovf counts dropped callbacks/bytes. |
| control_tx_full | A control reply could not fit the transmit queue. |

Counters are cumulative until MCU reset. `uart` counts HAL error callbacks;
`bits` is the last HAL UART error mask (for example 0x8 is overrun). `rx_ovf`
counts ring-overflow drops. `tx_full` counts exhausted control-output enqueue
attempts; best-effort diagnostic/LOST drops do **not** increment it or raise
another fault. `fault_q` and `mask` describe the last captured UART/queue
fault, not necessarily the current reason: bits 1/2/4 denote UART error,
RX-ring overflow and control-TX exhaustion. When simultaneous faults occur,
reason reports UART first, then RX overflow, then TX exhaustion; counters and
mask retain all captured causes. `invalidations` increments once per active
session loss. Repeated invalid input after teardown does not spam diagnostics.

USB messages are bounded and may be dropped if its console queue is full or
USB is disconnected. ESP_STATUS retains the last reason and fault counters
for inspection afterward. Fault callbacks only capture state/counters; all
invalidation formatting and USB/ESP queue submissions run in the main loop.
No diagnostics rely on the failing UART or recursively fill its queue.

These messages identify the failing path. They do not establish the physical
cause without captured logs. No hardware verification is claimed. Raw sensor
readings, commanded steps/s units, STOP priority, ownership, watchdogs, feature
switches, UWB and LED behavior are preserved.
