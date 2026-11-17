/*
 * SPDX-License-Identifier: MIT
 *
 * lcd_co5300.c: CO5300 466×466 AMOLED over quad SPI (Waveshare AMOLED-1.75C).
 *
 * A minimal driver on IDF's esp_lcd panel IO, like the 1.28's: hardware reset, the
 * Waveshare BSP's register sequence (esp32_s3_touch_amoled_1_75c.c lcd_init_cmds, after
 * esp_lcd_co5300's MADCTL/COLMOD), then window + RAMWR writes. In QSPI every register
 * write is opcode 0x02 + the command in bits 15..8, and pixels go out with 0x32 on all
 * four lines. The glass starts at RAM column 6 (the BSP's set_gap). Brightness is the
 * panel's 0x51, held at 0 until the first black frame is on the glass.
 *
 * Windows must start and end on even rows and columns (the BSP's LVGL rounder rounds
 * x1/y1 down to even, x2/y2 up to odd). Columns come aligned (molty_display_blit: x
 * and w are multiples of 4, or w reaches the even right edge); rows do not, so the
 * framebuffer mirrors the panel and an odd edge row is sent from it with its pair.
 * Pins: board.h.
 */
#include "lcd_co5300.h"
#include "board.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "lcd_co5300";

#define LCD_SPI_HOST     SPI2_HOST
/* The vendor demos' 40 MHz (BSP and Arduino_ESP32QSPI). The 1.46's SPD2010 runs at 80:
 * worth trying here once the panel is known good. */
#define LCD_QSPI_HZ      (80 * 1000 * 1000)
/* Largest single SPI transfer; a PSRAM source is bounced through internal DMA memory
 * per transfer, so keep it small and the queue shallow (the 1.46's lesson). */
#define LCD_MAX_XFER     4096

#define OP_REG           0x02u          /* QSPI: register write, 1 line  */
#define OP_PIXELS        0x32u          /* QSPI: pixel write, 4 lines    */
#define QSPI(op, cmd)    ((int)(((op) << 24) | ((cmd) << 8)))

#define CMD_CASET  0x2A
#define CMD_RASET  0x2B
#define CMD_RAMWR  0x2C
#define CMD_WRDISBV 0x51                /* brightness 0..255             */

typedef struct { uint8_t cmd, len; uint8_t data[1]; uint8_t delay_ms; } init_cmd_t;

static const init_cmd_t INIT[] = {
    { 0xFE, 1, {0x20}, 0 },             /* vendor page 0x20 (BSP)        */
    { 0x19, 1, {0x10}, 0 },
    { 0x1C, 1, {0xA0}, 0 },
    { 0xFE, 1, {0x00}, 0 },             /* back to the user commands     */
    { 0xC4, 1, {0x80}, 0 },             /* SPI mode control: QSPI        */
    { 0x36, 1, {0x00}, 0 },             /* MADCTL: RGB, no mirror/swap   */
    { 0x3A, 1, {0x55}, 0 },             /* COLMOD: 16 bpp                */
    { 0x35, 1, {0x00}, 0 },             /* TEON (TE pin unused)          */
    { 0x53, 1, {0x20}, 0 },             /* brightness control on         */
    { CMD_WRDISBV, 1, {0x00}, 0 },      /* dark until the first frame    */
    { 0x63, 1, {0xFF}, 0 },             /* HBM brightness                */
    { 0x11, 0, {0}, 120 },              /* SLPOUT                        */
    { 0x29, 0, {0}, 0 },                /* DISPON                        */
};

static esp_lcd_panel_io_handle_t s_io = NULL;
static uint16_t *s_fb = NULL;
static SemaphoreHandle_t s_sent = NULL;     /* given when a pixel write went out */

static esp_err_t reg(uint8_t cmd, const void *data, size_t len)
{
    return esp_lcd_panel_io_tx_param(s_io, QSPI(OP_REG, cmd), data, len);
}

