#!/usr/bin/env bash
# vad_build.sh — host build + run of the listen end-of-speech detector test
# (main/audio/vad.c — no ESP-IDF deps). VAD_SRC overrides the source (mutation checks).
# Usage: test/host/vad_build.sh   (run from anywhere)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AUDIO="$(cd "$HERE/../.." && pwd)/main/audio"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-vad.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

cc -std=c11 -Wall -Wextra -Werror -O2 \
  -I"$AUDIO" \
  "$HERE/vad_test.c" \
  "${VAD_SRC:-$AUDIO/vad.c}" \
  -lm \
  -o "$OUT/vad_test"
"$OUT/vad_test"

# Again under ASan + UBSan: a test that reads past its own buffer passes silently at
# -O2 (it did once). Skipped only where the compiler has no sanitizer runtime.
if echo 'int main(void){return 0;}' | cc -x c -fsanitize=address,undefined -o "$OUT/probe" - 2>/dev/null; then
  cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -fno-sanitize-recover=all \
    -I"$AUDIO" "$HERE/vad_test.c" "${VAD_SRC:-$AUDIO/vad.c}" -lm -o "$OUT/vad_test_san"
  ASAN_OPTIONS=detect_leaks=0 "$OUT/vad_test_san" | tail -1 | sed 's/^/sanitized /'
else
  echo "vad_build: no ASan/UBSan runtime here, sanitized run skipped" >&2
fi
