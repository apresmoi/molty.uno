/*
 * SPDX-License-Identifier: MIT
 *
 * board.c — Waveshare ESP32-S3-Touch-LCD-1.28.
 *
 *   display  GC9A01A 240×240 round, SPI, PSRAM framebuffer       lcd_gc9a01.c
 *   touch    CST816S, I2C 0x15, polled                            touch_cst816s.c
 *   imu      QMI8658, I2C 0x6B (0x6A on some runs of the board)   boards/common/qmi8658.c
 *   buttons  BOOT = accept (no reject button: pairing rejects on timeout)
 *   battery  GPIO1 ADC through a 1:3 divider
 *   serial   the USB-C port is a CH343P USB-UART bridge on UART0: logs, and Wi-Fi
 *            setup from the browser over Improv (net/improv_serial.c, molty.uno/setup)
 *   absent   speaker, mic (voice is unsupported), USB-C network (Wi-Fi only)
 *
 * The display is on its own SPI bus, so it comes up even if the I2C side fails.
 * Pins: board.h.
 */
#include "molty_board.h"
#include "board.h"
#include "lcd_gc9a01.h"
#include "touch_cst816s.h"
#include "qmi8658.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG = "board-1.28";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static adc_channel_t             s_bat_ch;

static esp_err_t battery_init(void)
{
    adc_unit_t unit;
    esp_err_t e = adc_oneshot_io_to_channel(BOARD_BAT_ADC_GPIO, &unit, &s_bat_ch);
    if (e != ESP_OK) return e;
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
    if ((e = adc_oneshot_new_unit(&ucfg, &s_adc)) != ESP_OK) return e;
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if ((e = adc_oneshot_config_channel(s_adc, s_bat_ch, &ccfg)) != ESP_OK) return e;
    adc_cali_curve_fitting_config_t cal = {
        .unit_id = unit, .chan = s_bat_ch, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    return adc_cali_create_scheme_curve_fitting(&cal, &s_cali);
}

static int battery_mv(void)
{
    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_adc, s_bat_ch, &raw) != ESP_OK) return -1;
    if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return -1;
    return mv * BOARD_BAT_DIVIDER;
}

static uint32_t init(void)
{
    uint32_t up = MOLTY_CAP_SERIAL;   /* UART0 through the CH343P: logs + Improv Wi-Fi setup */

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BOARD_BTN_BOOT, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&btn) == ESP_OK) up |= MOLTY_CAP_BTN_ACCEPT;

    esp_err_t e = lcd_gc9a01_init();
    if (e == ESP_OK) up |= MOLTY_CAP_DISPLAY;
    else ESP_LOGW(TAG, "LCD init failed (%s) — creature rendering will be dark", esp_err_to_name(e));

    if (battery_init() == ESP_OK) up |= MOLTY_CAP_BATTERY;
    else ESP_LOGW(TAG, "battery ADC unavailable");

    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = BOARD_I2C_PORT,
        .sda_io_num        = BOARD_I2C_SDA,
        .scl_io_num        = BOARD_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = true },
    };
    e = i2c_new_master_bus(&bus_cfg, &bus);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus failed (%s) — no touch or IMU", esp_err_to_name(e));
        return up;
    }

    if (cst816s_init(bus) == ESP_OK) up |= MOLTY_CAP_TOUCH;
    else ESP_LOGW(TAG, "touch init failed — taps disabled (pairing accepts on BOOT)");

    /* the demo probes both QMI8658 addresses (SA0 high / low) */
    uint8_t addr = QMI8658_ADDR_SA0_HIGH;
    if (i2c_master_probe(bus, addr, 50) != ESP_OK) addr = QMI8658_ADDR_SA0_LOW;
    const qmi8658_config_t imu = { .bus = bus, .addr = addr, .axes = BOARD_IMU_AXES };
    e = qmi8658_init(&imu);
    if (e == ESP_OK) up |= MOLTY_CAP_IMU;
    else ESP_LOGW(TAG, "IMU init failed (%s) — no tilt", esp_err_to_name(e));
    return up;
}

static bool button_pressed(molty_button_t b)
{
    return b == MOLTY_BTN_ACCEPT && gpio_get_level(BOARD_BTN_BOOT) == 0;
}

const molty_board_t molty_board_desc = {
    .id     = "waveshare-esp32-s3-touch-lcd-1.28",
    .name   = "Waveshare ESP32-S3-Touch-LCD-1.28",
    .width  = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .shape  = MOLTY_SHAPE_ROUND,
    .psram_mb = 2,
    .flash_mb = 16,
    .caps   = MOLTY_CAP_DISPLAY | MOLTY_CAP_TOUCH | MOLTY_CAP_IMU | MOLTY_CAP_BTN_ACCEPT | MOLTY_CAP_BATTERY |
              MOLTY_CAP_SERIAL,

    .init        = init,
    .framebuffer = lcd_gc9a01_framebuffer,
    .flush       = lcd_gc9a01_flush,
    .flush_rows  = lcd_gc9a01_flush_rows,

    .touch_poll     = cst816s_poll,
    .imu_get        = qmi8658_get,
    .button_pressed = button_pressed,
    .battery_mv     = battery_mv,
};
