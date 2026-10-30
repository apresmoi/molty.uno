#!/usr/bin/env bash
# ask_build.sh — host build + run of the question screen's geometry test
# (main/fluid/fluid_ask.c — no ESP-IDF deps). ASK_SRC overrides the source (mutation checks).
# Usage: test/host/ask_build.sh   (run from anywhere)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FLUID="$(cd "$HERE/../.." && pwd)/main/fluid"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-ask.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

cc -std=c11 -Wall -Wextra -Werror -O2 \
  -I"$FLUID" \
  "$HERE/ask_test.c" \
  "${ASK_SRC:-$FLUID/fluid_ask.c}" \
  -lm \
  -o "$OUT/ask_test"
"$OUT/ask_test"
