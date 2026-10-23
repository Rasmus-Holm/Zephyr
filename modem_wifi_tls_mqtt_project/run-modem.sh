#!/usr/bin/env bash
# _Formål: Byg, flash og start serial monitor for modem-varianten (Stamp CatM over PPP) af wifi-tls-no-sparkplugB.
set -euo pipefail

PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZEPHYR_WS="$HOME/Work/zephyrproject"
BUILD_DIR="$PROJECT/build-cellular"
MONITOR="$ZEPHYR_WS/modules/hal/espressif/tools/idf_monitor/idf_monitor.py"

# shellcheck source=/dev/null
source "$HOME/.venvs/zephyr-4.4.0/bin/activate"

PORT="$(ls /dev/serial/by-id/usb-Espressif* | head -1)"
cd "$ZEPHYR_WS"

west build -p always -b esp32c6_devkitc/esp32c6/hpcore -S espressif-flash-4M \
  -d "$BUILD_DIR" "$PROJECT" -- \
  -DOVERLAY_CONFIG="$PROJECT/cellular.conf" \
  -DEXTRA_DTC_OVERLAY_FILE="$PROJECT/modem-uart1.overlay;$PROJECT/usb.overlay"

west flash -d "$BUILD_DIR" --esp-baud-rate 460800 --esp-device "$PORT"

cd "$BUILD_DIR"
PYTHONPATH="$ZEPHYR_WS/zephyr/scripts/west_commands" \
  python "$MONITOR" -p "$PORT" -b 115200 "$BUILD_DIR/zephyr/zephyr.elf" --eol CRLF