/* window (x, y, w, h) + RAMWR of w×h packed px; queued, not awaited */
static esp_err_t send(int x, int y, int w, int h, const uint16_t *px)
{
    const int x0 = x + LCD_COL_OFFSET, x1 = x0 + w - 1, y1 = y + h - 1;
    const uint8_t ca[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
    const uint8_t ra[4] = { (uint8_t)(y >> 8), (uint8_t)y, (uint8_t)(y1 >> 8), (uint8_t)y1 };
    ESP_RETURN_ON_ERROR(reg(CMD_CASET, ca, 4), TAG, "caset");
    ESP_RETURN_ON_ERROR(reg(CMD_RASET, ra, 4), TAG, "raset");
    return esp_lcd_panel_io_tx_color(s_io, QSPI(OP_PIXELS, CMD_RAMWR), px, (size_t)w * h * sizeof(uint16_t));
}

static bool s_pending = false;             /* an async blit's pixels are still going out (lcd_co5300_blit_async) */
static uint32_t s_prof[3];                 /* us since the last lcd_co5300_blit_prof: mirror copy, waiting, commands+queue */

/* wait for the async blit's transfer, if one is out (its give is the next on s_sent) */
static esp_err_t wait_pending(void)
{
    if (!s_pending) return ESP_OK;
    s_pending = false;
    return xSemaphoreTake(s_sent, pdMS_TO_TICKS(100)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t send_wait(int x, int y, int w, int h, const uint16_t *px)
{
    esp_err_t e = send(x, y, w, h, px);
    if (e == ESP_OK && xSemaphoreTake(s_sent, pdMS_TO_TICKS(100)) != pdTRUE) e = ESP_ERR_TIMEOUT;
    return e;
}

static esp_err_t send_rows(int y0, int y1)
{
    return send(0, y0, LCD_WIDTH, y1 - y0, s_fb + (size_t)y0 * LCD_WIDTH);
}

static bool IRAM_ATTR color_sent(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    (void)io; (void)e; (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_sent, &woken);
    portYIELD_FROM_ISR(woken);       /* spi_master drops the return value (the 1.46's lesson) */
    return woken == pdTRUE;
}

esp_err_t lcd_co5300_init(void)
{
    s_sent = xSemaphoreCreateBinary();
    if (!s_sent) return ESP_ERR_NO_MEM;

    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << LCD_RST, .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst), TAG, "rst gpio");

    spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .data0_io_num = LCD_DATA0, .data1_io_num = LCD_DATA1,
        .data2_io_num = LCD_DATA2, .data3_io_num = LCD_DATA3,
        .max_transfer_sz = LCD_MAX_XFER,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num         = LCD_CS,
        .dc_gpio_num         = -1,              /* QSPI: the opcode says what follows */
        .spi_mode            = 0,
        .pclk_hz             = LCD_QSPI_HZ,
        .trans_queue_depth   = 2,
        .on_color_trans_done = color_sent,
        .lcd_cmd_bits        = 32,
        .lcd_param_bits      = 8,
        .flags = { .quad_mode = true },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &s_io),
                        TAG, "panel io");

    /* hardware reset, as esp_lcd_co5300 (low 10 ms, then 150 ms to boot) */
    gpio_set_level(LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    for (size_t i = 0; i < sizeof(INIT) / sizeof(INIT[0]); i++) {
        const init_cmd_t *c = &INIT[i];
        ESP_RETURN_ON_ERROR(reg(c->cmd, c->len ? c->data : NULL, c->len), TAG, "init 0x%02x", c->cmd);
        if (c->delay_ms) vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
    }

    size_t fb_bytes = (size_t)LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t);
    s_fb = heap_caps_calloc(1, fb_bytes, MALLOC_CAP_SPIRAM);
    if (!s_fb) {
        ESP_LOGE(TAG, "no %u-byte PSRAM framebuffer", (unsigned)fb_bytes);
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(send_rows(0, LCD_HEIGHT), TAG, "initial black frame");
    ESP_RETURN_ON_ERROR(reg(CMD_WRDISBV, (uint8_t[]){ 0xFF }, 1), TAG, "brightness");
    ESP_LOGI(TAG, "CO5300 ready: %dx%d, %d MHz QSPI", LCD_WIDTH, LCD_HEIGHT, LCD_QSPI_HZ / 1000000);
    return ESP_OK;
}

uint16_t *lcd_co5300_framebuffer(void) { return s_fb; }

esp_err_t lcd_co5300_power(bool on)
{
    if (!s_io) return ESP_ERR_INVALID_STATE;
    if (!on) {
        ESP_RETURN_ON_ERROR(reg(CMD_WRDISBV, (uint8_t[]){ 0 }, 1), TAG, "brightness");
        ESP_RETURN_ON_ERROR(reg(0x28, NULL, 0), TAG, "DISPOFF");
        return reg(0x10, NULL, 0);                      /* SLPIN */
    }
    ESP_RETURN_ON_ERROR(reg(0x11, NULL, 0), TAG, "SLPOUT");
    vTaskDelay(pdMS_TO_TICKS(120));                     /* the datasheet's wake time */
    ESP_RETURN_ON_ERROR(reg(0x29, NULL, 0), TAG, "DISPON");
    return reg(CMD_WRDISBV, (uint8_t[]){ 0xFF }, 1);
}

