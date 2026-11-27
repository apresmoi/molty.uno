#!/usr/bin/env bash
# say_flow_build.sh: host build + run of a say's words against its voice through mole_fluid.c itself (say_flow_test.c, as
# the web module's native build), one power on per case. Needs cJSON from ESP-IDF, as web/build.sh: set IDF_PATH (default
# ~/esp/esp-idf).
# Usage: test/host/say_flow_build.sh   (run from anywhere)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-say-flow.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

"$HERE/../../web/build.sh" test "$OUT" "$HERE/say_flow_test.c"
for c in filler filler2 long voicebox fast latecues listen; do
  "$OUT/say_flow_test" "$ROOT" "$c"
done
