#!/usr/bin/env bash
# guard_build.sh — host build + run of the internal-RAM / Wi-Fi recovery policies:
# main/diag/heap_guard.c (warn + shed), main/net/wifi_retry.c (reconnect after a drop) and
# main/diag/sta_watch.c (rssi only while associated), main/net/recv_bound.c (a silent upload is
# given up), main/diag/loop_watch.c (abort on a wedged event loop / esp_timer task),
# main/diag/flash_gate.c (flash writes that can wait hold while the screen animates).
# Plain C, no ESP-IDF.
# HEAP_GUARD_SRC / WIFI_RETRY_SRC / RECV_BOUND_SRC / STA_WATCH_SRC / LOOP_WATCH_SRC / FLASH_GATE_SRC override a source (mutation checks).
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
$CC -I"$MAIN/net" "$HERE/recv_bound_test.c" "${RECV_BOUND_SRC:-$MAIN/net/recv_bound.c}" -o "$OUT/recv_bound_test"
"$OUT/recv_bound_test"
$CC -I"$MAIN/diag" "$HERE/sta_watch_test.c" "${STA_WATCH_SRC:-$MAIN/diag/sta_watch.c}" -o "$OUT/sta_watch_test"
"$OUT/sta_watch_test"
$CC -I"$MAIN/diag" "$HERE/loop_watch_test.c" "${LOOP_WATCH_SRC:-$MAIN/diag/loop_watch.c}" -o "$OUT/loop_watch_test"
"$OUT/loop_watch_test"
$CC -I"$MAIN/diag" "$HERE/flash_gate_test.c" "${FLASH_GATE_SRC:-$MAIN/diag/flash_gate.c}" -o "$OUT/flash_gate_test"
"$OUT/flash_gate_test"
# mdns 1.11.1's mdns_unregister_netif takes its lock twice: called from the Wi-Fi disconnect
# handler it wedged the event loop and the esp_timer task for good (loop_watch.h). The STA netif
# stays registered; a drop disables its pcbs instead.
if grep -n 'mdns_unregister_netif *(' "${NETWORK_SRC:-$MAIN/net/network.c}"; then
  echo "FAIL: network.c calls mdns_unregister_netif (deadlocks in mdns < 1.11.3)"; exit 1
fi
echo "MDNS GUARD PASS: the Wi-Fi drop never unregisters the STA netif from mDNS"
