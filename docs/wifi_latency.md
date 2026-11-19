# Wi-Fi latency and UART console isolation

The reported `control_hb_timeout hb_ms=500 seen=1` means STM32 accepted a
heartbeat, then received no sufficiently recent one before its deadline.
`uart=1 bits=0x2` records a HAL UART noise error, not proof of its physical
cause. Earlier 277–300 ms ping times, modem sleep, and the UART0 console
sharing GPIO43/44 with the STM32 link are suspected contributors. This change
does not establish the root cause or claim hardware verification.

After successful `esp_wifi_init()` and before `esp_wifi_start()`, the application
calls `ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE))`. It reads back and logs
the setting once at startup:

```text
I (...) UART_TCP_BRIDGE: Wi-Fi power save=0 (WIFI_PS_NONE=0)
```

Disabling modem sleep keeps the Wi-Fi radio awake and **increases power
consumption** compared with power-save operation. It avoids waiting for
modem-sleep wakeups; it cannot guarantee network latency or session stability.
In legacy heartbeat mode the client interval is 100 ms and STM32's network
watchdog is 500 ms. The interactive client requires
a new connection after a drop and leaves ARM and movement commands to the
operator.

## Access-point selection

On startup and each reconnect, the ESP-IDF v6.1 driver scans the ESP32-S3's
permitted 2.4 GHz channels (`WIFI_ALL_CHANNEL_SCAN`) and tries eligible
`disal-robots` APs in descending scan RSSI order
(`WIFI_CONNECT_AP_BY_SIGNAL`). An AP must still meet the existing WPA2-PSK
authentication threshold. If association with the strongest candidate fails,
the driver can try another candidate. The station leaves the BSSID unset and
the channel hint at zero, so the SSID and authentication filter determine
eligibility across all permitted channels. See
[ESP-IDF station configuration](https://docs.espressif.com/projects/esp-idf/en/release-v6.1/esp32s3/api-guides/wifi-driver/overview.html#station-basic-configuration).

The ESP32 automatically retries after a driver disconnect event. The transport
task tears down the old TCP session, discards unsent movement and queues its
STM32 `CLOSE` before any new `OPEN`. It remembers the loss even if IP returns
between task iterations. After IP is available, a client can open a fresh TCP
session and wait for a new `SESSION <id> READY`. Association and IP acquisition
do not restore motor ownership or replay commands.

Read the timestamped USB Serial/JTAG log for association and DHCP:

```text
I (...) UART_TCP_BRIDGE: Wi-Fi connected bssid=02:11:22:33:44:55 channel=6 rssi=-53 dBm
I (...) UART_TCP_BRIDGE: Got IP: 192.168.1.141
W (...) UART_TCP_BRIDGE: Wi-Fi disconnected reason=200, retrying...
I (...) UART_TCP_BRIDGE: Wi-Fi reconnected bssid=02:11:22:33:44:66 channel=11 rssi=-61 dBm
I (...) UART_TCP_BRIDGE: Got IP: 192.168.1.141
```

`connected` is the first association since boot and `reconnected` marks
later associations. BSSID identifies the AP radio and channel is taken from
the association event. RSSI is a snapshot from the driver after association.
Values closer to zero indicate a stronger received signal. `rssi=unavailable`
can appear if the link is already gone or the current AP record has changed
by the time the event is handled. The IP line appears when the station obtains
IPv4. Each disconnect keeps its numeric driver reason. See
[disconnect diagnostics](disconnect_diagnostics.md) for the matching TCP and
STM32 session logs.

### Roaming limits and a possible second stage

This policy runs when the station connects. A healthy link can stay on its
current AP even when a stronger AP is nearby. The application initiates
no periodic scans or forced disconnects. Full-channel scans can add time to
initial association and reconnect.

A future conservative roaming policy would need to measure current RSSI over
time and compare candidates during bounded scans only when experiments and
motor ownership are inactive. Require sustained low RSSI, a candidate stronger
by a calibrated margin (hysteresis), and a cooldown between attempts. Validate
the idle-state gate, off-channel scan impact, TCP invalidation, DHCP, and
handoff latency on the deployed APs before enabling it. Any reassociation
would still require a fresh control session.

## Console configuration (installed ESP-IDF v6.1)

Tracked `sdkconfig.defaults` selects ESP32-S3 and:

```text
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_CONSOLE_SECONDARY_NONE=y
```

UART-default/custom consoles, USB OTG CDC console, and secondary USB output
are deselected. The effective derived `CONFIG_ESP_CONSOLE_UART_NUM=-1` and
disabled `CONFIG_ESP_CONSOLE_UART` confirm that no application console uses
GPIO43/44. These pins carry **UART1 TX GPIO43 / RX GPIO44, 8N1** for the STM32
link. The current default is 921600 baud and must match the STM32 image.
USB Serial/JTAG uses the chip's built-in USB connection;
use the board's native USB port, not a UART0 USB adapter.

In this SDK, `components/bootloader_support/src/bootloader_console.c` uses
the same `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` selection for the second-stage
bootloader: it releases the default UART console IO, switches the console to
USB Serial/JTAG, and disables duplicate ROM channel-2 output. There is no
separate bootloader console-route switch to set. Bootloader INFO logging is
retained on USB. Rebuild and flash **both bootloader and application** with
`idf.py flash`; flashing only the application would retain an older bootloader.

Initial mask-ROM boot output occurs before this configurable bootloader and
may still appear on UART0/GPIO43 at reset, depending on the chip's existing
straps/eFuses. Project defaults cannot suppress those initial ROM bytes.
No straps or eFuses are changed here. An attached STM32 may therefore still
observe initial boot bytes and require a fresh control session after reset.

`sdkconfig` is ignored and its existing selections take precedence over
defaults. The local effective configuration was updated selectively and
regenerated, preserving other project settings. On another existing checkout,
run `python3 "$IDF_PATH/tools/idf.py" menuconfig`, select **Component config →
ESP-STDIO → Channel for console output → USB Serial/JTAG Controller** and
**Channel for console secondary output → No secondary console**, then save
and rebuild. Do not delete an existing configuration just to apply defaults.

## Build and verify configuration

```bash
cd /home/derder/Documents/epuck-esp32
source /home/derder/.espressif/tools/activate_idf_v6.1.sh
export IDF_TARGET=esp32s3
python3 "$IDF_PATH/tools/idf.py" reconfigure
python3 tests/check_console_config.py sdkconfig
tests/run_host.sh
python3 "$IDF_PATH/tools/idf.py" build
```

Use SDK `python3` rather than the `idf.py` launcher so the interpreter matches
the existing CMake cache. The firmware is `build/comm_with_stm32.bin`.

To verify defaults on a fresh configuration without touching local settings,
use a new temporary directory (no project sdkconfig is copied):

```bash
fresh_dir=$(mktemp -d /tmp/epuck-console-check.XXXXXX)
python3 "$IDF_PATH/tools/idf.py" -B "$fresh_dir/build" \
  -D SDKCONFIG="$fresh_dir/sdkconfig" \
  -D SDKCONFIG_DEFAULTS="$PWD/sdkconfig.defaults" reconfigure
python3 tests/check_console_config.py sdkconfig "$fresh_dir/sdkconfig"
```

Check that both `build/config/sdkconfig.h` and
`build/bootloader/config/sdkconfig.h` define
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG 1`,
`CONFIG_ESP_CONSOLE_SECONDARY_NONE 1` and
`CONFIG_ESP_CONSOLE_UART_NUM -1`, without defining
`CONFIG_ESP_CONSOLE_UART`. Defaults contain no Wi-Fi credentials; existing
credentials/configuration are retained, and ignored files are not committed.

### Build and use the USB port from Docker

If the native activation script is unavailable, use the official v6.1
container. This example builds from tracked defaults in a separate directory
and uses the current UART baud default. Run the flash and monitor calls
when ready to use the robot's native USB port.

```bash
cd /home/derder/Documents/epuck-esp32
export ESP32_PORT=/dev/ttyACM1  # replace with the ESP32 native USB port
build_dir=$(mktemp -d /tmp/epuck-wifi-build.XXXXXX)
esp32_idf() {
  local -a usb=()
  if [ "$1" = flash ] || [ "$1" = monitor ]; then
    usb=(--device "$ESP32_PORT:/dev/ttyACM0"
         --group-add "$(stat -Lc %g "$ESP32_PORT")")
    set -- -p /dev/ttyACM0 "$@"
  fi
  docker run --rm -it --user "$(id -u):$(id -g)" \
    -e HOME=/tmp -e IDF_TARGET=esp32s3 -e IDF_CCACHE_ENABLE=0 \
    "${usb[@]}" -v "$PWD:/project:ro" -v "$build_dir:/out" \
    -w /project espressif/idf:v6.1 \
    python3 /opt/esp/idf/tools/idf.py -B /out/build -D SDKCONFIG=/out/sdkconfig "$@"
}
esp32_idf build
esp32_idf flash
esp32_idf monitor --no-reset --timestamps
```

## RLR 141 hardware comparison

Identify the native USB Serial/JTAG device with `ls -l /dev/serial/by-id/`.
Replace the example port below with that device. Close any monitor before
flashing. These are operator commands; no hardware was flashed by this change.

```bash
cd /home/derder/Documents/epuck-esp32
source /home/derder/.espressif/tools/activate_idf_v6.1.sh
export ESP32_PORT=/dev/ttyACM1
python3 "$IDF_PATH/tools/idf.py" -p "$ESP32_PORT" flash
python3 "$IDF_PATH/tools/idf.py" -p "$ESP32_PORT" monitor --no-reset --timestamps
```

1. Before flashing, record a baseline on the old image. Keep the robot
   stationary and disarmed at a marked position within range of at least two
   eligible APs. Record the selected BSSID and channel from the AP controller's
   station table if the old image omits them. Save its RSSI if available and
   note that AP-reported RSSI is measured at the other end of the link.
   Record time from boot to IPv4 over several boots.
2. Run the ping and client checks below against the IP reported by the robot.
   Save ping min/avg/max and packet loss, session ID and disconnect count
   during a fixed observation window such as 10 minutes. Confirm sustained
   DATA without arming or sending movement. Use `--legacy-heartbeat` only
   when running the original Universal firmware.
3. Build and flash the new image using the commands above. Attach the USB
   monitor before a manual reset if boot lines were missed. Record BSSID,
   channel and RSSI over several boots, plus time to IPv4. Repeat the ping
   and client checks using the same position, host, network load and duration.
   Compare the selected AP, latency, loss and session stability.
4. In an isolated test with the robot still disarmed, remove coverage of the
   current AP while another eligible AP remains reachable. Capture the
   disconnect reason, time to `Got IP`, and reconnected BSSID/channel/RSSI.
   Check that the old client loses its session. Start a new client using the
   IP in the log, confirm a new `SESSION <id> READY`, and enter
   `MOTOR_STATUS` to confirm the robot is disarmed. Run ping again to
   confirm reachability.

```bash
cd /home/derder/Documents/epuck-esp32
export ESP32_IP=192.168.1.141  # replace with the address in Got IP
ping -c 100 "$ESP32_IP"
python3 tools/wifi_client.py "$ESP32_IP" --plain --logs
```

Open the STM32 USB console in a third terminal as described in
[disconnect diagnostics](disconnect_diagnostics.md), and use `ESP_STATUS`
before and after the run to compare fault counters and correlate the
invalidation with the ESP log's session ID. For legacy heartbeat runs, also
compare heartbeat age. Run longer than the previous failure interval when
evaluating an intermittent disconnect. Hardware AP selection and latency
measurements are pending.

### Roll back the ESP32 image

Reflash the known-good image or rebuild the pre-change revision in a separate
worktree. Use the same ESP32-S3 build settings, especially the UART rate
matching the installed STM32 image. The default UART rate for these revisions
is 921600 baud.

```bash
cd /home/derder/Documents/epuck-esp32
source /home/derder/.espressif/tools/activate_idf_v6.1.sh
export IDF_TARGET=esp32s3
export ESP32_PORT=/dev/ttyACM1  # replace with the ESP32 native USB port
rollback_dir=$(mktemp -d /tmp/epuck-wifi-rollback.XXXXXX)
git worktree add --detach "$rollback_dir" 9af3a03d72be9e92d48fb478df5d6625559c4b5d
cd "$rollback_dir"
python3 "$IDF_PATH/tools/idf.py" build
python3 "$IDF_PATH/tools/idf.py" -p "$ESP32_PORT" flash
python3 "$IDF_PATH/tools/idf.py" -p "$ESP32_PORT" monitor --no-reset --timestamps
```
