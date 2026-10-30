#!/usr/bin/env bash
# director_build.sh — host build + run of the fluid director test
# (main/fluid/fluid_director.c — no ESP-IDF deps). DIRECTOR_SRC overrides the source
# (mutation checks: a copy with a guard removed must fail).
# Usage: test/host/director_build.sh   (run from anywhere)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAIN="$(cd "$HERE/../.." && pwd)/main"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-director.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

cc -std=c11 -Wall -Wextra -Werror -O2 \
  -I"$MAIN/fluid" -I"$MAIN/creature" \
  "$HERE/director_test.c" \
  "${DIRECTOR_SRC:-$MAIN/fluid/fluid_director.c}" \
  -lm \
  -o "$OUT/director_test"
"$OUT/director_test"
