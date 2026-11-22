#!/usr/bin/env bash
# sky_build.sh: host build + run of the sky behind the creature (main/creature/sky.c: the moon's phase, distance, size
# and dots, the falling star). Plain C, no ESP-IDF. SKY_SRC overrides the source (mutation checks).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CREATURE="$(cd "$HERE/../.." && pwd)/main/creature"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-sky.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -O2 -I"$CREATURE" "$HERE/sky_test.c" "${SKY_SRC:-$CREATURE/sky.c}" -lm -o "$OUT/sky_test"
"$OUT/sky_test"
