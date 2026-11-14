/*
 * SPDX-License-Identifier: MIT
 *
 * board.c: Waveshare ESP32-S3-Touch-AMOLED-1.75C.
 *
 *   display  CO5300 466×466 round AMOLED, quad SPI, PSRAM fb + blit   lcd_co5300.c
 *   touch    CST9217, I2C 0x5A, polled                                touch_cst9217.c
 *   imu      QMI8658, I2C 0x6B                                        boards/common/qmi8658.c
 *   speaker  ES8311 DAC + NS4150B amp ┐ one duplex I2S port            audio_es.c
 *   mic      ES7210 ADC, two mics     ┘
 *   buttons  BOOT = accept, PWR = reject (pairing)
 *   battery  AXP2101 PMU, I2C 0x34: its VBAT ADC
 *   usb_net  native USB on the USB-C port (TinyUSB NCM)
 *
 * No power latch: the AXP2101 keeps the board on until a long PWR press. The display
 * has its own SPI bus; everything else shares the I2C bus, PMU first (ALDO1 feeds the
 * codecs). Pins: board.h.
 */
#include "molty_board.h"
#include "board.h"
#include "lcd_co5300.h"
#include "touch_cst9217.h"
#include "audio_es.h"
#include "qmi8658.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "board-1.75c";

/* ── AXP2101 (register map: the vendor's 01_AXP2101 demo, axp2101_registers.h) ── */
#define AXP_STATUS1     0x00        /* bit 3: battery present                    */
#define AXP_IC_TYPE     0x03        /* 0x4A                                      */
#define AXP_ADC_CTRL    0x30        /* bit 0: VBAT channel                       */
#define AXP_VBAT_H      0x34        /* VBAT mV = (H & 0x1F) << 8 | L             */
#define AXP_LDO_ON0     0x90        /* bit 0: ALDO1                              */
#define AXP_ALDO1_V     0x92        /* bits 4..0: (mV - 500) / 100               */

static i2c_master_dev_handle_t s_pmu;

static esp_err_t pmu_rd(uint8_t r, uint8_t *v, size_t n) { return i2c_master_transmit_receive(s_pmu, &r, 1, v, n, 50); }
static esp_err_t pmu_wr(uint8_t r, uint8_t v) { return i2c_master_transmit(s_pmu, (uint8_t[]){ r, v }, 2, 50); }

/* Make sure ALDO1 (A3V3) is on at 3.3 V, as the vendor's AXP demo sets it, and the
 * battery ADC runs. Leaves every other rail as the PMU booted it. */
static esp_err_t pmu_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = PMU_ADDR, .scl_speed_hz = 400000 };
    esp_err_t e = i2c_master_bus_add_device(bus, &cfg, &s_pmu);
    uint8_t id = 0, ldo = 0, adc = 0, v = 0;
    if (e == ESP_OK) e = pmu_rd(AXP_IC_TYPE, &id, 1);
    if (e == ESP_OK && id != 0x4A) e = ESP_ERR_NOT_FOUND;
    if (e == ESP_OK) e = pmu_rd(AXP_LDO_ON0, &ldo, 1);
    if (e == ESP_OK && !(ldo & 0x01)) {
        e = pmu_rd(AXP_ALDO1_V, &v, 1);
        if (e == ESP_OK) e = pmu_wr(AXP_ALDO1_V, (v & 0xE0) | ((3300 - 500) / 100));
        if (e == ESP_OK) e = pmu_wr(AXP_LDO_ON0, ldo | 0x01);
        ESP_LOGI(TAG, "ALDO1 (codecs) was off: on at 3.3 V");
    }
    if (e == ESP_OK) e = pmu_rd(AXP_ADC_CTRL, &adc, 1);
    if (e == ESP_OK && !(adc & 0x01)) e = pmu_wr(AXP_ADC_CTRL, adc | 0x01);
    return e;
}

static int battery_mv(void)
{
    uint8_t st = 0, d[2];
    if (pmu_rd(AXP_STATUS1, &st, 1) != ESP_OK || !(st & 0x08)) return -1;   /* no battery */
    if (pmu_rd(AXP_VBAT_H, d, 2) != ESP_OK) return -1;
    return ((d[0] & 0x1F) << 8) | d[1];
}

