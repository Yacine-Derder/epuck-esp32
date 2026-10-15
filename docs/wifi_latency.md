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
Client heartbeats remain 100 ms, STM32's network watchdog remains 500 ms, and
the e-puck motor watchdog remains unchanged. There is no automatic reconnect,
ARM or movement replay in the client.

## Console configuration (installed ESP-IDF v6.1)

Tracked `sdkconfig.defaults` selects ESP32-S3 and:

```text
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_CONSOLE_SECONDARY_NONE=y
```

UART-default/custom consoles, USB OTG CDC console, and secondary USB output
are deselected. The effective derived `CONFIG_ESP_CONSOLE_UART_NUM=-1` and
disabled `CONFIG_ESP_CONSOLE_UART` confirm that no application console uses
GPIO43/44. These pins remain **UART1 TX GPIO43 / RX GPIO44, 115200 8N1** for
the STM32 link. USB Serial/JTAG uses the chip's built-in USB connection;
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

## Operator hardware comparison (not performed during implementation)

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

Confirm the startup power-save message above (attach a monitor before a
manual reset if it was missed). USB should carry bootloader/application logs;
GPIO43/44 should carry only the STM32 protocol after initial ROM boot.
Keep the ESP monitor open and run in a second terminal:

```bash
cd /home/derder/Documents/epuck-esp32
ping -c 20 192.168.1.35
python3 tools/wifi_client.py 192.168.1.35 --logs
```

Compare ping min/avg/max and packet loss with the earlier 277–300 ms results,
using the same AP, position and network load. Wait for `SESSION <id> READY`
and sustained DATA without arming. Observe for longer than the previous
failure interval; absence of a short-term failure alone does not prove a fix.
The client sends CONTROL_HB every 100 ms only after READY. No motor commands
are required for this comparison.

Open the STM32 USB console in a third terminal as described in
[disconnect diagnostics](disconnect_diagnostics.md), and use `ESP_STATUS`
before/after the run. Compare heartbeat age and cumulative UART noise/error
counters (or deltas since startup), plus any ESP disconnect reason with the
same session ID. A continued failure should be investigated using those logs,
without extending watchdogs. No STM32 firmware changes are needed here.
