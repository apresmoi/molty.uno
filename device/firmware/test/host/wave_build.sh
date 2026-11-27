#!/usr/bin/env bash
# wave_build.sh: host build + run of the tone player (main/rhythm/wave_player.c: its tables, rates and whole-number
# ratios, the design's click sessions at 16 kHz, the speaker's yield, the picture's clock and trace). Plain C, libm.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW="$(cd "$HERE/../.." && pwd)"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-wave.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
# internal RAM is the network's (docs: 12 to 20 KB free at worst): every allocation of the tone player and its maker goes
# to PSRAM. Fails on a heap call that is not one of the PSRAM macros (their host branch is plain calloc / malloc).
bad="$(grep -nE '\b(malloc|calloc|realloc|heap_caps_(malloc|calloc))\(' "$FW/main/rhythm/wave_player.c" "$FW/main/rhythm/wave_core.c" | grep -v '#define' | grep -v 'MALLOC_CAP_SPIRAM' || true)"
grep -q 'xTaskCreatePinnedToCoreWithCaps(build_task.*MALLOC_CAP_SPIRAM' "$FW/main/rhythm/rhythm_dev.c" || bad="$bad the tables' task stack is not in PSRAM"
grep -E '#define (WAV|WAVE)_(MALLOC|CALLOC)\(n\) heap_caps' "$FW/main/rhythm/wave_player.c" "$FW/main/rhythm/wave_core.c" | grep -vq SPIRAM && bad="$bad a PSRAM macro without MALLOC_CAP_SPIRAM"
if [ -n "$bad" ]; then echo "wave_build: an internal-RAM allocation on the tone player's path: $bad"; exit 1; fi
cc -std=c11 -Wall -Wextra -Werror -O2 -ffp-contract=off -I"$FW/main/rhythm" "$HERE/wave_test.c" \
  "$FW/main/rhythm/wave_player.c" "$FW/main/rhythm/rhythm_player.c" -lm -o "$OUT/wave_test"
"$OUT/wave_test"
