#!/usr/bin/env bash
# Flasher appen til ESP32-C6 via esptool (west flash --runner esp32).
# Enheden skal være tilsluttet via USB.

set -e

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$APP_DIR/scripts/zephyr-env.sh" 2>/dev/null || exit 1

cd "$APP_DIR"

west flash -d "$APP_DIR/build" --runner esp32