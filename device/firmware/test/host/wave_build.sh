#!/usr/bin/env bash
# wave_build.sh: host build + run of the tone player (main/rhythm/wave_player.c: its tables, rates and whole-number
# ratios, the design's click sessions at 16 kHz, the speaker's yield, the picture's clock and trace). Plain C, libm.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW="$(cd "$HERE/../.." && pwd)"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-wave.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -O2 -ffp-contract=off -I"$FW/main/rhythm" "$HERE/wave_test.c" \
  "$FW/main/rhythm/wave_player.c" "$FW/main/rhythm/rhythm_player.c" -lm -o "$OUT/wave_test"
"$OUT/wave_test"
