#!/usr/bin/env bash
# _Formål: Start serial monitor for én variant (wifi eller modem); bygger og flasher ikke.
# Input: variant som første argument, wifi eller modem; uden argument vælges den i en menu; build-mappen skal findes.
# Output: live konsol med adressedekodning; afslut med Ctrl+]. Åbning af porten nulstiller kortet.
set -euo pipefail

PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZEPHYR_WS="$HOME/Work/zephyrproject"
MONITOR="$ZEPHYR_WS/modules/hal/espressif/tools/idf_monitor/idf_monitor.py"

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

cd "$BUILD_DIR"
PYTHONPATH="$ZEPHYR_WS/zephyr/scripts/west_commands" \
  python "$MONITOR" -p "$PORT" -b 115200 "$BUILD_DIR/zephyr/zephyr.elf" --eol CRLF
