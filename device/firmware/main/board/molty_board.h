/*
 * SPDX-License-Identifier: MIT
 *
 * molty_board.h — the one seam between molty.UNO and the hardware it runs on.
 *
 * The core (fluid, creature, link, net, diag) knows no pins and no panel sizes. A
 * board is a folder under device/firmware/boards/<vendor>/<model>/ whose board.c defines
 *
 *     const molty_board_t molty_board_desc = { ... };
 *
 * and exactly one board is compiled in (Kconfig MOLTY_BOARD, see docs/PORTING.md).
 *
 *   display   required: a w×h RGB565 framebuffer (byte-swapped, panel wire order)
 *             plus full / row-band flush. Round or rectangular; the fluid draws in
 *             its largest centred square. Optional blit: the fluid then sends only the
 *             windows that changed, from a small internal-RAM buffer, and the
 *             framebuffer (PSRAM) stays out of the frame loop.
 *             Optional display_power: the panel off (and back on) while the screen is
 *             off (mole_fluid_screen); without it the fluid only stops drawing.
 *             Optional display_level: the panel's brightness (the goodbye fades it).
 *   power_off optional: the board cuts its own power (the PWR long press, power.c);
 *             without it, or while USB keeps it up, the screen only goes off.
 *   touch, imu, mic, speaker, accept / reject buttons, battery, usb_net, serial
 *             optional: leave the op NULL and drop the MOLTY_CAP_ bit. The core asks
 *             molty_board_has() and degrades — no mic/speaker: voice endpoints and
 *             the link's say/listen answer "unsupported"; no touch: pairing is
 *             accepted on the accept button; no IMU: no tilt, IMU endpoints
 *             answer "unsupported"; no usb_net: Wi-Fi only, set up over the cable
 *             with Improv when the board has serial.
 *
 * init() brings the hardware up in whatever order the board needs (shared buses,
 * expanders, resets) and returns the caps that actually came up: a capability the
 * board declares but fails to start is reported absent, never half-working.
 *
 * The wrappers below are what the core calls. Each one is safe on any board: an
 * absent capability returns ESP_ERR_NOT_SUPPORTED / false / 0, never crashes.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── capabilities ─────────────────────────────────────────────────────────── */

#define MOLTY_CAP_DISPLAY     (1u << 0)
#define MOLTY_CAP_TOUCH       (1u << 1)
#define MOLTY_CAP_IMU         (1u << 2)
#define MOLTY_CAP_MIC         (1u << 3)
#define MOLTY_CAP_SPEAKER     (1u << 4)
#define MOLTY_CAP_BTN_ACCEPT  (1u << 5)   /* pairing: accept (BOOT on the Waveshare boards) */
#define MOLTY_CAP_BTN_REJECT  (1u << 6)   /* pairing: reject (PWR on the 1.46)              */
#define MOLTY_CAP_BATTERY     (1u << 7)
#define MOLTY_CAP_USB_NET     (1u << 8)   /* native USB on the USB-C port: NCM at 10.13.37.1 */
#define MOLTY_CAP_SERIAL      (1u << 9)   /* the console UART on the USB-C port (USB-UART bridge):
                                           * serial logs + Wi-Fi setup over Improv (molty.uno/setup) */
#define MOLTY_CAP_COUNT       10

/** Name of capability bit i ("display", "touch", ... "serial"), as /api/info lists them. */
const char *molty_cap_name(int i);

typedef enum { MOLTY_SHAPE_RECT = 0, MOLTY_SHAPE_ROUND = 1 } molty_shape_t;
typedef enum { MOLTY_BTN_ACCEPT = 0, MOLTY_BTN_REJECT = 1 } molty_button_t;

#define MOLTY_TOUCH_MAX_POINTS 5

/* IMU sample in the device frame: x to the right and y up as the screen is read (the
 * fluid and the dashboard map tilt as dx = +ax, dy = -ay). A board remaps its chip's
 * axes to this frame. */
