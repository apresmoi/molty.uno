#!/usr/bin/env bash
# Host gate for the exact device protocol helpers; no device or network access.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LINK="$HERE/../../main/link"
AUDIO="$HERE/../../main/audio"
# the 1.46's audio driver: its streaming seam is what link_dma_test exercises
HW="$HERE/../../boards/waveshare/esp32-s3-touch-lcd-1.46"
IDF_JSON="${IDF_PATH:-$HOME/esp/esp-idf}/components/json/cJSON"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-link-test.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wno-deprecated-declarations ${LINK_TEST_CFLAGS:-} -I"$IDF_JSON" \
  -c "$IDF_JSON/cJSON.c" -o "$OUT/cJSON.o"
cc -std=c11 -Wall -Wextra -Werror -O2 ${LINK_TEST_CFLAGS:-} \
  -I"$LINK" -I"$IDF_JSON" "$HERE/link_test.c" \
  "$LINK/link_protocol.c" "$OUT/cJSON.o" -lm -o "$OUT/link_test"
"$OUT/link_test"

# Shadow only SDK/driver boundaries; compile the real firmware control paths.
mkdir -p "$OUT/stubs/freertos" "$OUT/stubs/driver" "$OUT/stubs/mbedtls"
for header in esp_err.h esp_app_desc.h esp_random.h esp_timer.h nvs.h \
  esp_http_server.h esp_websocket_client.h esp_crt_bundle.h esp_heap_caps.h \
  esp_netif.h esp_wifi.h esp_log.h molty_board.h mole_creature.h mole_diag.h sdkconfig.h \
  freertos/idf_additions.h \
  freertos/FreeRTOS.h freertos/task.h freertos/queue.h freertos/semphr.h \
  freertos/ringbuf.h driver/gpio.h mbedtls/base64.h; do
  printf '#include "link_sdk_stub.h"\n' > "$OUT/stubs/$header"
done

# Compile the actual driver streaming seam without unrelated mic/board setup.
python3 - "$HW/mole_audio.c" "$OUT/audio_stream_under_test.inc" <<'PY'
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
start = source.index('esp_err_t mole_audio_tx_stream_begin(')
end = source.index('static void rx_release(void) {', start)
Path(sys.argv[2]).write_text(source[start:end])
PY
cc -std=c11 -Wall -Wextra -Werror -O2 ${LINK_TEST_CFLAGS:-} \
  -I"$HERE" -I"$OUT" -I"$IDF_JSON" "$HERE/link_dma_test.c" -o "$OUT/link_dma_test"
# The self-update's screen writer (link_cmd_test: update progress owns the screen).
python3 - "$HERE/../../main/update/self_update.c" "$OUT/update_screen.inc" <<'PY'
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
start = source.index('static void screen_word(')
end = source.index('static void screen_home(', start)
Path(sys.argv[2]).write_text(source[start:end])
PY
"$OUT/link_dma_test"
for test in identity audio_pair client cmd ask; do
  extra=()
  if [ "$test" = identity ]; then extra+=("$LINK/link_identity.c"); fi
  if [ "$test" = audio_pair ]; then extra+=("$AUDIO/vad.c" "$AUDIO/chirp.c"); fi
  if [ "$test" = ask ]; then extra+=("$HERE/../../main/fluid/fluid_ask.c" "$HERE/../../main/sense/gesture.c"); fi
  cc -std=c11 -Wall -Wextra -Werror -O2 ${LINK_TEST_CFLAGS:-} \
    -I"$OUT/stubs" -I"$OUT" -I"$HERE" -I"$LINK" -I"$AUDIO" -I"$HERE/../../main/fluid" -I"$HERE/../../main/sense" -I"$HERE/../../main/update" -I"$IDF_JSON" \
    "$HERE/link_${test}_test.c" "$HERE/link_sdk_stub.c" \
    "$LINK/link_protocol.c" ${extra[@]+"${extra[@]}"} "$OUT/cJSON.o" -lm -o "$OUT/link_${test}_test"
  "$OUT/link_${test}_test"
done
