/*
 * SPDX-License-Identifier: MIT
 *
 * touch_cst9217.c: CST9217 capacitive touch (Waveshare AMOLED-1.75C), IDF i2c_master.
 *
 * Protocol from SensorLib's TouchDrvCST92xx (the Arduino demos) and Waveshare's
 * esp_lcd_touch_cst9217 (the BSP): 16-bit register addresses. After reset, 0xD101
 * enters command mode and 0xD1FC reads a checkcode whose top half is 0xCACA. A poll
 * reads 10 bytes at 0xD000: [0] id<<4 | event (6 = pressed), [1..3] X/Y 12 bit each,
 * [5] finger count, [6] 0xAB when the report is valid; writing 0xAB back acks it.
 * The panel's touch axes are mirrored in both against the display (the BSP's
 * mirror_x/mirror_y, the demo's setMirrorXY(true, true)). Polled; INT unused.
 */
#include "touch_cst9217.h"
#include "board.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "cst9217";

#define REG_POINTS      0xD000
#define REG_CMD_MODE    0xD101
#define REG_CHECKCODE   0xD1FC
#define ACK             0xAB
#define TP_TIMEOUT_MS   20

static i2c_master_dev_handle_t s_dev = NULL;
static i2c_master_bus_handle_t s_bus = NULL;
static int s_fail = 0;

static esp_err_t rd(uint16_t r, uint8_t *out, size_t n)
{
    const uint8_t a[2] = { (uint8_t)(r >> 8), (uint8_t)r };
    return i2c_master_transmit_receive(s_dev, a, 2, out, n, TP_TIMEOUT_MS);
}

static esp_err_t wr(const uint8_t *b, size_t n) { return i2c_master_transmit(s_dev, b, n, TP_TIMEOUT_MS); }

/* low 10 ms, then 50 ms to leave its boot code (BSP cst9217_reset) */
static void reset_pulse(void)
{
    gpio_set_level(TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

esp_err_t cst9217_init(i2c_master_bus_handle_t bus)
{
    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << TOUCH_RST, .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst), TAG, "rst gpio");
    reset_pulse();

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = TOUCH_ADDR,
        .scl_speed_hz    = 400000,
    };
    s_bus = bus;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add touch dev");

    uint8_t cc[4] = { 0 };
    esp_err_t e = wr((const uint8_t[]){ REG_CMD_MODE >> 8, REG_CMD_MODE & 0xFF }, 2);
    if (e == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
        e = rd(REG_CHECKCODE, cc, sizeof(cc));
    }
    if (e == ESP_OK && (cc[3] != 0xCA || cc[2] != 0xCA)) e = ESP_ERR_INVALID_RESPONSE;
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "no CST9217 at 0x%02x (%s, checkcode %02x%02x%02x%02x)", TOUCH_ADDR,
                 esp_err_to_name(e), cc[3], cc[2], cc[1], cc[0]);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return e;
    }
    ESP_LOGI(TAG, "CST9217 ready (checkcode %02x%02x%02x%02x)", cc[3], cc[2], cc[1], cc[0]);
    return ESP_OK;
}

int cst9217_poll(uint16_t *xs, uint16_t *ys, int max)
{
    if (!s_dev || max < 1) return 0;
    uint8_t d[10];
    if (rd(REG_POINTS, d, sizeof(d)) != ESP_OK) {
        if (++s_fail >= 12) {                 /* wedged: reset bus + controller */
            if (s_bus) i2c_master_bus_reset(s_bus);
            reset_pulse();
            s_fail = 0;
            ESP_LOGW(TAG, "touch recovered (bus/controller reset)");
        }
        return 0;
    }
    s_fail = 0;
    wr((const uint8_t[]){ REG_POINTS >> 8, REG_POINTS & 0xFF, ACK }, 3);
    if (d[6] != ACK || (d[5] & 0x7F) == 0 || (d[0] & 0x0F) != 0x06) return 0;
    const int x = (d[1] << 4) | (d[3] >> 4), y = (d[2] << 4) | (d[3] & 0x0F);
    xs[0] = (uint16_t)(x < LCD_WIDTH ? LCD_WIDTH - 1 - x : 0);
    ys[0] = (uint16_t)(y < LCD_HEIGHT ? LCD_HEIGHT - 1 - y : 0);
    return 1;
}
