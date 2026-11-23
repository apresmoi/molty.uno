#!/usr/bin/env bash
# weather_flow_build.sh: host build + run of the weather on the glass through mole_fluid.c itself (weather_flow_test.c, as the web
# module's native build). Needs cJSON from ESP-IDF, as web/build.sh: set IDF_PATH (default ~/esp/esp-idf).
# Usage: test/host/weather_flow_build.sh   (run from anywhere)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-weather-flow.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

"$HERE/../../web/build.sh" test "$OUT" "$HERE/weather_flow_test.c"
"$OUT/weather_flow_test" "$ROOT"
