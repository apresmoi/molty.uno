#!/usr/bin/env bash
# beat_build.sh: host build + run of the rhythm package's clock and the wheel on the firmware's own screen (beat_test.c:
# mole_fluid.c as the web module's native build, with the molty-rhythm skill's own library and packages/rhythm), and
# sense/ring_input.c. Needs cJSON from ESP-IDF (IDF_PATH, default ~/esp/esp-idf) and node for the library.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-beat.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
node --input-type=module -e "import { load, wireLibrary } from '$ROOT/skills/molty-rhythm/scripts/library.mjs'; console.log(JSON.stringify(wireLibrary(load())));" > "$OUT/lib.json"
"$HERE/../../web/build.sh" test "$OUT" "$HERE/beat_test.c"
"$OUT/beat_test" "$ROOT" "$OUT/lib.json"
