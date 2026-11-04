#!/usr/bin/env bash
# pkg_build.sh: host build + run of the package tests: the strict parser and the store
# (pkg_test.c: main/pkg) and the scene player with the director (scene_test.c: main/scene,
# main/fluid), on fixtures/shapes.json; scene_test also estimates a busy scene's frame cost
# against the creature (duck.molty). cJSON from ESP-IDF: set IDF_PATH (default ~/esp/esp-idf).
# PKG_SRC / SCENE_SRC override a source (mutation checks).
# Usage: test/host/pkg_build.sh   (run from anywhere)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAIN="$(cd "$HERE/../.." && pwd)/main"
MOLTY="$(cd "$HERE/../../../creatures/molty" && pwd)"
IDF_JSON="${IDF_PATH:-$HOME/esp/esp-idf}/components/json/cJSON"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-pkg.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

cc -std=c11 -Wno-deprecated-declarations -I"$IDF_JSON" -c "$IDF_JSON/cJSON.c" -o "$OUT/cJSON.o"
cc -std=c11 -Wall -Wextra -Werror -O2 \
  -I"$MAIN/pkg" -I"$IDF_JSON" \
  "$HERE/pkg_test.c" "${PKG_SRC:-$MAIN/pkg/pkg_parse.c}" "$MAIN/pkg/pkg_store.c" \
  "$OUT/cJSON.o" -lm -o "$OUT/pkg_test"
"$OUT/pkg_test" "$HERE/fixtures/shapes.json"

cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -O2 \
  -I"$MAIN/pkg" -I"$MAIN/scene" -I"$MAIN/fluid" -I"$MAIN/creature" -I"$IDF_JSON" \
  "$HERE/scene_test.c" "${SCENE_SRC:-$MAIN/scene/scene_player.c}" "$MAIN/pkg/pkg_parse.c" \
  "$MAIN/fluid/fluid_director.c" "$MAIN/fluid/fluid_sim.c" "$MAIN/fluid/fluid_target.c" "$MAIN/fluid/fluid_draw.c" \
  "$MAIN/creature/cute_spec.c" "$MAIN/creature/cute_rig.c" "$MAIN/creature/mood_engine.c" \
  "$OUT/cJSON.o" -lm -o "$OUT/scene_test"
"$OUT/scene_test" "$HERE/fixtures" "$MOLTY/duck.molty"