typedef struct {
    float ax, ay, az;   /* g   */
    float gx, gy, gz;   /* dps */
    uint32_t ms;        /* esp_timer uptime at capture */
} molty_imu_sample_t;

/* Battery: what the board can tell. pct and the flags are -1 / false where it cannot. */
typedef struct {
    int  mv;            /* cell voltage                                            */
    int  pct;           /* charge 0..100 from the board's fuel gauge, -1 = none    */
    bool charging;      /* the charger is filling the cell                         */
    bool usb;           /* external power present                                  */
} molty_battery_t;

/* Speaker: blocking clip playback + one exclusive PCM16 mono stream (the link's say). */
typedef struct {
    esp_err_t (*play_raw)(const int16_t *samples, size_t n, uint32_t rate_hz, int16_t amp_scale);
    void      (*set_volume)(uint8_t vol);                 /* 0..21 */
    esp_err_t (*stream_begin)(uint32_t rate_hz);
    esp_err_t (*stream_write)(const int16_t *samples, size_t n);
    esp_err_t (*stream_drain)(void);                      /* until queued PCM has played */
    void      (*stream_end)(void);
    uint32_t  (*stream_played)(void);                     /* optional: samples played since begin */
} molty_speaker_ops_t;

/* Microphone: blocking clip capture + one exclusive chunked stream, PCM16 mono. */
typedef struct {
    esp_err_t (*record_raw)(uint32_t rate_hz, uint32_t ms, int16_t **out, size_t *n);  /* caller free()s */
    esp_err_t (*stream_start)(uint32_t rate_hz, uint32_t chunk_ms, uint8_t gain);
    esp_err_t (*stream_read)(int16_t *buf, size_t chunk_samples, size_t *got);
    void      (*stream_stop)(void);
    bool      (*stream_active)(void);
} molty_mic_ops_t;

/* the panel's tearing-effect pulses (molty_display_te) */
typedef struct { bool line; uint32_t total, n, mean_us, min_us, max_us; } molty_te_t;

typedef struct {
    const char   *id;          /* board.json "name", e.g. "waveshare-esp32-s3-touch-lcd-1.46" */
    const char   *name;        /* human-readable                                         */
    int           width, height;
    molty_shape_t shape;
    uint32_t      caps;        /* MOLTY_CAP_* the board has (init may bring up fewer)    */
    int           psram_mb;    /* the hardware this image is for, checked at boot before    */
    int           flash_mb;    /* init() touches a pin (= board.json detect)                */

    void      (*early_init)(void);                /* optional: first thing at boot (power latch) */
    uint32_t  (*init)(void);                      /* bring up; returns the caps that came up    */

    /* display (required) */
    uint16_t *(*framebuffer)(void);
    esp_err_t (*flush)(void);
    esp_err_t (*flush_rows)(int y0, int y1);      /* full-width rows [y0, y1)                    */
    esp_err_t (*blit)(int x, int y, int w, int h, const uint16_t *px);   /* optional: see molty_display_blit */
    esp_err_t (*blit_async)(int x, int y, int w, int h, const uint16_t *px);   /* optional: see molty_display_blit_async */
    esp_err_t (*blit_wait)(void);                 /* optional, with blit_async                                    */
    void      (*blit_prof)(uint32_t *out);        /* optional: us in the async blits, see molty_display_blit_prof */
    void      (*blit_mirror)(bool on);            /* optional: see molty_display_blit_mirror                        */
    void      (*display_te)(molty_te_t *out, bool window);   /* optional: see molty_display_te                       */
    void      (*display_te_on)(bool on);          /* optional, EXPERIMENT: see molty_display_te_on                  */
    void      (*display_where)(uint32_t out[3]);  /* optional: see molty_display_where                              */
    void      (*display_inject)(void);            /* optional, EXPERIMENT: see molty_display_inject                 */
    esp_err_t (*display_power)(bool on);          /* optional: panel off / on (sleep, backlight)  */
    esp_err_t (*display_level)(uint8_t level);    /* optional: brightness 0..255 (255 at power on) */
    esp_err_t (*power_off)(void);                 /* optional: cut the power; returns only on failure */

    /* optional — NULL when absent */
    int       (*touch_poll)(uint16_t *xs, uint16_t *ys, int max);   /* panel px; returns count  */
    void      (*imu_get)(molty_imu_sample_t *out);                  /* latest cached sample     */
    const molty_speaker_ops_t *speaker;
    const molty_mic_ops_t     *mic;
    uint8_t   mic_gain;    /* the listen's mic stream gain (1..32); 0 = 8. A board sets it for its
                            * mic's sensitivity: the louder the mic, the lower (clipping is lost
                            * audio; the VAD follows its noise floor, so it needs no retune) */
    bool      (*button_pressed)(molty_button_t b);
    esp_err_t (*battery)(molty_battery_t *out);                     /* fails on a failed read   */
} molty_board_t;

