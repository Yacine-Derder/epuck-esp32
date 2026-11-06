# Simple Wi-Fi motor control and combined acquisition

Use the STM32 **RobotExperiment** image for UWB + motors + DATA2. The original
Universal image remains read-only. The ESP32 keeps one TCP owner on port 3333,
the existing session framing, UART validation and bounded queues. It has no
motor arming/heartbeat logic of its own. New competitors get a best-effort
`DISCONNECT - reason=controller_busy ...` notice, then EOF. Close the existing
collector, ROS bridge or wifi_client before connecting another tool. A failed
best-effort notice can still appear as immediate EOF; Python explains this case.

The clients default to no CONTROL_HB. For an original Universal image, use
`wifi_client.py --legacy-heartbeat`, `uwb_collect.py --legacy-heartbeat`, or ROS
`legacy_heartbeat:=true`. Internal dsPIC HB is a different operation, handled
entirely by STM32. The existing ESP binary can carry M/STOP if it already supports
DATA2/UWB3; rebuild/flash this version for explicit busy diagnostics. Older
DATA-only bridges require flashing. No hardware is flashed automatically.

From this repository, with the ROS bridge and other collectors stopped:

```bash
# Interactive: type M 100 100, M -100 100, MOTOR_STATUS, STOP.
python3 tools/wifi_client.py 192.168.1.141 --plain

# Drive for five seconds, record everything and request STOP on exit.
python3 tools/robot_experiment.py 192.168.1.141 --seconds 5 \
  --command 'M 100 100' --output moving-raw.jsonl

# Confirm acquisition period and genuinely new DATA2 at approximately 10 Hz.
python3 tools/robot_experiment.py 192.168.1.141 --seconds 30 \
  --command 'STREAM 100' --command STREAM_STATUS --command MOTOR_STATUS \
  --output stationary-raw.jsonl
python3 tools/robot_bandwidth.py stationary-raw.jsonl moving-raw.jsonl

# Existing compact collector now retains DATA2 and motor replies, with no motion.
python3 tools/uwb_collect.py --robot 192.168.1.141:3333 --no-live
```

The experiment CLI never reconnects or replays movement. It waits for READY before
sending, logs actual sent commands and all received lines with capture/session,
PC Unix and monotonic reception times, and requests STOP on exit after a movement
request. Inspect `OK STOP`; a queued/sent STOP alone does not confirm physical zero.
The simple interactive display can coalesce DATA2 to 1 Hz; use the rate checker or
raw recording to measure the stream itself.

`robot_bandwidth.py` accepts the experiment CLI and ROS worker raw JSONL schema
(`kind=received`, `raw`, `received_unix`). It reports actual application bytes,
DATA2 rate, gaps, stale counts, TDoA/component availability, AR count and recent
transport diagnostics. It does not measure RF airtime, TCP headers or Wi-Fi retries.
Compare equal-duration stationary baseline and combined runs under the same plan.
See the [ROS experiment procedure](https://github.com/Yacine-Derder/mul_rlr_localization/blob/main/docs/ROBOT_EXPERIMENT.md)
and [STM32 protocol/failure/build/rollback details](https://github.com/Yacine-Derder/epuck-stm32/blob/main/docs/robot_experiment.md).

DATA2 preserves source time modulo 65536, STM32 uint32 sequence/age and raw signed
phase counts. Producer counter width is unknown; no encoder interpretation,
counter unwrapping, UWB clock mapping, calibration or interpolation is added.
Compact logs mark stale receipt ages (>300 ms), sequence discontinuity/gaps and
connection provenance. At slower configured rates interpret interarrival gaps
against that rate; the age threshold concerns the sample's STM32 formatting age.

Software checks: `tests/run_host.sh` passes the protocol/transport C mocks and
125 Python tests. Native ESP32-S3 build succeeds; native build:

```bash
source /home/derder/.espressif/tools/activate_idf_v6.1.sh
python3 "$IDF_PATH/tools/idf.py" build
# Operator only, replace port:
python3 "$IDF_PATH/tools/idf.py" -p /dev/ttyACM1 flash
```

Physical UART/RF/Wi-Fi timing and motor behavior remain operator validation.