static uint32_t init(void)
{
    uint32_t up = MOLTY_CAP_USB_NET;

    if (lcd_co5300_init() == ESP_OK) up |= MOLTY_CAP_DISPLAY;
    else ESP_LOGW(TAG, "LCD init failed: creature rendering will be dark");

    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << BOARD_BTN_BOOT) | (1ULL << BOARD_BTN_PWR), .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .intr_type = GPIO_INTR_DISABLE,   /* both pulled up on the board */
    };
    if (gpio_config(&btn) == ESP_OK) up |= MOLTY_CAP_BTN_ACCEPT | MOLTY_CAP_BTN_REJECT;

    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = BOARD_I2C_PORT,
        .sda_io_num        = BOARD_I2C_SDA,
        .scl_io_num        = BOARD_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = true },
    };
    esp_err_t e = i2c_new_master_bus(&bus_cfg, &bus);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus failed (%s): no touch, IMU, audio or battery", esp_err_to_name(e));
        return up;
    }

    e = pmu_init(bus);
    if (e == ESP_OK) up |= MOLTY_CAP_BATTERY;
    else ESP_LOGW(TAG, "AXP2101 failed (%s): no battery reading", esp_err_to_name(e));

    if (cst9217_init(bus) == ESP_OK) up |= MOLTY_CAP_TOUCH;
    else ESP_LOGW(TAG, "touch init failed: taps disabled (pairing accepts on BOOT)");

    const qmi8658_config_t imu = { .bus = bus, .addr = BOARD_IMU_ADDR, .axes = BOARD_IMU_AXES };
    e = qmi8658_init(&imu);
    if (e == ESP_OK) up |= MOLTY_CAP_IMU;
    else ESP_LOGW(TAG, "IMU init failed (%s): no tilt", esp_err_to_name(e));

    return up | es_audio_init(bus);
}

static bool button_pressed(molty_button_t b)
{
    return b == MOLTY_BTN_ACCEPT ? gpio_get_level(BOARD_BTN_BOOT) == 0 : gpio_get_level(BOARD_BTN_PWR) == 1;
}

static const molty_speaker_ops_t s_speaker = {
    .play_raw      = es_audio_play_raw,
    .set_volume    = es_audio_set_volume,
    .stream_begin  = es_audio_tx_stream_begin,
    .stream_write  = es_audio_tx_stream_write,
    .stream_drain  = es_audio_tx_stream_drain,
    .stream_end    = es_audio_tx_stream_end,
    .stream_played = es_audio_tx_stream_played,
};

static const molty_mic_ops_t s_mic = {
    .record_raw    = es_audio_record_raw,
    .stream_start  = es_audio_rx_start_stream,
    .stream_read   = es_audio_rx_read_chunk,
    .stream_stop   = es_audio_rx_stop_stream,
    .stream_active = es_audio_rx_stream_active,
};

const molty_board_t molty_board_desc = {
    .id     = "waveshare-esp32-s3-touch-amoled-1.75c",
    .name   = "Waveshare ESP32-S3-Touch-AMOLED-1.75C",
    .width  = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .shape  = MOLTY_SHAPE_ROUND,
    .psram_mb = 8,
    .flash_mb = 32,
    .caps   = MOLTY_CAP_DISPLAY | MOLTY_CAP_TOUCH | MOLTY_CAP_IMU | MOLTY_CAP_MIC | MOLTY_CAP_SPEAKER |
              MOLTY_CAP_BTN_ACCEPT | MOLTY_CAP_BTN_REJECT | MOLTY_CAP_BATTERY | MOLTY_CAP_USB_NET,

    .init        = init,
    .framebuffer = lcd_co5300_framebuffer,
    .flush       = lcd_co5300_flush,
    .flush_rows  = lcd_co5300_flush_rows,
    .blit        = lcd_co5300_blit,

    .touch_poll     = cst9217_poll,
    .imu_get        = qmi8658_get,
    .speaker        = &s_speaker,
    .mic            = &s_mic,
    /* ES7210 at 24 dB PGA, two mics averaged. TODO(hardware): tune from a real turn's
     * "pk"/"cl" in the turn trace (docs/AGENT-LINK.md); the 1.46's MSM mic needed 6. */
    .mic_gain       = 4,
    .button_pressed = button_pressed,
    .battery_mv     = battery_mv,
};
