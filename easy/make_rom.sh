#!/bin/sh
# Double-clickable / terminal helper for macOS and Linux.
cd "$(dirname "$0")" || exit 1
exec python3 make_rom.py "$@"
