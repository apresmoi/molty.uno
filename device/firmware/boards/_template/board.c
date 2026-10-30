/*
 * SPDX-License-Identifier: MIT
 *
 * board.c — the template: copy this folder to device/firmware/boards/<vendor>/<model>/ and
 * fill it in (docs/PORTING.md walks through it). Folders starting with "_" are not
 * built; yours will be once it is in the Kconfig MOLTY_BOARD choice.
 *
 * The contract (main/board/molty_board.h):
 *   - display is required: a width×height RGB565 framebuffer, byte-swapped (the
 *     panel's wire order, high byte first in memory), plus a full flush and a
 *     full-width row-band flush. Put the framebuffer in PSRAM.
 *   - everything else is optional: leave the op NULL and the MOLTY_CAP_ bit out.
 *   - init() brings the hardware up in your board's order and returns the caps that
 *     actually came up. Log and continue on a failure; never abort the boot.
 *   - touch_poll() returns panel pixels in the framebuffer's orientation.
 *   - imu_get() returns the device frame: x right, y up as the screen is read.
 *     boards/common/qmi8658.c takes an axis map for that.
 *   - button_pressed(): true while held. Accept (and reject, if any) drive pairing.
 *   - USB-C through a USB-UART bridge to the console UART (CH340/CH343/CP210x)?
 *     Declare MOLTY_CAP_SERIAL (and return it from init): the core then offers Wi-Fi
 *     setup over the cable with Improv (net/improv_serial.c, molty.uno/setup). Native
 *     USB on USB-C instead: MOLTY_CAP_USB_NET, the console at 10.13.37.1.
 *
 * Reference ports: waveshare/esp32-s3-touch-lcd-1.46 (QSPI panel, IO expander,
 * audio, two buttons) and waveshare/esp32-s3-touch-lcd-1.28 (SPI panel, no audio,
 * one button, UART console).
 */
#include "molty_board.h"
#include "board.h"

#include <stdlib.h>
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "board-template";

static uint16_t *s_fb;

/* ── display: replace with your panel driver (see the 1.28's lcd_gc9a01.c) ──── */

static esp_err_t lcd_init(void)
{
    s_fb = heap_caps_calloc(1, (size_t)LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_fb) return ESP_ERR_NO_MEM;
    /* TODO: SPI/QSPI/RGB bus, panel IO, reset, the vendor init sequence, backlight */
    return ESP_ERR_NOT_SUPPORTED;
}

static uint16_t *lcd_framebuffer(void) { return s_fb; }

static esp_err_t lcd_flush_rows(int y0, int y1)
{
    /* TODO: window = full-width rows [y0, y1), then stream s_fb + y0 * LCD_WIDTH */
    (void)y0; (void)y1;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t lcd_flush(void) { return lcd_flush_rows(0, LCD_HEIGHT); }

/* ── bring-up ─────────────────────────────────────────────────────────────── */

static uint32_t init(void)
{
    uint32_t up = MOLTY_CAP_SERIAL;   /* drop if USB-C is not a UART bridge (see above) */

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BOARD_BTN_ACCEPT, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&btn) == ESP_OK) up |= MOLTY_CAP_BTN_ACCEPT;

    esp_err_t e = lcd_init();
    if (e == ESP_OK) up |= MOLTY_CAP_DISPLAY;
    else ESP_LOGW(TAG, "LCD init failed (%s)", esp_err_to_name(e));

    /* TODO: I2C bus (i2c_new_master_bus), then touch → MOLTY_CAP_TOUCH and the IMU →
     * MOLTY_CAP_IMU, each only when its init returned ESP_OK */
    return up;
}

static bool button_pressed(molty_button_t b)
{
    return b == MOLTY_BTN_ACCEPT && gpio_get_level(BOARD_BTN_ACCEPT) == 0;
}

const molty_board_t molty_board_desc = {
    .id     = "myvendor-mymodel",              /* = board.json "name"                  */
    .name   = "My Vendor My-Model",
    .width  = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .shape  = MOLTY_SHAPE_ROUND,               /* or MOLTY_SHAPE_RECT                  */
    .psram_mb = 2,                             /* = board.json detect: checked at boot */
    .flash_mb = 16,
    .caps   = MOLTY_CAP_DISPLAY | MOLTY_CAP_BTN_ACCEPT | MOLTY_CAP_SERIAL,

    .early_init  = NULL,                       /* e.g. a soft-power latch (the 1.46)   */
    .init        = init,
    .framebuffer = lcd_framebuffer,
    .flush       = lcd_flush,
    .flush_rows  = lcd_flush_rows,

    .touch_poll     = NULL,                    /* int  f(uint16_t *xs, uint16_t *ys, int max) */
    .imu_get        = NULL,                    /* void f(molty_imu_sample_t *out)      */
    .speaker        = NULL,                    /* const molty_speaker_ops_t *           */
    .mic            = NULL,                    /* const molty_mic_ops_t *               */
    .button_pressed = button_pressed,
    .battery_mv     = NULL,                    /* int  f(void), millivolts             */
};
