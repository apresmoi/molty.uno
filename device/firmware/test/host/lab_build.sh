#!/usr/bin/env bash
# lab_build.sh: host build + run of the frame rate lab's test (lab_test.c: main/diag/fps_lab.c, and the creature's reuse in
# main/creature/creature_dots.c through creature_pkg.c) on the cat package. cJSON from ESP-IDF: set IDF_PATH (default
# ~/esp/esp-idf). -ffp-contract=off as creature_build.sh (the player's hash).
# Usage: test/host/lab_build.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAIN="$(cd "$HERE/../.." && pwd)/main"
REPO="$(cd "$HERE/../../../.." && pwd)"
IDF_JSON="${IDF_PATH:-$HOME/esp/esp-idf}/components/json/cJSON"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-lab.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

cc -std=c11 -Wno-deprecated-declarations -I"$IDF_JSON" -c "$IDF_JSON/cJSON.c" -o "$OUT/cJSON.o"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -O2 -ffp-contract=off \
  -I"$MAIN/creature" -I"$MAIN/pkg" -I"$MAIN/fluid" -I"$MAIN/scene" -I"$MAIN/diag" -I"$IDF_JSON" \
  "$HERE/lab_test.c" "$MAIN/diag/fps_lab.c" "$MAIN/creature/creature_player.c" "$MAIN/pkg/pkg_parse.c" \
  "$MAIN/creature/creature_body.c" "$MAIN/creature/creature_pkg.c" "$MAIN/creature/creature_clip.c" "$MAIN/creature/creature_dots.c" "$MAIN/creature/creature_ring.c" "$MAIN/creature/mood_engine.c" "$MAIN/creature/rig_geo.c" \
  "$MAIN/creature/cute_spec.c" "$MAIN/creature/cute_rig.c" \
  "$MAIN/fluid/fluid_sim.c" "$MAIN/fluid/fluid_target.c" "$MAIN/fluid/glyph_font.c" "$MAIN/fluid/fluid_draw.c" "$MAIN/fluid/fluid_fine.c" \
  "$OUT/cJSON.o" -lm -o "$OUT/lab_test"
"$OUT/lab_test" "$REPO/packages/cat/cat.json"
