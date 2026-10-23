#!/usr/bin/env bash
set -e
# Læs konsoluddata fra ESP32-C6 DevKitC over USB Serial/JTAG.
# Porten kan variere; default /dev/ttyACM0.
PORT="${PORT:-/dev/ttyACM0}"
TIMEOUT="${TIMEOUT:-15}"
# Undgå DTR/RTS-toggling (kan sætte ESP32-C6 i ROM-downloadmode).
exec timeout "${TIMEOUT}" stdbuf -o0 cat "${PORT}"
