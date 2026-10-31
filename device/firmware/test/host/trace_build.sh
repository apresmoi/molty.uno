#!/usr/bin/env bash
# trace_build.sh — host build + run of the turn-trace modules (docs/AGENT-LINK.md "Turn trace"):
# the uplink ring (main/link/link_uplink.c), the shake settle (main/sense/shake_settle.c) and the
# listener reactions (main/link/listen_react.c). Plain C, no ESP-IDF. UPLINK_SRC / SETTLE_SRC /
# REACT_SRC override a source (mutation checks).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAIN="$(cd "$HERE/../.." && pwd)/main"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-trace.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
CC="cc -std=c11 -Wall -Wextra -Werror -O2"
$CC -I"$MAIN/link" "$HERE/uplink_test.c" "${UPLINK_SRC:-$MAIN/link/link_uplink.c}" -o "$OUT/uplink_test"
"$OUT/uplink_test"
$CC -I"$MAIN/sense" "$HERE/settle_test.c" "${SETTLE_SRC:-$MAIN/sense/shake_settle.c}" -lm -o "$OUT/settle_test"
"$OUT/settle_test"
$CC -I"$MAIN/link" "$HERE/react_test.c" "${REACT_SRC:-$MAIN/link/listen_react.c}" -o "$OUT/react_test"
"$OUT/react_test"
