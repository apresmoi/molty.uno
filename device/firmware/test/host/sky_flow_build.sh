#!/usr/bin/env bash
# sky_flow_build.sh: host build + run of the sky behind the cat through mole_fluid.c itself (sky_flow_test.c, as the web
# module's native build). Needs cJSON from ESP-IDF, as web/build.sh: set IDF_PATH (default ~/esp/esp-idf).
# Usage: test/host/sky_flow_build.sh   (run from anywhere)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-sky-flow.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

"$HERE/../../web/build.sh" test "$OUT" "$HERE/sky_flow_test.c"
"$OUT/sky_flow_test" "$ROOT"
