/*
 * SPDX-License-Identifier: MIT
 *
 * lcd_co5300.c: CO5300 466×466 AMOLED over quad SPI (Waveshare AMOLED-1.75C).
 *
 * A minimal driver on IDF's spi_master: hardware reset, the Waveshare BSP's register sequence
 * (esp32_s3_touch_amoled_1_75c.c lcd_init_cmds, after esp_lcd_co5300's MADCTL/COLMOD), then window + RAMWR
 * writes. In QSPI every register write is opcode 0x02 + a 24-bit address holding the command in bits 15..8, and
 * pixels go out with 0x32 (+ RAMWR's address), the data on all four lines. The glass starts at RAM column 6 (the
 * BSP's set_gap). Brightness is the panel's 0x51, held at 0 until the first black frame is on the glass.
 *
 * The transfers are lcd_q's (lcd_q.h): queued transactions with bounded waits and a fixed pair of bounce buffers,
 * so no panel call waits for ever and the SPI master allocates nothing; a transfer that does not end resets the
 * device (logged) and the caller repaints. (esp_lcd's panel io, before, waited portMAX_DELAY for each transfer it
 * counted in flight.)
 *
 * Windows must start and end on even rows and columns (the BSP's LVGL rounder rounds
 * x1/y1 down to even, x2/y2 up to odd). Columns come aligned (molty_display_blit: x
 * and w are multiples of 4, or w reaches the even right edge); rows do not, so the
 * framebuffer mirrors the panel and an odd edge row is sent from it with its pair.
 * Pins: board.h.
 */
#include "lcd_co5300.h"
#include "lcd_q.h"
#include "board.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "lcd_co5300";

#define LCD_SPI_HOST     SPI2_HOST
/* The vendor demos' 40 MHz (BSP and Arduino_ESP32QSPI). The 1.46's SPD2010 runs at 80:
 * worth trying here once the panel is known good. */
#define LCD_QSPI_HZ      (80 * 1000 * 1000)

#define OP_REG           0x02u          /* QSPI: register write, 1 line  */
#define OP_PIXELS        0x32u          /* QSPI: pixel write, 4 lines    */
#define ADDR(cmd)        ((uint32_t)(cmd) << 8)

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
    { 0x35, 1, {0x00}, 0 },             /* TEON: a pulse each refresh    */
    { 0x53, 1, {0x20}, 0 },             /* brightness control on         */
    { CMD_WRDISBV, 1, {0x00}, 0 },      /* dark until the first frame    */
    { 0x63, 1, {0xFF}, 0 },             /* HBM brightness                */
    { 0x11, 0, {0}, 120 },              /* SLPOUT                        */
    { 0x29, 0, {0}, 0 },                /* DISPON                        */
};

/* ── the SPI device under lcd_q ── */
static const spi_device_interface_config_t DEV = {
    .flags = SPI_DEVICE_HALFDUPLEX, .clock_speed_hz = LCD_QSPI_HZ, .mode = 0,
    .spics_io_num = LCD_CS, .queue_size = LQ_DEPTH,
};
static spi_device_handle_t s_dev;
static spi_transaction_ext_t s_t[LQ_DEPTH];        /* (a slot's transaction, while it is queued) */
static lq_t s_q;
static uint16_t *s_fb = NULL;

