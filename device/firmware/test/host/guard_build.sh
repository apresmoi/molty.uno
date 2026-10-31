#!/usr/bin/env bash
# guard_build.sh — host build + run of the internal-RAM / Wi-Fi recovery policies:
# main/diag/heap_guard.c (warn + shed) and main/net/wifi_retry.c (reconnect after a drop).
# Plain C, no ESP-IDF. HEAP_GUARD_SRC / WIFI_RETRY_SRC override a source (mutation checks).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAIN="$(cd "$HERE/../.." && pwd)/main"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-guard.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
CC="cc -std=c11 -Wall -Wextra -Werror -O2"
$CC -I"$MAIN/diag" "$HERE/heap_guard_test.c" "${HEAP_GUARD_SRC:-$MAIN/diag/heap_guard.c}" -o "$OUT/heap_guard_test"
"$OUT/heap_guard_test"
$CC -I"$MAIN/net" "$HERE/wifi_retry_test.c" "${WIFI_RETRY_SRC:-$MAIN/net/wifi_retry.c}" -o "$OUT/wifi_retry_test"
"$OUT/wifi_retry_test"
