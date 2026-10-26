#!/usr/bin/env bash
# fluid_golden_build.sh — host build + run of the fluid's pixel golden
# (fluid_golden_test.c vs fluid_golden.txt). -ffp-contract=off keeps the float
# results independent of whether the compiler fuses multiply-adds, so the same
# platform's golden holds across compilers and -O levels.
# Usage: test/host/fluid_golden_build.sh [--print]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FLUID="$(cd "$HERE/../.." && pwd)/main/fluid"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-golden.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

cc -std=c11 -D_POSIX_C_SOURCE=199309L -Wall -Wextra -O2 -ffp-contract=off \
  -I"$FLUID" \
  "$HERE/fluid_golden_test.c" \
  "$FLUID/fluid_sim.c" \
  "$FLUID/fluid_target.c" \
  "$FLUID/fluid_draw.c" \
  -lm \
  -o "$OUT/fluid_golden_test"

if [ "${1:-}" = "--print" ]; then
  "$OUT/fluid_golden_test" --print
else
  "$OUT/fluid_golden_test" "$HERE/fluid_golden.txt"
fi
