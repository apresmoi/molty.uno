/*
 * SPDX-License-Identifier: MIT
 *
 * molty_board.c — the core side of the board seam (molty_board.h): capability
 * bookkeeping and the guarded wrappers the core calls. Nothing here knows a pin.
 */
#include "molty_board.h"
#include "link_pair.h"
#include "sense.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "sdkconfig.h"

static const char *TAG = "board";

#define B molty_board_desc

static uint32_t s_caps;   /* what came up; written once by molty_board_init() before any task */

static const char *const CAP_NAMES[MOLTY_CAP_COUNT] = {
    "display", "touch", "imu", "mic", "speaker", "btn_accept", "btn_reject", "battery", "usb_net", "serial",
};

const char *molty_cap_name(int i) { return i >= 0 && i < MOLTY_CAP_COUNT ? CAP_NAMES[i] : NULL; }

/* ── image tag ────────────────────────────────────────────────────────────── */

/* .rodata_custom_desc is linked right after esp_app_desc_t (IDF's sections.ld), so
 * the tag sits at a fixed image offset: an OTA upload can be checked from its first
 * bytes, before a 1.28 image overwrites a 1.46 (or the reverse) and boots there. */
typedef struct {
    char magic[8];
    char board[MOLTY_IMAGE_TAG_BYTES - 8];
} molty_image_tag_t;
_Static_assert(sizeof(molty_image_tag_t) == MOLTY_IMAGE_TAG_BYTES, "image tag size");
_Static_assert(sizeof(CONFIG_MOLTY_BOARD_DIR) <= MOLTY_IMAGE_TAG_BYTES - 8, "board folder name too long");

const __attribute__((section(".rodata_custom_desc"))) molty_image_tag_t s_image_tag = {
    .magic = { 'M', 'O', 'L', 'T', 'Y', 'B', 'R', 'D' },   /* MOLTY_IMAGE_TAG_MAGIC, no NUL */
    .board = CONFIG_MOLTY_BOARD_DIR,
};

const char *molty_board_dir(void) { return s_image_tag.board; }

int molty_board_image_check(const uint8_t *img, size_t len, char *other, size_t other_len)
{
    if (len < MOLTY_IMAGE_TAG_OFFSET + MOLTY_IMAGE_TAG_BYTES) return -1;
    if (img[0] != 0xE9) return -1;                                 /* ESP image magic   */
    const uint8_t app_magic[4] = { 0x32, 0x54, 0xCD, 0xAB };       /* ESP_APP_DESC_MAGIC_WORD */
    if (memcmp(img + 0x20, app_magic, 4) != 0) return -1;
    const molty_image_tag_t *t = (const molty_image_tag_t *)(img + MOLTY_IMAGE_TAG_OFFSET);
    const char *board = MOLTY_LEGACY_BOARD_DIR;
    char buf[sizeof(t->board) + 1];
    if (memcmp(t->magic, MOLTY_IMAGE_TAG_MAGIC, sizeof(t->magic)) == 0) {
        memcpy(buf, t->board, sizeof(t->board));
        buf[sizeof(t->board)] = '\0';
        board = buf;
    }
    if (strcmp(board, s_image_tag.board) == 0) return 1;
    if (other && other_len) snprintf(other, other_len, "%s", board);
    return 0;
}

/* a cap counts only if the board both declares it and provides its ops */
static uint32_t caps_with_ops(void)
{
    uint32_t c = B.caps;
    if (!B.framebuffer || !B.flush || !B.flush_rows) c &= ~MOLTY_CAP_DISPLAY;
    if (!B.touch_poll) c &= ~MOLTY_CAP_TOUCH;
    if (!B.imu_get) c &= ~MOLTY_CAP_IMU;
    const molty_speaker_ops_t *sp = B.speaker;
    if (!sp || !sp->play_raw || !sp->set_volume || !sp->stream_begin || !sp->stream_write ||
        !sp->stream_drain || !sp->stream_end) c &= ~MOLTY_CAP_SPEAKER;
    const molty_mic_ops_t *mic = B.mic;
    if (!mic || !mic->record_raw || !mic->stream_start || !mic->stream_read || !mic->stream_stop ||
        !mic->stream_active) c &= ~MOLTY_CAP_MIC;
    if (!B.button_pressed) c &= ~(MOLTY_CAP_BTN_ACCEPT | MOLTY_CAP_BTN_REJECT);
    if (!B.battery_mv) c &= ~MOLTY_CAP_BATTERY;
    return c;
}

void molty_board_early_init(void)
{
    if (B.early_init) B.early_init();
}

static char s_mismatch[128];

const char *molty_board_mismatch(void) { return s_mismatch[0] ? s_mismatch : NULL; }

/* The image's board vs the chip it runs on. A wrong-board image usually dies earlier, in
 * the PSRAM bring-up (octal vs quad); this catches the rest before init() drives pins
 * wired to something else on this hardware. */
static bool fits_hardware(void)
{
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    int flash_mb = (int)((flash + (1u << 19)) >> 20);                     /* nearest MB */
    int psram_mb = esp_psram_is_initialized() ? (int)((esp_psram_get_size() + (1u << 19)) >> 20) : 0;
    if ((!B.flash_mb || flash_mb == B.flash_mb) && psram_mb == B.psram_mb) return true;
    snprintf(s_mismatch, sizeof(s_mismatch), "built for %s: %d MB PSRAM / %d MB flash expected, %d / %d found",
             B.id, B.psram_mb, B.flash_mb, psram_mb, flash_mb);
    return false;
}

