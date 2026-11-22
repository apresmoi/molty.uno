#!/usr/bin/env bash
# tap_build.sh: host build + run of what a touch does to the water and the creature (tap_test.c: mole_fluid.c itself,
# as the web module's native build). Needs cJSON from ESP-IDF, as web/build.sh: set IDF_PATH (default ~/esp/esp-idf).
# Usage: test/host/tap_build.sh   (run from anywhere)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-tap.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

"$HERE/../../web/build.sh" test "$OUT" "$HERE/tap_test.c"
"$OUT/tap_test" "$ROOT"
