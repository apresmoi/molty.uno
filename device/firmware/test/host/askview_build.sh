#!/usr/bin/env bash
# askview_build.sh — host build + run of the question's look over a moving scene (askview_test.c: main/fluid/
# fluid_askview.c with the sim, the drawer and a panel cost model). ASKVIEW_SRC overrides the source (mutation checks);
# ASKVIEW_DUMP=<dir> writes frames as PPM to look at by eye.
# Usage: test/host/askview_build.sh   (run from anywhere)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FLUID="$(cd "$HERE/../.." && pwd)/main/fluid"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-askview.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -O2 \
  -I"$FLUID" \
  "$HERE/askview_test.c" \
  "${ASKVIEW_SRC:-$FLUID/fluid_askview.c}" "$FLUID/fluid_ask.c" \
  "$FLUID/fluid_sim.c" "$FLUID/fluid_target.c" "$FLUID/fluid_draw.c" "$FLUID/fluid_fine.c" "$FLUID/glyph_font.c" \
  -lm \
  -o "$OUT/askview_test"
"$OUT/askview_test"
