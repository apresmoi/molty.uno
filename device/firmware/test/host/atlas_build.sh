#!/usr/bin/env bash
# atlas_build.sh: the sky atlas, both halves. The agent's: the molty-atlas skill's sky maths against known values
# (skills/molty-atlas/scripts/sky.test.mjs). The device's: atlas_test.c (mole_fluid.c as the web module's native
# build) fed what the skill sends for Berlin on 2026-11-27 at 22:00, and with no place, with packages/atlas.
# ATLAS_RENDER=<dir> also writes the screens there as PNG. Needs cJSON from ESP-IDF (IDF_PATH, default ~/esp/esp-idf)
# and node.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-atlas.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
node --test "$ROOT/skills/molty-atlas/scripts/sky.test.mjs"
mkdir -p "$OUT/berlin" "$OUT/none"
echo '{"name":"Berlin","lat":52.5,"lon":13.4,"tz":"Europe/Berlin"}' > "$OUT/berlin/atlas-place.json"
for p in berlin none; do
  MOLTY_CONFIG_DIR="$OUT/$p" node "$ROOT/skills/molty-atlas/scripts/atlas.mjs" show orion --at 2026-11-27T21:00:00Z --dry-run | grep '^{' > "$OUT/$p.jsonl"
done
grep -nE '\b(malloc|calloc|realloc|heap_caps_(malloc|calloc))\(' "$HERE/../../main/scene/atlas.c" | grep -v '#define' | grep -v 'MALLOC_CAP_SPIRAM' \
  && { echo "atlas.c: allocate through ATLAS_CALLOC (PSRAM)"; exit 1; }
"$HERE/../../web/build.sh" test "$OUT" "$HERE/atlas_test.c"
if [ -n "${ATLAS_RENDER:-}" ]; then
  mkdir -p "$ATLAS_RENDER"
  "$OUT/atlas_test" "$ROOT" "$OUT/berlin.jsonl" "$OUT/none.jsonl" "$ATLAS_RENDER"
  for f in "$ATLAS_RENDER"/*.ppm; do sips -s format png "$f" --out "${f%.ppm}.png" >/dev/null && rm "$f"; done
else
  "$OUT/atlas_test" "$ROOT" "$OUT/berlin.jsonl" "$OUT/none.jsonl"
fi
