#!/usr/bin/env bash
# lcd_build.sh: host build + run of the CO5300 driver's transfers (boards/.../amoled-1.75c/lcd_q.c) against a fake SPI
# device (lcd_q_test.c). Plain C, no ESP-IDF. LCD_Q_SRC overrides the source (mutation checks).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOARD="$(cd "$HERE/../.." && pwd)/boards/waveshare/esp32-s3-touch-amoled-1.75c"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-lcd.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -I"$BOARD" "$HERE/lcd_q_test.c" "${LCD_Q_SRC:-$BOARD/lcd_q.c}" -o "$OUT/lcd_q_test"
"$OUT/lcd_q_test"
