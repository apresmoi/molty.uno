/*
 * SPDX-License-Identifier: MIT
 *
 * lcd_gc9a01.c — GC9A01A 240×240 round panel over 4-wire SPI (Waveshare 1.28).
 *
 * A minimal driver on IDF's esp_lcd panel IO: hardware reset, Waveshare's register
 * sequence (LCD_1in28.cpp LCD_1IN28_InitReg, which ends MADCTL = 0x08 — BGR, no
 * mirroring — and INVON, as the panel needs), then window + RAMWR flushes from the
 * PSRAM framebuffer. No esp_lcd_panel_t: nothing here needs mirroring or gaps.
 * Pins: board.h.
 */
#include "lcd_gc9a01.h"
#include "board.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "lcd_gc9a01";

/* Largest single SPI transfer. The framebuffer is PSRAM, so spi_master bounces each
 * transfer through internal DMA memory: keep it small and the queue shallow (the
 * 1.46's lesson: deep queues of big bounces starved Wi-Fi and TLS of internal RAM). */
#define LCD_MAX_XFER     4096

#define CMD_CASET  0x2A
#define CMD_RASET  0x2B
#define CMD_RAMWR  0x2C

typedef struct { uint8_t cmd, len; uint8_t data[12]; uint8_t delay_ms; } init_cmd_t;

/* LCD_1IN28_InitReg, verbatim order and values (inter-register enables, power,
 * gamma, frame rate), from Waveshare's ESP32-S3-Touch-LCD-1.28-Test demo. */
static const init_cmd_t INIT[] = {
    { 0xEF, 0, {0}, 0 },
    { 0xEB, 1, {0x14}, 0 },
    { 0xFE, 0, {0}, 0 },
    { 0xEF, 0, {0}, 0 },
    { 0xEB, 1, {0x14}, 0 },
    { 0x84, 1, {0x40}, 0 },
    { 0x85, 1, {0xFF}, 0 },
    { 0x86, 1, {0xFF}, 0 },
    { 0x87, 1, {0xFF}, 0 },
    { 0x88, 1, {0x0A}, 0 },
    { 0x89, 1, {0x21}, 0 },
    { 0x8A, 1, {0x00}, 0 },
    { 0x8B, 1, {0x80}, 0 },
    { 0x8C, 1, {0x01}, 0 },
    { 0x8D, 1, {0x01}, 0 },
    { 0x8E, 1, {0xFF}, 0 },
    { 0x8F, 1, {0xFF}, 0 },
    { 0xB6, 2, {0x00, 0x20}, 0 },
    { 0x36, 1, {0x08}, 0 },                 /* MADCTL: BGR, no mirror (the demo's final value) */
    { 0x3A, 1, {0x05}, 0 },                 /* COLMOD: 16 bpp                                  */
    { 0x90, 4, {0x08, 0x08, 0x08, 0x08}, 0 },
    { 0xBD, 1, {0x06}, 0 },
    { 0xBC, 1, {0x00}, 0 },
    { 0xFF, 3, {0x60, 0x01, 0x04}, 0 },
    { 0xC3, 1, {0x13}, 0 },
    { 0xC4, 1, {0x13}, 0 },
    { 0xC9, 1, {0x22}, 0 },
    { 0xBE, 1, {0x11}, 0 },
    { 0xE1, 2, {0x10, 0x0E}, 0 },
    { 0xDF, 3, {0x21, 0x0C, 0x02}, 0 },
    { 0xF0, 6, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 0 },
    { 0xF1, 6, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 0 },
    { 0xF2, 6, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 0 },
    { 0xF3, 6, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 0 },
    { 0xED, 2, {0x1B, 0x0B}, 0 },
    { 0xAE, 1, {0x77}, 0 },
    { 0xCD, 1, {0x63}, 0 },
    { 0x70, 9, {0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03}, 0 },
    { 0xE8, 1, {0x34}, 0 },
    { 0x62, 12, {0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70}, 0 },
    { 0x63, 12, {0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70}, 0 },
    { 0x64, 7, {0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07}, 0 },
    { 0x66, 10, {0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00}, 0 },
    { 0x67, 10, {0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98}, 0 },
    { 0x74, 7, {0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00}, 0 },
    { 0x98, 2, {0x3E, 0x07}, 0 },
    { 0x35, 0, {0}, 0 },                    /* TEON                                            */
    { 0x21, 0, {0}, 0 },                    /* INVON: this IPS panel is inverted               */
    { 0x11, 0, {0}, 120 },                  /* SLPOUT                                          */
    { 0x29, 0, {0}, 20 },                   /* DISPON                                          */
};

