/*
 * SPDX-License-Identifier: MIT
 *
 * board.c — Waveshare ESP32-S3-Touch-LCD-1.46 (the reference board).
 *
 *   display  SPD2010 412×412 round, quad SPI, PSRAM framebuffer   mole_lcd.c
 *   touch    SPD2010, I2C 0x53, polled                             mole_touch.c
 *   imu      QMI8658, I2C 0x6B                                     boards/common/qmi8658.c
 *   speaker  PCM5101 DAC, I2S TX       ┐ lazily opened on first use mole_audio.c
 *   mic      MSM mic, I2S RX           ┘
 *   buttons  BOOT = accept, PWR = reject (pairing)
 *   usb_net  native USB on the USB-C port (TinyUSB NCM)
 *
 * One I2C bus carries the IMU, the TCA9554 expander (LCD + touch resets) and the
 * touch controller, so init order is: bus → IMU → LCD (expander) → touch. Pins: board.h.
 */
#include "molty_board.h"
#include "board.h"
#include "mole_lcd.h"
#include "mole_touch.h"
#include "mole_audio.h"
#include "qmi8658.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "board-1.46";

/* Battery power latch (soft-power button). On battery, the side button only closes
 * the rail momentarily; the firmware must drive the latch HIGH within ~100 ms or the
 * chip loses power when the button is released. On USB power this is a no-op.
 * Driving it LOW powers the device off. */
static void early_init(void)
{
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = (1ULL << BOARD_PWR_LATCH),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr_cfg);
    gpio_set_level(BOARD_PWR_LATCH, 1);
    ESP_LOGI(TAG, "battery power latched (GPIO %d = HIGH)", BOARD_PWR_LATCH);
}

static uint32_t init(void)
{
    /* I2S is opened on first use (mole_audio.c), as before the board layer */
    uint32_t up = MOLTY_CAP_SPEAKER | MOLTY_CAP_MIC | MOLTY_CAP_USB_NET;

    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << BOARD_BTN_BOOT) | (1ULL << BOARD_BTN_PWR), .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&btn) == ESP_OK) up |= MOLTY_CAP_BTN_ACCEPT | MOLTY_CAP_BTN_REJECT;

    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = BOARD_I2C_PORT,
        .sda_io_num        = BOARD_I2C_SDA,
        .scl_io_num        = BOARD_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority     = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    esp_err_t e = i2c_new_master_bus(&bus_cfg, &bus);
    if (e != ESP_OK) {
        /* the LCD reset sits on the expander: no bus, no display either */
        ESP_LOGE(TAG, "I2C bus failed (%s) — no display, touch or IMU", esp_err_to_name(e));
        return up;
    }

    const qmi8658_config_t imu = { .bus = bus, .addr = BOARD_IMU_ADDR, .axes = BOARD_IMU_AXES };
    e = qmi8658_init(&imu);
    if (e == ESP_OK) up |= MOLTY_CAP_IMU;
    else ESP_LOGW(TAG, "IMU init failed (%s) — no tilt", esp_err_to_name(e));

    e = mole_lcd_init(bus);
    if (e == ESP_OK) up |= MOLTY_CAP_DISPLAY;
    else ESP_LOGW(TAG, "LCD init failed (%s) — creature rendering will be dark", esp_err_to_name(e));

    if (mole_touch_init(bus) == ESP_OK) up |= MOLTY_CAP_TOUCH;
    else ESP_LOGW(TAG, "touch init failed — taps/gestures disabled");
    return up;
}

static bool button_pressed(molty_button_t b)
{
    return gpio_get_level(b == MOLTY_BTN_ACCEPT ? BOARD_BTN_BOOT : BOARD_BTN_PWR) == 0;
}

static const molty_speaker_ops_t s_speaker = {
    .play_raw     = mole_audio_play_raw,
    .set_volume   = mole_audio_set_volume,
    .stream_begin = mole_audio_tx_stream_begin,
    .stream_write = mole_audio_tx_stream_write,
    .stream_drain = mole_audio_tx_stream_drain,
    .stream_end   = mole_audio_tx_stream_end,
};

static const molty_mic_ops_t s_mic = {
    .record_raw    = mole_audio_record_raw,
    .stream_start  = mole_audio_rx_start_stream,
    .stream_read   = mole_audio_rx_read_chunk,
    .stream_stop   = mole_audio_rx_stop_stream,
    .stream_active = mole_audio_rx_stream_active,
};

const molty_board_t molty_board_desc = {
    .id     = "waveshare-esp32-s3-touch-lcd-1.46",
    .name   = "Waveshare ESP32-S3-Touch-LCD-1.46",
    .width  = MOLE_LCD_WIDTH,
    .height = MOLE_LCD_HEIGHT,
    .shape  = MOLTY_SHAPE_ROUND,
    .psram_mb = 8,
    .flash_mb = 16,
    .caps   = MOLTY_CAP_DISPLAY | MOLTY_CAP_TOUCH | MOLTY_CAP_IMU | MOLTY_CAP_MIC | MOLTY_CAP_SPEAKER |
              MOLTY_CAP_BTN_ACCEPT | MOLTY_CAP_BTN_REJECT | MOLTY_CAP_USB_NET,

    .early_init  = early_init,
    .init        = init,
    .framebuffer = mole_lcd_framebuffer,
    .flush       = mole_lcd_flush,
    .flush_rows  = mole_lcd_flush_rows,

    .touch_poll     = mole_touch_poll,
    .imu_get        = qmi8658_get,
    .speaker        = &s_speaker,
    .mic            = &s_mic,
    .button_pressed = button_pressed,
};
