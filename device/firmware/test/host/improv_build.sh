#!/usr/bin/env bash
# improv_build.sh — host build + run of the Improv Wi-Fi Serial framing test
# (main/net/improv_proto.c — no ESP-IDF deps).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NET="$(cd "$HERE/../.." && pwd)/main/net"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-improv.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -O2 -I"$NET" "$HERE/improv_test.c" "$NET/improv_proto.c" -o "$OUT/improv_test"
"$OUT/improv_test"
