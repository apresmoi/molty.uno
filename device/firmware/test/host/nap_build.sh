#!/usr/bin/env bash
# nap_build.sh: host build + run of the screen's nap (main/fluid/screen_nap.c: the screen off a while after the
# creature fell asleep, what keeps it on, what wakes it, the pick-up). Plain C, no ESP-IDF.
# NAP_SRC overrides the source (mutation checks).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FLUID="$(cd "$HERE/../.." && pwd)/main/fluid"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-nap.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -O2 -I"$FLUID" "$HERE/nap_test.c" "${NAP_SRC:-$FLUID/screen_nap.c}" -lm -o "$OUT/nap_test"
"$OUT/nap_test"
