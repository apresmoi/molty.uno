#!/usr/bin/env bash
# cute_build.sh — host build + run of the cute-kit runtime test (creature/cute_spec +
# cute_rig + the fluid core). Tests .molty files from device/creatures/molty/ (export them
# with `node device/creatures/tools/molty-export.mjs all`). Optional: an outdir for mood + action
# previews. The actions' curves are held to their JS twin: act_golden.txt (act-golden.mjs).
# Usage: test/host/cute_build.sh [outdir]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAIN="$(cd "$HERE/../.." && pwd)/main"
MOLTY="$(cd "$HERE/../../../creatures/molty" && pwd)"
OUT="$HERE/cute_test_bin"

cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -O2 \
  -I"$MAIN/creature" -I"$MAIN/fluid" \
  "$HERE/cute_test.c" \
  "$MAIN/creature/cute_spec.c" \
  "$MAIN/creature/cute_rig.c" \
  "$MAIN/fluid/fluid_sim.c" \
  "$MAIN/fluid/fluid_target.c" \
  "$MAIN/fluid/fluid_draw.c" \
  -lm \
  -o "$OUT"

args=(-g "$HERE/act_golden.txt")
if [ $# -ge 1 ]; then mkdir -p "$1"; args+=(-o "$1"); fi
status=0
# the actions' JS twin must still write the golden the firmware is held to
if command -v node >/dev/null; then
  node "$HERE/../../../creatures/tools/act-golden.mjs" --check || status=$?
else
  echo "node not found: act_golden.txt is not checked against device/creatures/js/actions.js"
fi
"$OUT" "${args[@]}" "$MOLTY"/duck.molty "$MOLTY"/kitten.molty || status=$?
rm -f "$OUT"
exit $status