/** Defined by the selected board's board.c. */
extern const molty_board_t molty_board_desc;

/* ── lifecycle (app_main) ─────────────────────────────────────────────────── */

void      molty_board_early_init(void);
/** Bring the board up; logs and returns the caps that came up. When the chip's PSRAM or
 *  flash size differs from the board's (an image for another board), no pin is touched:
 *  every capability is absent and molty_board_mismatch() says why. */
uint32_t  molty_board_init(void);
/** NULL, or why this image does not fit the hardware (e.g. "built for ... : 8 MB PSRAM
 *  expected, 2 MB found"). Reported by /api/info as board_mismatch. */
const char *molty_board_mismatch(void);

/* ── queries ──────────────────────────────────────────────────────────────── */

const char   *molty_board_id(void);
const char   *molty_board_name(void);
uint32_t      molty_board_caps(void);
/** What the board declares (before init), e.g. for decisions made before it is up. */
bool          molty_board_declares(uint32_t caps);
/** True when every bit of `caps` came up. */
bool          molty_board_has(uint32_t caps);

int           molty_display_width(void);
int           molty_display_height(void);
molty_shape_t molty_display_shape(void);

/* ── image tag: which board an app image is for ───────────────────────────── */

/* Every image carries its board folder (CONFIG_MOLTY_BOARD_DIR) in the custom app
 * descriptor, right after esp_app_desc_t: image offset MOLTY_IMAGE_TAG_OFFSET. */
#define MOLTY_IMAGE_TAG_OFFSET  (0x20 + 256)
#define MOLTY_IMAGE_TAG_BYTES   64
#define MOLTY_IMAGE_TAG_MAGIC   "MOLTYBRD"
/* Images from before the board layer carry no tag; they were all built for this. */
#define MOLTY_LEGACY_BOARD_DIR  "waveshare/esp32-s3-touch-lcd-1.46"

/** This image's board folder, e.g. "waveshare/esp32-s3-touch-lcd-1.46". */
const char *molty_board_dir(void);
/** Check the first `len` bytes of an app image (OTA upload) against this board.
 *  1: built for this board; 0: built for another (its folder in `other`); -1: not enough
 *  bytes yet, or not an app image (esp_ota_end judges those). */
int molty_board_image_check(const uint8_t *img, size_t len, char *other, size_t other_len);

/* ── the core's calls (each degrades on an absent capability) ─────────────── */

uint16_t *molty_display_framebuffer(void);           /* NULL when the display is down */
esp_err_t molty_display_flush(void);
esp_err_t molty_display_flush_rows(int y0, int y1);
/** Send w×h px (packed rows, wire order) to the panel at (x, y); x and w are multiples of 4
 *  (or w reaches the right edge). Returns once px may be reused. The board's blit leaves
 *  the framebuffer as it was (whoever draws keeps it for /api/framebuffer); without one,
 *  px is copied into the framebuffer and its rows flushed. */