esp_err_t lcd_co5300_level(uint8_t level)
{
    if (!s_io) return ESP_ERR_INVALID_STATE;
    return reg(CMD_WRDISBV, &level, 1);
}

esp_err_t lcd_co5300_flush(void)
{
    if (!s_io || !s_fb) return ESP_ERR_INVALID_STATE;
    wait_pending();
    return send_rows(0, LCD_HEIGHT);
}

esp_err_t lcd_co5300_flush_rows(int y0, int y1)
{
    if (!s_io || !s_fb) return ESP_ERR_INVALID_STATE;
    wait_pending();
    y0 = y0 < 0 ? 0 : y0 & ~1;
    y1 = y1 > LCD_HEIGHT ? LCD_HEIGHT : (y1 + 1) & ~1;
    if (y1 <= y0) return ESP_OK;
    return send_rows(y0, y1);
}

esp_err_t lcd_co5300_blit(int x, int y, int w, int h, const uint16_t *px)
{
    if (!s_io || !s_fb) return ESP_ERR_INVALID_STATE;
    for (int r = 0; r < h; r++)
        memcpy(s_fb + (size_t)(y + r) * LCD_WIDTH + x, px + (size_t)r * w, (size_t)w * sizeof(uint16_t));
    const int ya = (y + 1) & ~1, yb = (y + h) & ~1;     /* the even row pairs px holds whole */
    wait_pending();
    xSemaphoreTake(s_sent, 0);                          /* a flush's leftover give */
    esp_err_t e = ESP_OK;
    if (y & 1) e = send_wait(0, y - 1, LCD_WIDTH, 2, s_fb + (size_t)(y - 1) * LCD_WIDTH);
    if (e == ESP_OK && yb > ya) e = send_wait(x, ya, w, yb - ya, px + (size_t)(ya - y) * w);
    if (e == ESP_OK && ((y + h) & 1)) e = send_wait(0, yb, LCD_WIDTH, 2, s_fb + (size_t)yb * LCD_WIDTH);
    return e;
}

esp_err_t lcd_co5300_blit_async(int x, int y, int w, int h, const uint16_t *px)
{
    if (!s_io || !s_fb) return ESP_ERR_INVALID_STATE;
    const int64_t t0 = esp_timer_get_time();
    for (int r = 0; r < h; r++)                         /* the mirror, while the last window still goes out */
        memcpy(s_fb + (size_t)(y + r) * LCD_WIDTH + x, px + (size_t)r * w, (size_t)w * sizeof(uint16_t));
    const int ya = (y + 1) & ~1, yb = (y + h) & ~1;
    const int64_t t1 = esp_timer_get_time();
    esp_err_t e = wait_pending();                       /* the last window out: its buffer and the bus are free */
    xSemaphoreTake(s_sent, 0);                          /* a flush's leftover give */
    const int64_t t2 = esp_timer_get_time();
    if (e == ESP_OK && (y & 1)) e = send_wait(0, y - 1, LCD_WIDTH, 2, s_fb + (size_t)(y - 1) * LCD_WIDTH);
    if (e == ESP_OK && yb > ya) { e = send(x, ya, w, yb - ya, px + (size_t)(ya - y) * w); s_pending = e == ESP_OK; }
    const int64_t t3 = esp_timer_get_time();
    s_prof[0] += (uint32_t)(t1 - t0); s_prof[1] += (uint32_t)(t2 - t1); s_prof[2] += (uint32_t)(t3 - t2);
    if (e == ESP_OK && ((y + h) & 1)) { e = wait_pending(); if (e == ESP_OK) e = send_wait(0, yb, LCD_WIDTH, 2, s_fb + (size_t)yb * LCD_WIDTH); }
    return e;
}

esp_err_t lcd_co5300_blit_wait(void)
{
    const int64_t t = esp_timer_get_time();
    const esp_err_t e = wait_pending();
    s_prof[1] += (uint32_t)(esp_timer_get_time() - t);
    return e;
}

void lcd_co5300_blit_prof(uint32_t *out) { for (int i = 0; i < 3; i++) { out[i] += s_prof[i]; s_prof[i] = 0; } }
