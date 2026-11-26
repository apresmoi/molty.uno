#!/usr/bin/env bash
# picture_flow_build.sh: host build + run of changes of picture through mole_fluid.c itself (picture_flow_test.c, as the
# web module's native build), one power on per case. Needs cJSON from ESP-IDF, as web/build.sh: set IDF_PATH (default
# ~/esp/esp-idf).
# Usage: test/host/picture_flow_build.sh   (run from anywhere)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-picture-flow.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

"$HERE/../../web/build.sh" test "$OUT" "$HERE/picture_flow_test.c"
for c in early late never broken welcome-word welcome-late welcome-change say hold ask-scene screen nap bye restart creatures; do
  "$OUT/picture_flow_test" "$ROOT" "$c"
done
