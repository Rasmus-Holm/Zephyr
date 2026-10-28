#!/usr/bin/env bash
# _Formål: Byg wifi-tls-no-sparkplugB for én variant (wifi eller modem); flasher og rører ikke hardware.
# Input: variant som første argument, wifi eller modem; uden argument vælges den i en menu.
# Output: build-mappen for varianten (build-wifi for wifi, build-cellular for modem).
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

# Vælg build-mappe, Kconfig-overlay og DT-overlays efter variant
case "$VARIANT" in
  wifi)
    BUILD_DIR="$PROJECT/build-wifi"
    OVERLAY_CONFIG=""
    DTC_OVERLAYS="$PROJECT/modbus-uart0.overlay;$PROJECT/usb.overlay"
    ;;
  modem)
    BUILD_DIR="$PROJECT/build-cellular"
    OVERLAY_CONFIG="$PROJECT/cellular.conf"
    DTC_OVERLAYS="$PROJECT/modem-uart1.overlay;$PROJECT/modbus-uart0.overlay;$PROJECT/usb.overlay"
    ;;
  *)
    echo "Brug: $0 [wifi|modem]" >&2
    exit 1
    ;;
esac

# shellcheck source=/dev/null
source "$HOME/.venvs/zephyr-4.4.0/bin/activate"
cd "$ZEPHYR_WS"

CMAKE_ARGS=("-DEXTRA_DTC_OVERLAY_FILE=$DTC_OVERLAYS")
if [[ -n "$OVERLAY_CONFIG" ]]; then
  CMAKE_ARGS+=("-DOVERLAY_CONFIG=$OVERLAY_CONFIG")
fi

west build -p always -b esp32c6_devkitc/esp32c6/hpcore -S espressif-flash-4M \
  -d "$BUILD_DIR" "$PROJECT" -- "${CMAKE_ARGS[@]}"