static esp_lcd_panel_io_handle_t s_io = NULL;
static uint16_t *s_fb = NULL;

/* window = full-width rows [y0, y1), then stream them */
static esp_err_t push_rows(int y0, int y1)
{
    const uint8_t ca[4] = { 0, 0, (LCD_WIDTH - 1) >> 8, (LCD_WIDTH - 1) & 0xFF };
    const uint8_t ra[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)((y1 - 1) >> 8), (uint8_t)(y1 - 1) };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, CMD_CASET, ca, 4), TAG, "caset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, CMD_RASET, ra, 4), TAG, "raset");
    return esp_lcd_panel_io_tx_color(s_io, CMD_RAMWR, s_fb + (size_t)y0 * LCD_WIDTH,
                                     (size_t)(y1 - y0) * LCD_WIDTH * sizeof(uint16_t));
}

esp_err_t lcd_gc9a01_init(void)
{
    /* backlight off until the first black frame is on the glass */
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << LCD_PIN_BL) | (1ULL << LCD_PIN_RST),
        .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "gpio");
    gpio_set_level(LCD_PIN_BL, 0);

    spi_bus_config_t bus = {
        .sclk_io_num = LCD_PIN_SCLK, .mosi_io_num = LCD_PIN_MOSI, .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_MAX_XFER,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = LCD_PIN_CS,
        .dc_gpio_num       = LCD_PIN_DC,
        .spi_mode          = 0,
        .pclk_hz           = LCD_SPI_CLK_HZ,
        .trans_queue_depth = 2,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &s_io),
                        TAG, "panel io");

    /* hardware reset, as LCD_1IN28_Reset (high → low → high) */
    gpio_set_level(LCD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LCD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LCD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    for (size_t i = 0; i < sizeof(INIT) / sizeof(INIT[0]); i++) {
        const init_cmd_t *c = &INIT[i];
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, c->cmd, c->len ? c->data : NULL, c->len),
                            TAG, "init 0x%02x", c->cmd);
        if (c->delay_ms) vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
    }

    size_t fb_bytes = (size_t)LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t);
    s_fb = heap_caps_calloc(1, fb_bytes, MALLOC_CAP_SPIRAM);
    if (!s_fb) {
        ESP_LOGE(TAG, "no %u-byte PSRAM framebuffer", (unsigned)fb_bytes);
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(push_rows(0, LCD_HEIGHT), TAG, "initial black frame");
    gpio_set_level(LCD_PIN_BL, 1);
    ESP_LOGI(TAG, "GC9A01 ready — %dx%d, %d MHz SPI, backlight on", LCD_WIDTH, LCD_HEIGHT,
             LCD_SPI_CLK_HZ / 1000000);
    return ESP_OK;
}

uint16_t *lcd_gc9a01_framebuffer(void) { return s_fb; }

esp_err_t lcd_gc9a01_flush(void)
{
    if (!s_io || !s_fb) return ESP_ERR_INVALID_STATE;
    return push_rows(0, LCD_HEIGHT);
}

esp_err_t lcd_gc9a01_flush_rows(int y0, int y1)
{
    if (!s_io || !s_fb) return ESP_ERR_INVALID_STATE;
    if (y0 < 0) y0 = 0;
    if (y1 > LCD_HEIGHT) y1 = LCD_HEIGHT;
    if (y1 <= y0) return ESP_OK;
    return push_rows(y0, y1);
}
