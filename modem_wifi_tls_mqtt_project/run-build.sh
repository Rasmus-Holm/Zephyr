#!/usr/bin/env bash
# _Formål: Byg wifi-tls-no-sparkplugB for én variant (wifi eller modem); flasher og rører ikke hardware.
# Input: variant som første argument, wifi eller modem (eller en strømmålings-variant, se case); uden argument vælges den i en menu.
# Output: build-mappen for varianten (build-wifi for wifi, build-cellular for modem).
set -euo pipefail

PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
POWER="$PROJECT/power"
ZEPHYR_WS="$HOME/Work/zephyrproject"

VARIANT="${1:-}"
# Uden argument spørges der i en terminal; uden terminal stopper scriptet
if [[ -z "$VARIANT" && -t 0 ]]; then
  PS3="Vælg variant: "
  select VARIANT in wifi modem wifi-1 wifi-2a wifi-2b wifi-2c modem-1 modem-2a modem-2d; do
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
  # Strømmålinger (Testplan_stroemforbrug_PPK2): conf-filerne i power/ lægges oven i hinanden.
  # -1 = baseline med PPK2-markører; 2a-2d = test 2-trinnene; uden USB-konsol fra 2a.
  wifi-1|wifi-2a|wifi-2b|wifi-2c)
    BUILD_DIR="$PROJECT/build-$VARIANT"
    CONF_FILES="$PROJECT/wifi.conf;$PROJECT/wifi-local.conf;$POWER/ppk.conf"
    DTC_OVERLAYS="$PROJECT/modbus-uart0.overlay"
    case "$VARIANT" in
      wifi-1)  DTC_OVERLAYS="$DTC_OVERLAYS;$PROJECT/usb.overlay" ;;
      wifi-2a) CONF_FILES="$CONF_FILES;$POWER/2a-lowpower.conf" ;;
      wifi-2b) CONF_FILES="$CONF_FILES;$POWER/2a-lowpower.conf;$POWER/2b-wifi-ps.conf" ;;
      wifi-2c) CONF_FILES="$CONF_FILES;$POWER/2a-lowpower.conf;$POWER/2b-wifi-ps.conf;$POWER/2c-pm.conf" ;;
    esac
    ;;
  modem-1|modem-2a|modem-2d)
    BUILD_DIR="$PROJECT/build-$VARIANT"
    CONF_FILES="$PROJECT/cellular.conf;$PROJECT/cellular-local.conf;$POWER/ppk.conf"
    DTC_OVERLAYS="$PROJECT/modem-uart1.overlay;$PROJECT/modbus-uart0.overlay"
    case "$VARIANT" in
      modem-1)  DTC_OVERLAYS="$DTC_OVERLAYS;$PROJECT/usb.overlay" ;;
      modem-2a) CONF_FILES="$CONF_FILES;$POWER/2a-lowpower.conf;$POWER/2a-lowpower-modem.conf" ;;
      modem-2d) CONF_FILES="$CONF_FILES;$POWER/2a-lowpower.conf;$POWER/2a-lowpower-modem.conf;$POWER/2d-modem-psm.conf" ;;
    esac
    ;;
  *)
    echo "Brug: $0 [wifi|modem|wifi-1|wifi-2a|wifi-2b|wifi-2c|modem-1|modem-2a|modem-2d]" >&2
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
