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

# Vælg build-mappe, Kconfig-filer og DT-overlays efter variant (prj.conf indlæses altid af Zephyr)
case "$VARIANT" in
  wifi)
    BUILD_DIR="$PROJECT/build-wifi"
    CONF_FILES="$PROJECT/wifi.conf;$PROJECT/wifi-local.conf"
    DTC_OVERLAYS="$PROJECT/modbus-uart0.overlay;$PROJECT/usb.overlay"
    ;;
  modem)
    BUILD_DIR="$PROJECT/build-cellular"
    CONF_FILES="$PROJECT/cellular.conf;$PROJECT/cellular-local.conf"
    DTC_OVERLAYS="$PROJECT/modem-uart1.overlay;$PROJECT/modbus-uart0.overlay;$PROJECT/usb.overlay"
    ;;
  *)
    echo "Brug: $0 [wifi|modem]" >&2
    exit 1
    ;;
esac

# Stop før build, hvis en conf-fil mangler, så kortet ikke får CHANGE_ME-standardværdier
IFS=';' read -ra CONF_LIST <<< "$CONF_FILES"
for conf in "${CONF_LIST[@]}"; do
  if [[ ! -f "$conf" ]]; then
    echo "Mangler $conf; opret den (se README, Lokal konfiguration)." >&2
    exit 1
  fi
done

# shellcheck source=/dev/null
source "$HOME/.venvs/zephyr-4.4.0/bin/activate"
cd "$ZEPHYR_WS"

west build -p always -b esp32c6_devkitc/esp32c6/hpcore -S espressif-flash-4M \
  -d "$BUILD_DIR" "$PROJECT" -- \
  "-DEXTRA_CONF_FILE=$CONF_FILES" "-DEXTRA_DTC_OVERLAY_FILE=$DTC_OVERLAYS"
