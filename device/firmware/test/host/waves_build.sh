#!/usr/bin/env bash
# waves_build.sh: host build + run of the waves package's scope on the firmware's own screen (waves_test.c: mole_fluid.c
# as the web module's native build, with the molty-waves skill's own chord kinds and packages/waves). WAVES_RENDER=<dir>
# also writes the screen at a few states there as PNG. Needs cJSON from ESP-IDF (IDF_PATH, default ~/esp/esp-idf) and node.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-waves.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
node --input-type=module -e "import { wire } from '$ROOT/skills/molty-waves/scripts/chords.mjs'; console.log(JSON.stringify(wire()));" > "$OUT/chords.json"
"$HERE/../../web/build.sh" test "$OUT" "$HERE/waves_test.c"
if [ -n "${WAVES_RENDER:-}" ]; then
  mkdir -p "$WAVES_RENDER"
  "$OUT/waves_test" "$ROOT" "$OUT/chords.json" "$WAVES_RENDER"
  for f in "$WAVES_RENDER"/*.ppm; do sips -s format png "$f" --out "${f%.ppm}.png" >/dev/null && rm "$f"; done
else
  "$OUT/waves_test" "$ROOT" "$OUT/chords.json"
fi
