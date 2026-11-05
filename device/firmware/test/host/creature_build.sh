#!/usr/bin/env bash
# creature_build.sh: host build + run of the creature package player test (creature_test.c:
# main/creature/creature_player.c) against goldens the JS reference player writes
# (tools/creatures/golden.mjs) for every creature package in packages/. Needs node.
# cJSON from ESP-IDF: set IDF_PATH (default ~/esp/esp-idf). CP_SRC overrides the player
# source (mutation checks).
# -ffp-contract=off: no fused multiply-adds, so a hash argument rounds as it does in JS
# (main/CMakeLists.txt builds the player the same way).
# The render + bench part runs each creature through the device's whole frame (the fluid core
# and the glyphs) and against the .molty duck (device/creatures/molty/duck.molty).
# Usage: test/host/creature_build.sh [out dir]   (run from anywhere; out dir: a 412 px PPM per mood)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAIN="$(cd "$HERE/../.." && pwd)/main"
REPO="$(cd "$HERE/../../../.." && pwd)"
IDF_JSON="${IDF_PATH:-$HOME/esp/esp-idf}/components/json/cJSON"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-creature.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

node "$REPO/tools/creatures/golden.mjs" "$OUT/golden" >/dev/null
cc -std=c11 -Wno-deprecated-declarations -I"$IDF_JSON" -c "$IDF_JSON/cJSON.c" -o "$OUT/cJSON.o"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -O2 -ffp-contract=off \
  -I"$MAIN/creature" -I"$MAIN/pkg" -I"$MAIN/fluid" -I"$IDF_JSON" \
  "$HERE/creature_test.c" "${CP_SRC:-$MAIN/creature/creature_player.c}" "$MAIN/pkg/pkg_parse.c" \
  "$MAIN/creature/creature_body.c" "$MAIN/creature/creature_pkg.c" "$MAIN/creature/mood_engine.c" "$MAIN/creature/rig_geo.c" \
  "$MAIN/creature/cute_spec.c" "$MAIN/creature/cute_rig.c" \
  "$MAIN/fluid/fluid_sim.c" "$MAIN/fluid/fluid_target.c" "$MAIN/fluid/fluid_draw.c" \
  "$OUT/cJSON.o" -lm -o "$OUT/creature_test"
if [ $# -ge 1 ]; then mkdir -p "$1"; fi
"$OUT/creature_test" "$REPO/packages" "$OUT/golden" "$REPO/device/creatures/molty/duck.molty" "$@"
