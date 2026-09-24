#!/usr/bin/env bash
# Sætter Zephyr-miljøet op. Kilde dette script: `source scripts/zephyr-env.sh`
# eller byg/flash via scripts/build.sh og scripts/flash.sh.

ZEPHYRPROJECT_DIR="$HOME/zephyrproject"
ZEPHYR_APP_DIR="${ZEPHYR_APP_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

# 1) Aktiver west venv
if [ -f "$ZEPHYRPROJECT_DIR/.venv/bin/activate" ]; then
    source "$ZEPHYRPROJECT_DIR/.venv/bin/activate"
else
    echo "FEJL: venv ikke fundet i $ZEPHYRPROJECT_DIR/.venv" >&2
    return 1
fi

# 2) Sæt Zephyr-miljøvariabler
export ZEPHYR_BASE="$ZEPHYRPROJECT_DIR/zephyr"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr

echo "Zephyr-miljø klar:"
echo "  ZEPHYR_BASE=$ZEPHYR_BASE"
echo "  ZEPHYR_TOOLCHAIN_VARIANT=$ZEPHYR_TOOLCHAIN_VARIANT"
echo "  App: $ZEPHYR_APP_DIR"