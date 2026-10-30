#!/usr/bin/env bash
# gesture_build.sh — host build + run of the shake detector test
# (main/sense/gesture.c — no ESP-IDF deps). GESTURE_SRC overrides the source (mutation checks).
# Usage: test/host/gesture_build.sh   (run from anywhere)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SENSE="$(cd "$HERE/../.." && pwd)/main/sense"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-gesture.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

cc -std=c11 -Wall -Wextra -Werror -O2 \
  -I"$SENSE" \
  "$HERE/gesture_test.c" \
  "${GESTURE_SRC:-$SENSE/gesture.c}" \
  -lm \
  -o "$OUT/gesture_test"
"$OUT/gesture_test"
