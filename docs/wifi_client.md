# Wi-Fi client terminal

The UI uses [prompt_toolkit](https://python-prompt-toolkit.readthedocs.io/en/stable/pages/full_screen_apps.html).
The TCP protocol, one-second READY handshake limit, 100 ms CONTROL_HB schedule,
partial-write handling, and motor control are unchanged. Compact DATA2 needs
the updated ESP bridge; the UI itself runs on the host. There is no automatic reconnect, ARM, or replay of movement.

## Install and run

Use a Linux/POSIX terminal; 80 columns and 24 rows is comfortable but not
required. Resizing shrinks/hides the upper panes before sacrificing the fixed
bottom input row, and never switches to plain mode. Python 3.9 or later and
a single direct UI dependency are needed:

```bash
cd /home/derder/Documents/epuck-esp32
python3 -m venv .venv
.venv/bin/python -m pip install -r tools/requirements.txt
.venv/bin/python tools/wifi_client.py 192.168.1.35 --ui
```

The installed package may bring its own small terminal-width dependencies.
`.venv` is ignored. For optional debug messages, heartbeat acknowledgements,
or both:

```bash
.venv/bin/python tools/wifi_client.py 192.168.1.35 --logs
.venv/bin/python tools/wifi_client.py 192.168.1.35 --verbose
.venv/bin/python tools/wifi_client.py 192.168.1.35 --logs --verbose
```

The network thread never renders or writes terminal output. It only updates
a bounded thread-safe display model. The UI uses one full-screen Application
and a dedicated editable input Buffer. Background updates read shared state,
update the separate telemetry/event panes and invalidate the Application; they
never replace the input Document or change focus. The UI renders at most ten
times per second; sensor arrivals overwrite one latest-sample slot instead of
queuing redraws. Input editing, event browsing, and slow output do not block the socket
worker. As with any desktop process, system-wide scheduling stalls can still
interrupt heartbeats; no watchdog is extended to compensate.

## Panels and keys

- The status line distinguishes waiting for READY, ready session ID, and
  disconnection. Transport readiness does not mean motor control is armed.
- The sensor panel shows sequence, wrapping e-puck timestamp, proximity[8],
  accelerometer[x/y/z] (light values omitted), and commanded step counters L/R.
  All sensor readings are raw; step counters are commanded steps, not measured
  wheel motion. Accelerometer readings require e-puck JE1 bridges 33–34,
  35–36 and 37–38.
- **Reported age** is STM32's sample age when publishing DATA. It excludes
  transport and queue delay. **Since reception** is local monotonic elapsed
  time since the complete DATA record reached this client. The values are
  kept separate; neither is a synchronized end-to-end latency measurement.
- The event panel retains the latest 200 messages. PgUp/PgDn browse older/newer
  events while keeping command focus. Valid DATA records never enter this
  history. Session notices, actual acknowledgements, errors and disconnect
  reasons do. `--logs` includes LOG records. `[MOTOR] OK HB` is processed and
  counted regardless of `--verbose`; only its display is filtered.
- The bottom command field retains partially typed text when records arrive.
  Enter submits; arrows, Home/End, Backspace/Delete and selection work normally.
  Up/Down recall the last 100 commands without sending them; Down restores the
  partial draft when returning from history. History stays in memory for this
  launch only. Keywords are case-insensitive and normalized on the wire.
  `m -200 +200` becomes `M -200 200`. `stream 100`, `stream 50`, `stream 0`
  and `stream_status` configure/query automatic DATA without motor ownership;
  periods must be 0 or 50..10000 ms. See [streaming](sensor_streaming.md). Blank lines do nothing. Speeds are integer
  **commanded steps/s**, limited to -1000..1000. Malformed commands are rejected.
- A local `queued locally; not acknowledged` entry confirms only queue
  submission. Only an actual e-puck reply such as `[MOTOR] OK M` acknowledges
  an operation. Nothing automatically arms or starts movement.
- Ctrl-C or Ctrl-D exits; queued commands are discarded and the existing
  best-effort STOP shutdown runs if READY was received. The terminal is
  restored on exit/exception. Disconnects and handshake failure exit without
  waiting for Enter; recent events remain in terminal scrollback afterward.

Commands typed before READY are rejected, not saved for later. After loss,
relaunch explicitly and explicitly ARM again if movement is desired.
The command queue remains 32 entries; full queues reject new input. Input
editing is bounded to 128 characters, wire commands to 63 characters, received
records to 1024 bytes, and display messages to 1024 characters. An oversized
edit/paste is rejected as a whole on Enter, rather than sending its truncated
prefix. The sensor parser validates16 DATA2 fields or24 legacy DATA fields and type ranges before
replacing a sample; malformed DATA adds a bounded event and retains the last
valid sample, whose reception age continues increasing.

## Plain mode and automation

Plain mode requires only the standard library and emits line-oriented output
without terminal escape sequences:

```bash
python3 tools/wifi_client.py 192.168.1.35 --plain
python3 tools/wifi_client.py 192.168.1.35 --plain --logs --verbose
python3 tools/wifi_client.py 192.168.1.35 --logs > wifi-session.log
```

It is selected automatically only when stdin or stdout is redirected or TERM
is missing/dumb/unknown. Interactive terminals use the full-screen interface
even when small; a missing UI dependency produces an installation error before
connecting, rather than falling back to scrolling output. Use `--ui` to require
the persistent interface explicitly (terminal stdin/stdout and a supported
TERM setting required), or
`--plain` to deliberately select line-oriented output. There is no dedicated
input area in plain mode.
Plain mode coalesces valid DATA to the latest sample at most once per second
(plus a final sample at exit), retaining the original DATA wire text. Other
events follow the same filters as full-screen mode. Very slow display output
can lose older history entries; the network/heartbeat loop remains independent.
An idle/open input pipe does not prevent exit on network loss. EOF exits
without sending any unterminated partial input. Scripted commands must still
wait for READY, and must keep stdin open for the intended session duration.

## Tests

```bash
cd /home/derder/Documents/epuck-esp32
source .venv/bin/activate
tests/run_host.sh
```

This runs existing protocol/transport/client regressions plus model, plain-CLI,
and prompt_toolkit input-pipe tests: heartbeat traffic during idle/editing and
telemetry, case normalization, malformed/oversized input, latest-only DATA,
display-only filtering, readiness gating/timeouts, disconnect without input,
EOF and Ctrl-C/Ctrl-D STOP behavior. A POSIX pseudo-terminal test checks actual
full-screen launch on an 18x60 terminal, real SIGWINCH resizing during a
partial command, Ctrl-C shutdown, and restoration of terminal attributes.
The continuous-traffic regression verifies slow typing, exact submitted
commands, cursor/selection/focus retention, edit keys, bounded history
navigation, and bottom-row placement at sizes down to one row. An injected UI
exception also verifies shutdown without an out-of-band print. These are
automated checks; no manual interactive terminal or robot testing is claimed.
Without the optional dependency, only UI-specific tests skip; run in the venv
to include them. These use local sockets/mocks, not robot hardware.

The client accepts fast legacy streams; current STM32 firmware coalesces
50ms raw acquisition to at most1Hz Wi-Fi publication. Screen redraws are
independently bounded to10Hz. STREAM replies enter the event pane;
DATA itself only replaces the live sample. No periodic R polling is used by
the updated STM32. Normal DATA2 needs compatible STM32/ESP builds; run the Python client directly.
Transmission rate is not necessarily physical sensor sampling rate.
