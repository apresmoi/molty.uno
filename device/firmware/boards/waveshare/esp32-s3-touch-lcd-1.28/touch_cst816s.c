/*
 * SPDX-License-Identifier: MIT
 *
 * touch_cst816s.c — CST816S capacitive touch (Waveshare 1.28), IDF i2c_master API.
 *
 * Register layout from the demo's CST816S.cpp (Felix Biego, MIT): a 6-byte read
 * from 0x01 = gesture, finger count, event|X[11:8], X[7:0], Y[11:8], Y[7:0].
 * Unlike the demo we poll instead of waiting for INT, so the chip's auto-sleep
 * (it stops answering I2C a few seconds after the last touch) is switched off
 * (0xFE DisAutoSleep). Repeated read failures pulse the reset line and re-arm.
 */
#include "touch_cst816s.h"
#include "board.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "cst816s";

#define REG_GESTURE      0x01
#define REG_CHIP_ID      0xA7
#define REG_DIS_AUTOSLEEP 0xFE
#define TP_TIMEOUT_MS    20

static i2c_master_dev_handle_t s_dev = NULL;
static i2c_master_bus_handle_t s_bus = NULL;
static int s_fail = 0;

static esp_err_t rd(uint8_t reg, uint8_t *out, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, n, TP_TIMEOUT_MS);
}

static esp_err_t wr(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = { reg, v };
    return i2c_master_transmit(s_dev, b, 2, TP_TIMEOUT_MS);
}

/* reset pulse as the demo's begin(): high, low 5 ms, high, 50 ms to boot */
static void reset_pulse(void)
{
    gpio_set_level(TOUCH_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(TOUCH_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

esp_err_t cst816s_init(i2c_master_bus_handle_t bus)
{
    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << TOUCH_PIN_RST, .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst), TAG, "rst gpio");
    gpio_set_level(TOUCH_PIN_RST, 1);
    reset_pulse();

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = TOUCH_ADDR,
        .scl_speed_hz    = 400000,
    };
    s_bus = bus;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add touch dev");

    uint8_t id[3] = { 0 };
    esp_err_t e = rd(REG_CHIP_ID, id, sizeof(id));
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "no answer at 0x%02x (%s)", TOUCH_ADDR, esp_err_to_name(e));
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return e;
    }
    ESP_RETURN_ON_ERROR(wr(REG_DIS_AUTOSLEEP, 0x01), TAG, "disable auto-sleep");
    ESP_LOGI(TAG, "CST816S ready (chip 0x%02x, fw %02x.%02x)", id[0], id[1], id[2]);
    return ESP_OK;
}

int cst816s_poll(uint16_t *xs, uint16_t *ys, int max)
{
    if (!s_dev || max < 1) return 0;
    uint8_t d[6];
    if (rd(REG_GESTURE, d, sizeof(d)) != ESP_OK) {
        if (++s_fail >= 12) {                 /* wedged: reset bus + controller, re-arm */
            if (s_bus) i2c_master_bus_reset(s_bus);
            reset_pulse();
            wr(REG_DIS_AUTOSLEEP, 0x01);
            s_fail = 0;
            ESP_LOGW(TAG, "touch recovered (bus/controller reset)");
        }
        return 0;
    }
    s_fail = 0;
    if ((d[1] & 0x0F) == 0) return 0;
    uint16_t x = (uint16_t)(((d[2] & 0x0F) << 8) | d[3]), y = (uint16_t)(((d[4] & 0x0F) << 8) | d[5]);
    xs[0] = x < LCD_WIDTH ? x : LCD_WIDTH - 1;
    ys[0] = y < LCD_HEIGHT ? y : LCD_HEIGHT - 1;
    return 1;
}
