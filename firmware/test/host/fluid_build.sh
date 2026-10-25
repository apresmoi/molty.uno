#!/usr/bin/env bash
# fluid_build.sh — host build + run of the "fluid" creature core test
# (main/fluid/fluid_{sim,target,draw}.c — no ESP-IDF deps).
# Usage: test/host/fluid_build.sh   (run from anywhere)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FLUID="$(cd "$HERE/../.." && pwd)/main/fluid"
OUT="$HERE/fluid_test_bin"

cc -std=c11 -D_POSIX_C_SOURCE=199309L -Wall -Wextra -O2 \
  -I"$FLUID" \
  "$HERE/fluid_test.c" \
  "$FLUID/fluid_sim.c" \
  "$FLUID/fluid_target.c" \
  "$FLUID/fluid_draw.c" \
  -lm \
  -o "$OUT"

status=0
"$OUT" || status=$?
rm -f "$OUT"
exit $status
