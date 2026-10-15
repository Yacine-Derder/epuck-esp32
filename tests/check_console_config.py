"""Check effective sdkconfig files, including a fresh defaults-only configuration."""
import argparse
from pathlib import Path


def check(path):
    values = {}
    for line in path.read_text().splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            values[key] = value
    expected = {
        "CONFIG_IDF_TARGET": '"esp32s3"',
        "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG": "y",
        "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED": "y",
        "CONFIG_ESP_CONSOLE_SECONDARY_NONE": "y",
        "CONFIG_ESP_CONSOLE_UART_NUM": "-1",
    }
    forbidden = (
        "CONFIG_ESP_CONSOLE_UART_DEFAULT", "CONFIG_ESP_CONSOLE_UART_CUSTOM",
        "CONFIG_ESP_CONSOLE_UART", "CONFIG_ESP_CONSOLE_USB_CDC",
        "CONFIG_ESP_CONSOLE_NONE", "CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG",
    )
    for key, value in expected.items():
        if values.get(key) != value:
            raise SystemExit(f"{path}: expected {key}={value}, got {values.get(key)}")
    for key in forbidden:
        if values.get(key, "n") != "n":
            raise SystemExit(f"{path}: unexpected console setting {key}={values[key]}")
    print(f"PASS: {path}: ESP32-S3 USB Serial/JTAG only; console UART disabled")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sdkconfig", nargs="+", type=Path)
    for config in parser.parse_args().sdkconfig:
        check(config)
