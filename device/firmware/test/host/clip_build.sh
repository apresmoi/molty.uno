#!/usr/bin/env bash
# clip_build.sh: host build + run of the rigged creature test (clip_test.c: main/creature/creature_clip.c and
# creature_dots.c) against goldens the JS reference writes (tools/creatures/clip-golden.mjs) for every rigged creature
# package in packages/, plus its routes and its cost a frame. Needs node. cJSON from ESP-IDF: set IDF_PATH (default
# ~/esp/esp-idf).
# Usage: test/host/clip_build.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAIN="$(cd "$HERE/../.." && pwd)/main"
REPO="$(cd "$HERE/../../../.." && pwd)"
IDF_JSON="${IDF_PATH:-$HOME/esp/esp-idf}/components/json/cJSON"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-clip.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

node "$REPO/tools/creatures/clip-golden.mjs" "$OUT/golden" >/dev/null
cc -std=c11 -Wno-deprecated-declarations -I"$IDF_JSON" -c "$IDF_JSON/cJSON.c" -o "$OUT/cJSON.o"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -O2 \
  -I"$MAIN/creature" -I"$IDF_JSON" \
  "$HERE/clip_test.c" "$MAIN/creature/creature_clip.c" "$MAIN/creature/creature_dots.c" \
  "$OUT/cJSON.o" -lm -o "$OUT/clip_test"
"$OUT/clip_test" "$REPO/packages" "$OUT/golden"
