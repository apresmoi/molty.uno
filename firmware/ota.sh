#!/bin/zsh
# Hands-free app update over HTTP (no BOOT button, no esptool). Needs firmware
# already running on the device (it serves POST /api/ota).
# Usage: ./ota.sh   (from this directory; build/molty.bin must be current)
# Env: MOLTY_URL (default http://10.13.37.1, the USB-C link; or http://molty.local)
set -e
B=${MOLTY_URL:-http://10.13.37.1}
BIN="$(cd "$(dirname "$0")" && pwd)/build/molty.bin"
HOST=${B#http://}; HOST=${HOST%%/*}
echo "OTA → $B   image: $(wc -c < "$BIN") bytes"
resp=$(curl -s -m120 -X POST "$B/api/ota" --data-binary @"$BIN")
echo "  $resp"
case "$resp" in *'"ok":true'*) ;; *) echo "OTA rejected"; exit 1;; esac
echo "→ rebooting into new image, waiting for rejoin ..."
sleep 3
for i in $(seq 1 40); do ping -c1 -t1 "$HOST" >/dev/null 2>&1 && { echo "back up after ~$((i+3))s"; exit 0; }; sleep 1; done
echo "did not rejoin in time (rollback may revert on next reset)"; exit 1