uint32_t molty_board_init(void)
{
    if (!fits_hardware()) {
        ESP_LOGE(TAG, "wrong board: this image is %s — hardware left alone, nothing will show. "
                 "Reflash the right firmware (molty.uno/setup, or esptool: the ROM bootloader always answers).",
                 s_mismatch);
        s_caps = 0;
        return 0;
    }
    uint32_t want = caps_with_ops();
    uint32_t up = B.init ? B.init() : 0;
    s_caps = up & want;
    char list[96] = "";
    for (int i = 0; i < MOLTY_CAP_COUNT; i++) {
        if (!(want & (1u << i))) continue;
        size_t n = strlen(list);
        snprintf(list + n, sizeof(list) - n, "%s%s%s", n ? " " : "", s_caps & (1u << i) ? "" : "!", CAP_NAMES[i]);
    }
    ESP_LOGI(TAG, "%s (%dx%d %s): %s", B.name, B.width, B.height,
             B.shape == MOLTY_SHAPE_ROUND ? "round" : "rect", list);
    if (want & ~s_caps) ESP_LOGW(TAG, "capabilities marked ! failed to start and are reported absent");
    return s_caps;
}

const char   *molty_board_id(void)        { return B.id; }
const char   *molty_board_name(void)      { return B.name; }
uint32_t      molty_board_caps(void)      { return s_caps; }
bool          molty_board_has(uint32_t c) { return c && (s_caps & c) == c; }
bool          molty_board_declares(uint32_t c) { return c && (B.caps & c) == c; }
int           molty_display_width(void)   { return B.width; }
int           molty_display_height(void)  { return B.height; }
molty_shape_t molty_display_shape(void)   { return B.shape; }

/* ── display ──────────────────────────────────────────────────────────────── */

uint16_t *molty_display_framebuffer(void)
{
    return (s_caps & MOLTY_CAP_DISPLAY) ? B.framebuffer() : NULL;
}

esp_err_t molty_display_flush(void)
{
    return (s_caps & MOLTY_CAP_DISPLAY) ? B.flush() : ESP_ERR_INVALID_STATE;
}

esp_err_t molty_display_flush_rows(int y0, int y1)
{
    return (s_caps & MOLTY_CAP_DISPLAY) ? B.flush_rows(y0, y1) : ESP_ERR_INVALID_STATE;
}

/* ── touch, IMU, buttons, battery ─────────────────────────────────────────── */

int molty_touch_poll(uint16_t *xs, uint16_t *ys, int max)
{
    if (!(s_caps & MOLTY_CAP_TOUCH)) return 0;
    if (max > MOLTY_TOUCH_MAX_POINTS) max = MOLTY_TOUCH_MAX_POINTS;
    int n = B.touch_poll(xs, ys, max);
    link_pair_touch(n);   /* the one touch poller also drives the pairing tap */
    sense_touch(n, xs, ys);   /* ...and the agent's tap events */
    return n;
}

bool molty_imu_get(molty_imu_sample_t *out)
{
    if (!(s_caps & MOLTY_CAP_IMU)) { memset(out, 0, sizeof(*out)); return false; }
    B.imu_get(out);
    return true;
}

bool molty_button_pressed(molty_button_t b)
{
    uint32_t need = b == MOLTY_BTN_ACCEPT ? MOLTY_CAP_BTN_ACCEPT : MOLTY_CAP_BTN_REJECT;
    return (s_caps & need) && B.button_pressed(b);
}

esp_err_t molty_battery_mv(int *mv)
{
    if (!(s_caps & MOLTY_CAP_BATTERY)) return ESP_ERR_NOT_SUPPORTED;
    int v = B.battery_mv();
    if (v < 0) return ESP_FAIL;
    *mv = v;
    return ESP_OK;
}

/* ── speaker ──────────────────────────────────────────────────────────────── */

#define SPEAKER (s_caps & MOLTY_CAP_SPEAKER)

esp_err_t molty_speaker_play(const int16_t *samples, size_t n, uint32_t rate_hz, int16_t amp_scale)
{
    return SPEAKER ? B.speaker->play_raw(samples, n, rate_hz, amp_scale) : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t molty_speaker_set_volume(uint8_t vol)
{
    if (!SPEAKER) return ESP_ERR_NOT_SUPPORTED;
    B.speaker->set_volume(vol);
    return ESP_OK;
}

esp_err_t molty_speaker_stream_begin(uint32_t rate_hz)
{
    return SPEAKER ? B.speaker->stream_begin(rate_hz) : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t molty_speaker_stream_write(const int16_t *samples, size_t n)
{
    return SPEAKER ? B.speaker->stream_write(samples, n) : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t molty_speaker_stream_drain(void)
{
    return SPEAKER ? B.speaker->stream_drain() : ESP_ERR_NOT_SUPPORTED;
}

void molty_speaker_stream_end(void)
{
    if (SPEAKER) B.speaker->stream_end();
}

/* ── microphone ───────────────────────────────────────────────────────────── */

#define MIC (s_caps & MOLTY_CAP_MIC)

esp_err_t molty_mic_record(uint32_t rate_hz, uint32_t ms, int16_t **out, size_t *n)
{
    return MIC ? B.mic->record_raw(rate_hz, ms, out, n) : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t molty_mic_stream_start(uint32_t rate_hz, uint32_t chunk_ms, uint8_t gain)
{
    return MIC ? B.mic->stream_start(rate_hz, chunk_ms, gain) : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t molty_mic_stream_read(int16_t *buf, size_t chunk_samples, size_t *got)
{
    return MIC ? B.mic->stream_read(buf, chunk_samples, got) : ESP_ERR_NOT_SUPPORTED;
}

void molty_mic_stream_stop(void)
{
    if (MIC) B.mic->stream_stop();
}

bool molty_mic_stream_active(void)
{
    return MIC && B.mic->stream_active();
}
