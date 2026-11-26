#!/usr/bin/env bash
# rhythm_build.sh: host build + run of the beat player (main/rhythm: its timing on the sample clock, the mix, a tempo
# change, mute, pause, count-in, phrase) against the molty-rhythm skill's own library (library.mjs, as it sends it).
# Plain C + cJSON from ESP-IDF (IDF_PATH, default ~/esp/esp-idf) and node for the library.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW="$(cd "$HERE/../.." && pwd)"
REPO="$(cd "$FW/../.." && pwd)"
JSON="${IDF_PATH:-$HOME/esp/esp-idf}/components/json/cJSON"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-rhythm.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
node --input-type=module -e "import { load, wireLibrary } from '$REPO/skills/molty-rhythm/scripts/library.mjs'; console.log(JSON.stringify(wireLibrary(load())));" > "$OUT/lib.json"
cc -std=c11 -Wall -Wextra -Werror -O2 -ffp-contract=off -I"$FW/main/rhythm" -I"$JSON" "$HERE/rhythm_test.c" \
  "$FW/main/rhythm/rhythm_player.c" "$FW/main/rhythm/rhythm_wire.c" "$JSON/cJSON.c" -lm -o "$OUT/rhythm_test"
"$OUT/rhythm_test" "$OUT/lib.json"
