#!/bin/zsh
# Hands-free serial flash: ask the running firmware to reboot into ROM download
# mode (POST /api/reboot_bootloader), wait for the USB-Serial-JTAG port, flash.
# Needs firmware already running on the device (it serves that endpoint).
# Boards with native USB on USB-C only (the 1.46: the plain build in build/). Boards
# with a USB-UART bridge (the 1.28) auto-reset into the bootloader: use esptool directly.
# Usage: ./flash_auto.sh   (from this directory; build/ must be current)
# Env: MOLTY_URL (default http://10.13.37.1), PORT (serial port, required on most
#      machines, e.g. /dev/cu.usbmodem2101 on macOS), IDF_PATH (default ~/esp/esp-idf),
#      MINIFORGE_BIN (optional: a Python env with esptool, prepended to PATH).
set -e
B=${MOLTY_URL:-http://10.13.37.1}
PORT=${PORT:-/dev/cu.usbmodem2101}
HERE="$(cd "$(dirname "$0")" && pwd)"

echo "→ requesting reboot to download mode ($B)"
curl -s -m5 -X POST "$B/api/reboot_bootloader" || true; echo
echo "→ waiting for download-mode port $PORT ..."
for i in $(seq 1 40); do
  if [ -e "$PORT" ] && ! ping -c1 -t1 10.13.37.1 >/dev/null 2>&1; then break; fi
  sleep 0.5
done
[ -e "$PORT" ] || { echo "download-mode port not found"; exit 1; }
sleep 1

[ -n "$MINIFORGE_BIN" ] && export PATH="$MINIFORGE_BIN:$PATH"
export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
. "$IDF_PATH/export.sh" >/dev/null 2>&1
cd "$HERE/build"
echo "→ flashing"
python -m esptool --chip esp32s3 -p "$PORT" -b 460800 --before no_reset --after hard_reset write_flash @flash_args | tail -4
echo "→ waiting for device to rejoin network ..."
for i in $(seq 1 30); do ping -c1 -t1 10.13.37.1 >/dev/null 2>&1 && { echo "back up after ~${i}s"; exit 0; }; sleep 1; done
echo "did not rejoin — may need a manual replug (no button)"
