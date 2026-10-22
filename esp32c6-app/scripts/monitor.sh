#!/usr/bin/env bash
# Åbner serial monitor for USB Serial/JTAG-konsollen via `west espressif monitor`.
# Brug: scripts/monitor.sh [port]   (standard: /dev/ttyACM0)

set -e

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$APP_DIR/scripts/zephyr-env.sh" 2>/dev/null || exit 1

PORT="${1:-/dev/ttyACM0}"
ELF="$APP_DIR/build/zephyr/zephyr.elf"

if [ ! -e "$PORT" ]; then
    echo "FEJL: $PORT findes ikke. Slut ESP32-C6 DevKitC'en til USB og prøv igen." >&2
    exit 1
fi

# `west espressif` er en workspace-extension: det kører kun inde i ~/zephyrproject.
cd "$ZEPHYRPROJECT_DIR"

if [ -f "$ELF" ]; then
    exec west espressif monitor -p "$PORT" -e "$ELF"
else
    echo "Bemærk: $ELF findes endnu - byg først (scripts/build.sh)." >&2
    exec west espressif monitor -p "$PORT"
fi