static int dev_queue(void *ctx, int slot, const lq_tx_t *t)
{
    (void)ctx;
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    spi_transaction_ext_t *x = &s_t[slot];
    memset(x, 0, sizeof(*x));
    x->base.flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | (t->flags & LQ_QUAD ? SPI_TRANS_MODE_QIO : 0) |
                    (t->flags & LQ_KEEP ? SPI_TRANS_CS_KEEP_ACTIVE : 0);
    x->command_bits = t->flags & LQ_CMD ? 8 : 0;
    x->address_bits = t->flags & LQ_CMD ? 24 : 0;
    x->base.cmd = t->cmd; x->base.addr = t->addr;
    x->base.length = t->len * 8;
    if (t->data) x->base.tx_buffer = t->data;
    else if (t->len) { x->base.flags |= SPI_TRANS_USE_TXDATA; memcpy(x->base.tx_data, t->d, t->len); }
    return spi_device_queue_trans(s_dev, &x->base, 0);  /* (lcd_q never queues more than the device's queue holds) */
}
static int dev_result(void *ctx, int *slot, int wait_ms)
{
    (void)ctx;
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    spi_transaction_t *t = NULL;
    const esp_err_t e = spi_device_get_trans_result(s_dev, &t, pdMS_TO_TICKS(wait_ms));
    if (e == ESP_OK) *slot = (int)((spi_transaction_ext_t *)t - s_t);
    return e;
}
static int dev_hold(void *ctx, bool on)
{
    (void)ctx;
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    if (!on) { spi_device_release_bus(s_dev); return ESP_OK; }
    return spi_device_acquire_bus(s_dev, portMAX_DELAY);   /* (lcd_q takes it only once every transfer has ended) */
}
static int dev_reset(void *ctx)
{
    (void)ctx;
    if (s_dev) {
        const esp_err_t e = spi_bus_remove_device(s_dev);   /* refused while a transfer is still queued */
        if (e != ESP_OK) return e;
        s_dev = NULL;
    }
    return spi_bus_add_device(LCD_SPI_HOST, &DEV, &s_dev);
}
static bool dev_dma_ok(void *ctx, const void *p) { (void)ctx; return esp_ptr_dma_capable(p); }
static const lq_io_t IO = { dev_queue, dev_result, dev_hold, dev_reset, dev_dma_ok, NULL };

/* a failure of lcd_q's (it is clean again, or stuck: lq.h), logged with where it was: a few a boot */
static esp_err_t logged(esp_err_t e)
{
    static uint32_t seen; static int n;
    if (e == ESP_OK || s_q.fails == seen) return e;
    seen = s_q.fails;
    if (n++ < 8)
        ESP_LOGE(TAG, "panel transfer failed (%s at %d): %lu queued %lu ended, %lu dropped, %lu resets%s; dma free %u largest %u",
                 esp_err_to_name(s_q.err), s_q.err_at, (unsigned long)s_q.head, (unsigned long)s_q.tail, (unsigned long)s_q.dropped,
                 (unsigned long)s_q.resets, s_q.stuck ? ", STUCK (tried again each call)" : "",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    return e;
}

static esp_err_t reg(uint8_t cmd, const uint8_t *data, size_t len)
{
    return logged(lq_reg(&s_q, OP_REG, ADDR(cmd), data, (uint32_t)len));
}

/* window (x, y, w, h) + RAMWR of w×h packed px; queued, not awaited */
static esp_err_t send(int x, int y, int w, int h, const uint16_t *px)
{
    const int x0 = x + LCD_COL_OFFSET, x1 = x0 + w - 1, y1 = y + h - 1;
    const lq_tx_t pre[2] = {
        { NULL, 4, ADDR(CMD_CASET), OP_REG, LQ_CMD, { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 } },
        { NULL, 4, ADDR(CMD_RASET), OP_REG, LQ_CMD, { (uint8_t)(y >> 8), (uint8_t)y, (uint8_t)(y1 >> 8), (uint8_t)y1 } },
    };
    return logged(lq_window(&s_q, pre, 2, OP_PIXELS, ADDR(CMD_RAMWR), (const uint8_t *)px, (uint32_t)w * h * sizeof(uint16_t)));
}

static esp_err_t send_rows(int y0, int y1)
{
    return send(0, y0, LCD_WIDTH, y1 - y0, s_fb + (size_t)y0 * LCD_WIDTH);
}

static uint32_t s_prof[3];
static bool s_mirror = true;               /* async blits keep the framebuffer mirror (lcd_co5300_mirror)          */

/* The tearing effect (TEON: a pulse at each refresh's blanking) on LCD_TE, an input only: counted with its period by a
 * tiny interrupt (no logging, no allocation), which also wakes a task waiting for one (lcd_co5300_te_wait: the fluid
 * paces its frames by them). If it cannot be installed the panel goes on without it (logged once): the waits say so. */
static portMUX_TYPE s_te_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_te_on;                            /* (its interrupt installed) */
static uint32_t s_te_total, s_te_k, s_te_min = UINT32_MAX, s_te_max;
static uint64_t s_te_sum;
static int64_t s_te_last;
static SemaphoreHandle_t s_te_sem;              /* given at each pulse (lcd_co5300_te_wait) */
static void te_isr(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    const int64_t t = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&s_te_mux);
    if (s_te_last) {
        const uint32_t dt = (uint32_t)(t - s_te_last);
        s_te_sum += dt; s_te_k++;
        if (dt < s_te_min) s_te_min = dt;
        if (dt > s_te_max) s_te_max = dt;
    }
    s_te_last = t; s_te_total++;
    portEXIT_CRITICAL_ISR(&s_te_mux);
    if (s_te_sem) xSemaphoreGiveFromISR(s_te_sem, &woken);
    portYIELD_FROM_ISR(woken);
}
static void te_start(void)
{
    const gpio_config_t te = {
        .pin_bit_mask = 1ULL << LCD_TE, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_ENABLE,   /* (a quiet low if the panel drives none) */
        .intr_type = GPIO_INTR_POSEDGE,
    };
    s_te_sem = xSemaphoreCreateBinary();            /* (none: no waits, the fluid paces by its timer) */
    esp_err_t e = gpio_config(&te);
    if (e == ESP_OK) { e = gpio_install_isr_service(0); if (e == ESP_ERR_INVALID_STATE) e = ESP_OK; }   /* (another's already in) */
    if (e == ESP_OK) e = gpio_isr_handler_add(LCD_TE, te_isr, NULL);
    if (e != ESP_OK) { ESP_LOGW(TAG, "TE line on GPIO%d not read (%s)", LCD_TE, esp_err_to_name(e)); gpio_set_intr_type(LCD_TE, GPIO_INTR_DISABLE); return; }
    s_te_on = true;
}

