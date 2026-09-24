#!/usr/bin/env bash
# Bygger appen med west.
# Brug: scripts/build.sh [-p]   (-p = clean rebuild)

set -e

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$APP_DIR/scripts/zephyr-env.sh" 2>/dev/null || exit 1

cd "$APP_DIR"

BOARD="esp32c6_devkitc/esp32c6/hpcore"
PRUNE=""
if [ "$1" = "-p" ]; then
    PRUNE="-p"
fi

west build $PRUNE -b "$BOARD" -d "$APP_DIR/build"

echo ""
echo "Byg færdig: $APP_DIR/build/zephyr/zephyr.elf"