esp_err_t molty_display_blit(int x, int y, int w, int h, const uint16_t *px);
/** As molty_display_blit, but it may return while the panel still reads px: px stays untouched until the next
 *  molty_display_blit_async or molty_display_blit_wait returns (each waits for the transfer before). Boards without
 *  one blit synchronously. molty_display_blit_can_overlap: whether it truly returns early. */
esp_err_t molty_display_blit_async(int x, int y, int w, int h, const uint16_t *px);
esp_err_t molty_display_blit_wait(void);
bool      molty_display_blit_can_overlap(void);
/** us spent in the async blits since the last call, added into out[3] (the mirror copy, waiting, commands); 0s without. */
void      molty_display_blit_prof(uint32_t *out);
/** Off: blits on whole row pairs may leave the framebuffer as it was (a board keeping a mirror of the panel for odd
 *  edges skips copying into it); the caller repaints in full before it sends windows with odd edges again. */
void      molty_display_blit_mirror(bool on);
/** The panel's tearing-effect pulses (one a refresh of its glass): total since boot and, with window, those since the
 *  last windowed call with their period (us; n 0: none came; the window starts again). line false: the board reads no TE
 *  line (none, or its interrupt could not be installed). Any task. */
void      molty_display_te(molty_te_t *out, bool window);
/** EXPERIMENT (the bench's te=0): the TE line's interrupt off, or on again. */
void      molty_display_te_on(bool on);
/** Where the panel's driver is (the stuck sender's log line): out[0] where the board's driver waits (its own stage, 0
 *  none), out[1] transfers queued, out[2] their ends taken, out[3] async blits started, out[4] collected (zeros without). */
void      molty_display_where(uint32_t out[5]);
/** EXPERIMENT (the bench's stuck=1): the board's next panel call meets a transfer that never ends, so its recovery shows. */
void      molty_display_inject(void);
/** The panel off (false) or on. ESP_ERR_NOT_SUPPORTED when the board cannot; the caller
 *  stops drawing either way. */
esp_err_t molty_display_power(bool on);
/** The panel's brightness, 0..255. ESP_ERR_NOT_SUPPORTED when the board cannot. */
esp_err_t molty_display_level(uint8_t level);
/** Cut the power. Returns only when it could not (ESP_ERR_NOT_SUPPORTED without the op). */
esp_err_t molty_power_off(void);

/** Poll touch; feeds the pairing tap detector. 0 points without touch. */
int       molty_touch_poll(uint16_t *xs, uint16_t *ys, int max);
/** Latest IMU sample; false (and *out zeroed) without an IMU. */
bool      molty_imu_get(molty_imu_sample_t *out);
bool      molty_button_pressed(molty_button_t b);    /* false without that button */
esp_err_t molty_battery_read(molty_battery_t *out);

esp_err_t molty_speaker_play(const int16_t *samples, size_t n, uint32_t rate_hz, int16_t amp_scale);
esp_err_t molty_speaker_set_volume(uint8_t vol);
esp_err_t molty_speaker_stream_begin(uint32_t rate_hz);
esp_err_t molty_speaker_stream_write(const int16_t *samples, size_t n);
esp_err_t molty_speaker_stream_drain(void);
void      molty_speaker_stream_end(void);
/** Samples the speaker has played since stream_begin (the say's playback position);
 *  0 without a speaker or when the board cannot tell. */
uint32_t  molty_speaker_stream_played(void);

esp_err_t molty_mic_record(uint32_t rate_hz, uint32_t ms, int16_t **out, size_t *n);
esp_err_t molty_mic_stream_start(uint32_t rate_hz, uint32_t chunk_ms, uint8_t gain);
/** The board's listen gain (molty_board_t.mic_gain; 8 when it sets none). */
uint8_t   molty_mic_gain(void);
esp_err_t molty_mic_stream_read(int16_t *buf, size_t chunk_samples, size_t *got);
void      molty_mic_stream_stop(void);
bool      molty_mic_stream_active(void);

#ifdef __cplusplus
}
#endif
