#!/usr/bin/env bash
# _Formål: Flash det byggede firmware for én variant (wifi eller modem) til ESP32-C6; bygger ikke.
# Input: variant som første argument, wifi eller modem; uden argument vælges den i en menu; build-mappen skal findes.
# Output: firmware på kortet via USB Serial/JTAG.
set -euo pipefail

PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZEPHYR_WS="$HOME/Work/zephyrproject"

VARIANT="${1:-}"
# Uden argument spørges der i en terminal; uden terminal stopper scriptet
if [[ -z "$VARIANT" && -t 0 ]]; then
  PS3="Vælg variant: "
  select VARIANT in wifi modem; do
    [[ -n "$VARIANT" ]] && break
  done
fi

# Vælg build-mappe efter variant
case "$VARIANT" in
  wifi)  BUILD_DIR="$PROJECT/build-wifi" ;;
  modem) BUILD_DIR="$PROJECT/build-cellular" ;;
  *)
    echo "Brug: $0 [wifi|modem]" >&2
    exit 1
    ;;
esac

if [[ ! -f "$BUILD_DIR/zephyr/zephyr.elf" ]]; then
  echo "Ingen build i $BUILD_DIR; kør run-build.sh $VARIANT først." >&2
  exit 1
fi

# shellcheck source=/dev/null
source "$HOME/.venvs/zephyr-4.4.0/bin/activate"

PORT="$(ls /dev/serial/by-id/usb-Espressif* | head -1)"
cd "$ZEPHYR_WS"

west flash -d "$BUILD_DIR" --esp-baud-rate 460800 --esp-device "$PORT"
