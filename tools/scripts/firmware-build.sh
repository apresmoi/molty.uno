#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# firmware-build.sh — build the firmware for one board (or all of them). Never flashes.
#
# Each board gets its own build dir AND its own sdkconfig, firmware/build/<name>/, so
# boards never share flash/PSRAM settings (main/CMakeLists.txt refuses a mismatch).
# The board's sdkconfig.board is layered over sdkconfig.defaults by
# firmware/CMakeLists.txt from -DMOLTY_BOARD. Needs ESP-IDF's environment
# (. $IDF_PATH/export.sh) and console/dist/index.html (make console).
#
#   scripts/firmware-build.sh <board>     a folder (waveshare/esp32-s3-touch-lcd-1.28)
#                                         or a board.json name
#   scripts/firmware-build.sh --all       every board under firmware/boards/
#
# Prints the build dir(s); the images are <build-dir>/molty.bin etc.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOARDS="$ROOT/scripts/firmware-boards.py"
command -v idf.py >/dev/null || { echo "firmware-build: idf.py not on PATH (. \$IDF_PATH/export.sh)" >&2; exit 1; }

build_one() {
  local dir name out
  dir="$(python3 "$BOARDS" dir "$1")"
  name="$(python3 "$BOARDS" name "$1")"
  out="$ROOT/firmware/build/$name"
  echo "firmware-build: $dir -> $out"
  idf.py -C "$ROOT/firmware" -B "$out" -DSDKCONFIG="$out/sdkconfig" -DMOLTY_BOARD="$dir" build
  python3 "$BOARDS" check-image "$out/molty.bin" "$dir"
}

[[ $# -eq 1 ]] || { sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
python3 "$BOARDS" check
if [[ "$1" == "--all" ]]; then
  for b in $(python3 "$BOARDS" list); do build_one "$b"; done
else
  build_one "$1"
fi