esp_err_t lcd_co5300_init(void)
{
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
        .max_transfer_sz = LQ_XFER,          /* (its DMA descriptors: 3 of 12 B) */
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");
    ESP_RETURN_ON_ERROR(spi_bus_add_device(LCD_SPI_HOST, &DEV, &s_dev), TAG, "spi device");
    lq_init(&s_q, &IO, &(lq_tx_t){ NULL, 0, ADDR(0x00), OP_REG, LQ_CMD, { 0 } });   /* (NOP: closes a cut frame) */
    /* the bounce buffers, once (none: each PSRAM chunk is bounced by the SPI master, as esp_lcd did; logged) */
    uint8_t *bounce = heap_caps_malloc(LQ_BUFS * LQ_BUF, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!bounce) ESP_LOGW(TAG, "no %d B of DMA RAM for the bounce buffers: the SPI master bounces each chunk", LQ_BUFS * LQ_BUF);
    lq_bounce(&s_q, bounce, LQ_BUFS, LQ_BUF);

    /* hardware reset, as esp_lcd_co5300 (low 10 ms, then 150 ms to boot) */
    gpio_set_level(LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    for (size_t i = 0; i < sizeof(INIT) / sizeof(INIT[0]); i++) {
        const init_cmd_t *c = &INIT[i];
        ESP_RETURN_ON_ERROR(reg(c->cmd, c->data, c->len), TAG, "init 0x%02x", c->cmd);
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
    te_start();
    return ESP_OK;
}

void lcd_co5300_inject(void) { s_q.inject = true; }

void lcd_co5300_te(molty_te_t *out, bool window)
{
    portENTER_CRITICAL(&s_te_mux);
    *out = (molty_te_t){ s_te_on, s_te_total, s_te_k, s_te_k ? (uint32_t)(s_te_sum / s_te_k) : 0, s_te_k ? s_te_min : 0, s_te_max, s_te_last };
    if (window) { s_te_k = 0; s_te_sum = 0; s_te_min = UINT32_MAX; s_te_max = 0; }
    portEXIT_CRITICAL(&s_te_mux);
}

bool lcd_co5300_te_wait(uint32_t after, int timeout_ms)
{
    if (!s_te_on || !s_te_sem) return false;
    const int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        portENTER_CRITICAL(&s_te_mux);
        const uint32_t n = s_te_total;
        portEXIT_CRITICAL(&s_te_mux);
        if ((int32_t)(n - after) > 0) return true;
        const int64_t left = end - esp_timer_get_time();
        if (left <= 0) return false;
        const TickType_t ticks = pdMS_TO_TICKS(left / 1000);
        xSemaphoreTake(s_te_sem, ticks ? ticks : 1);    /* (a pulse wakes it at once; the ticks only bound it) */
    }
}

uint16_t *lcd_co5300_framebuffer(void) { return s_fb; }

esp_err_t lcd_co5300_power(bool on)
{
    if (!s_fb) return ESP_ERR_INVALID_STATE;
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
    if (!s_fb) return ESP_ERR_INVALID_STATE;
    return reg(CMD_WRDISBV, &level, 1);
}

esp_err_t lcd_co5300_flush(void)
{
    if (!s_fb) return ESP_ERR_INVALID_STATE;
    return send_rows(0, LCD_HEIGHT);
}

esp_err_t lcd_co5300_flush_rows(int y0, int y1)
{
    if (!s_fb) return ESP_ERR_INVALID_STATE;
    y0 = y0 < 0 ? 0 : y0 & ~1;
    y1 = y1 > LCD_HEIGHT ? LCD_HEIGHT : (y1 + 1) & ~1;
    if (y1 <= y0) return ESP_OK;
    return send_rows(y0, y1);
}

esp_err_t lcd_co5300_blit_async(int x, int y, int w, int h, const uint16_t *px)
{
    if (!s_fb) return ESP_ERR_INVALID_STATE;
    const int64_t t0 = esp_timer_get_time();
    if (s_mirror || (y & 1) || (h & 1))                 /* the mirror, while the last window still goes out */
        for (int r = 0; r < h; r++) memcpy(s_fb + (size_t)(y + r) * LCD_WIDTH + x, px + (size_t)r * w, (size_t)w * sizeof(uint16_t));
    const int ya = (y + 1) & ~1, yb = (y + h) & ~1;     /* the even row pairs px holds whole */
    const int64_t t1 = esp_timer_get_time();
    esp_err_t e = logged(lq_wait(&s_q));                /* the last window out: its source and the bus are free */
    const int64_t t2 = esp_timer_get_time();
    if (e == ESP_OK && (y & 1)) e = send(0, y - 1, LCD_WIDTH, 2, s_fb + (size_t)(y - 1) * LCD_WIDTH);
    if (e == ESP_OK && yb > ya) e = send(x, ya, w, yb - ya, px + (size_t)(ya - y) * w);
    if (e == ESP_OK && ((y + h) & 1)) e = send(0, yb, LCD_WIDTH, 2, s_fb + (size_t)yb * LCD_WIDTH);
    const int64_t t3 = esp_timer_get_time();
    s_prof[0] += (uint32_t)(t1 - t0); s_prof[1] += (uint32_t)(t2 - t1); s_prof[2] += (uint32_t)(t3 - t2);
    return e;
}

esp_err_t lcd_co5300_blit(int x, int y, int w, int h, const uint16_t *px)
{
    const esp_err_t e = lcd_co5300_blit_async(x, y, w, h, px);
    return e == ESP_OK ? logged(lq_wait(&s_q)) : e;
}

esp_err_t lcd_co5300_blit_wait(void)
{
    const int64_t t = esp_timer_get_time();
    const esp_err_t e = logged(lq_wait(&s_q));
    s_prof[1] += (uint32_t)(esp_timer_get_time() - t);
    return e;
}

void lcd_co5300_mirror(bool on) { s_mirror = on; }

void lcd_co5300_blit_prof(uint32_t *out) { for (int i = 0; i < 3; i++) { out[i] += s_prof[i]; s_prof[i] = 0; } }
