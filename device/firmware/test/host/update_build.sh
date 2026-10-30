#!/usr/bin/env bash
# update_build.sh — host build + run of the firmware self-update rules
# (main/update/update_core.c + cJSON from ESP-IDF: set IDF_PATH, default ~/esp/esp-idf).
# UPDATE_SRC overrides the source (mutation checks).
# Usage: test/host/update_build.sh   (run from anywhere)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UPD="$(cd "$HERE/../.." && pwd)/main/update"
IDF_JSON="${IDF_PATH:-$HOME/esp/esp-idf}/components/json/cJSON"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-update.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

cc -std=c11 -Wno-deprecated-declarations -I"$IDF_JSON" -c "$IDF_JSON/cJSON.c" -o "$OUT/cJSON.o"
cc -std=c11 -Wall -Wextra -Werror -O2 \
  -I"$UPD" -I"$IDF_JSON" \
  "$HERE/update_test.c" \
  "${UPDATE_SRC:-$UPD/update_core.c}" \
  "$OUT/cJSON.o" -lm \
  -o "$OUT/update_test"
"$OUT/update_test"
