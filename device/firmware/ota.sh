#!/bin/zsh
# Hands-free app update over HTTP (no BOOT button, no esptool). Needs firmware
# already running on the device (it serves POST /api/ota).
# Usage: ./ota.sh   (from this directory; the build must be current)
# Env: MOLTY_URL (default http://10.13.37.1, the USB-C link; or http://molty.local)
#      BOARD     a board built with `make firmware BOARD=...` (folder or name): uploads
#                build/<name>/molty.bin. Unset: build/molty.bin (the plain 1.46 build).
# The image's board tag is checked here and again by the device (/api/ota).
set -e
B=${MOLTY_URL:-http://10.13.37.1}
HERE="$(cd "$(dirname "$0")" && pwd)"
BOARDS="$HERE/../../tools/scripts/firmware-boards.py"
if [[ -n "$BOARD" ]]; then
  BIN="$HERE/build/$(python3 "$BOARDS" name "$BOARD")/molty.bin"
else
  BIN="$HERE/build/molty.bin"; BOARD=waveshare/esp32-s3-touch-lcd-1.46
fi
[[ -f "$BIN" ]] || { echo "no image at $BIN (build it first)"; exit 1; }
python3 "$BOARDS" check-image "$BIN" "$BOARD"
HOST=${B#http://}; HOST=${HOST%%/*}
echo "OTA → $B   image: $(wc -c < "$BIN") bytes"
resp=$(curl -s -m120 -X POST "$B/api/ota" --data-binary @"$BIN")
echo "  $resp"
case "$resp" in *'"ok":true'*) ;; *) echo "OTA rejected"; exit 1;; esac
echo "→ rebooting into new image, waiting for rejoin ..."
sleep 3
for i in $(seq 1 40); do ping -c1 -t1 "$HOST" >/dev/null 2>&1 && { echo "back up after ~$((i+3))s"; exit 0; }; sleep 1; done
echo "did not rejoin in time (rollback may revert on next reset)"; exit 1
