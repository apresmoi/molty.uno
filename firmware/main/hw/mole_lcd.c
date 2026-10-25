/*
 * mole_lcd.c — SPD2010 LCD driver for Molty (IDF v5.5)
 *
 * Target board: Waveshare ESP32-S3-Touch-LCD-1.46
 * Pin reference: Waveshare 1.46 demo bundle: Arduino/examples/LVGL_Arduino/Display_SPD2010.h
 *
 * Notes:
 *  - No double-buffering, no DMA streaming.  Single PSRAM framebuffer + panel_draw_bitmap flush.
 *  - The TCA9554PWR I/O expander controls the LCD RST line (EXIO_PIN1 = bit 0).
 *  - The I2C bus is passed in from app_main (created by mole_imu_init or
 *    by this module's caller — see mole_lcd_init signature).
 *  - PSRAM allocation uses MALLOC_CAP_SPIRAM.  The framebuffer is 412*412*2 = 339,488 bytes.
 */

#include "mole_lcd.h"
#include "mole_tca9554.h"
#include "esp_lcd_spd2010.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "mole_lcd";

/* ── Pin definitions (from Display_SPD2010.h) ──────────────────────────────── */
#define LCD_BCLK        40
#define LCD_DATA0       46
#define LCD_DATA1       45
#define LCD_DATA2       42
#define LCD_DATA3       41
#define LCD_CS          21
#define LCD_TE          18
#define LCD_BL_GPIO      5      /* GPIO5 HIGH = backlight on */

/* TCA9554PWR pin number for LCD RST. Waveshare's Display_SPD2010.cpp uses
 * EXIO_PIN2 (bit 1 of the output register), not PIN1. PIN1 leaves the panel
 * held in reset; SPI init "succeeds" but the screen stays black. */
#define LCD_RST_EXIO    TCA9554_PIN2   /* = 1 (bit 1) */

/* SPI host */
#define LCD_SPI_HOST    SPI2_HOST

/* QSPI clock. Was 20 MHz (conservative). Pushed to 80 MHz to cut the full-frame
 * flush (~34 ms @20 MHz → ~8 ms). If the panel shows tearing/garbage at 80, drop
 * to 40. SPI2 max on the S3 is 80 MHz. */
#define LCD_QSPI_CLK_HZ  (80 * 1000 * 1000)

/* Max single SPI transfer: 2048 bytes (from Display_SPD2010.h) */
#define LCD_MAX_XFER     2048

/* ── Module state ───────────────────────────────────────────────────────────── */
static esp_lcd_panel_handle_t s_panel   = NULL;
static uint16_t              *s_fb      = NULL;   /* PSRAM framebuffer */

/* Packed-staging scratch for partial (sub-row-width) flushes — see
 * mole_lcd_flush_rect's header comment (mole_lcd.h) for why this exists (the
 * panel's draw_bitmap has no stride parameter). Sized to budgets.md §1.2's
 * "Dirty-rect packed staging" line (65,536 B); allocated once, lazily, in
 * PSRAM (same MALLOC_CAP_SPIRAM convention as the framebuffer itself). */
#define MOLE_LCD_STAGING_MAX_BYTES 65536
#define MOLE_LCD_STAGING_MAX_PX    (MOLE_LCD_STAGING_MAX_BYTES / sizeof(uint16_t))
static uint16_t *s_staging = NULL;

/* ── Helpers ────────────────────────────────────────────────────────────────── */

static esp_err_t backlight_on(void)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << LCD_BL_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_cfg), TAG, "backlight gpio_config");
    return gpio_set_level(LCD_BL_GPIO, 1);
}

/* ── Public API ─────────────────────────────────────────────────────────────── */

esp_err_t mole_lcd_init(i2c_master_bus_handle_t bus)
{
    esp_err_t ret;

    /* 1. TCA9554 — set LCD RST pin low, then high to bring the panel out of reset */
    ESP_RETURN_ON_ERROR(mole_tca9554_init(bus), TAG, "tca9554 init");

    /* RST low → wait → RST high */
    ESP_RETURN_ON_ERROR(mole_tca9554_set_pin(LCD_RST_EXIO, 0), TAG, "rst low");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(mole_tca9554_set_pin(LCD_RST_EXIO, 1), TAG, "rst high");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_LOGI(TAG, "LCD RST toggled via TCA9554 EXIO_PIN2");

    /* 2. Init SPI2 host for Quad-SPI */
    spi_bus_config_t bus_cfg = {
        .sclk_io_num     = LCD_BCLK,
        .data0_io_num    = LCD_DATA0,
        .data1_io_num    = LCD_DATA1,
        .data2_io_num    = LCD_DATA2,
        .data3_io_num    = LCD_DATA3,
        .max_transfer_sz = LCD_MAX_XFER,
    };
    ret = spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        /* ESP_ERR_INVALID_STATE means already initialized — OK if reusing host */
        ESP_RETURN_ON_ERROR(ret, TAG, "spi_bus_initialize");
    }

    /* 3. LCD panel IO — QSPI mode */
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num        = LCD_CS,
        .dc_gpio_num        = -1,          /* QSPI has no separate DC pin */
        .spi_mode           = 3,
        .pclk_hz            = LCD_QSPI_CLK_HZ,
        /* 2, not 10: the framebuffer is PSRAM, so spi_master bounces EVERY queued
         * color chunk through a fresh ~4 KB internal-DMA malloc that stays held
         * until the transaction is reaped. Depth 10 pinned up to ~40 KB of the
         * scarce internal heap per flush — starving Wi-Fi, TLS and, once it ran
         * dry, the flush itself ("send color failed", 2026-10-25). */
        .trans_queue_depth  = 2,
        .on_color_trans_done = NULL,
        .user_ctx           = NULL,
        .lcd_cmd_bits       = 32,          /* QSPI cmd width */
        .lcd_param_bits     = 8,
        .flags = {
            .quad_mode = true,
        },
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io),
        TAG, "esp_lcd_new_panel_io_spi");

    /* 4. Create SPD2010 panel (QSPI interface) */
    spd2010_vendor_config_t vendor_cfg = {
        .init_cmds      = NULL,    /* use built-in default sequence */
        .init_cmds_size = 0,
        .flags = { .use_qspi_interface = 1 },
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num  = -1,     /* RST is via TCA9554, not a direct GPIO */
        .rgb_ele_order   = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel  = 16,
        .vendor_config   = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_spd2010(io, &panel_cfg, &s_panel),
        TAG, "esp_lcd_new_panel_spd2010");

    /* 5. Reset (software, since reset_gpio_num=-1) + init */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel),  TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel),   TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on");

    /* 6. Allocate PSRAM framebuffer (412 * 412 * 2 bytes = 339,488 bytes) */
    size_t fb_bytes = (size_t)MOLE_LCD_WIDTH * MOLE_LCD_HEIGHT * sizeof(uint16_t);
    s_fb = (uint16_t *)heap_caps_malloc(fb_bytes, MALLOC_CAP_SPIRAM);
    if (!s_fb) {
        ESP_LOGE(TAG, "Failed to allocate %u-byte PSRAM framebuffer", (unsigned)fb_bytes);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "PSRAM framebuffer allocated: %u bytes @ %p", (unsigned)fb_bytes, s_fb);

    /* 6b. Packed-staging scratch for mole_lcd_flush_rect / mole_lcd_blit's
     * partial-width push (see mole_lcd.h). Non-fatal if it fails to allocate
     * — mole_lcd_flush_rect simply returns ESP_ERR_NO_MEM and callers fall
     * back to a full mole_lcd_flush(), never a garbled partial one. */
    s_staging = (uint16_t *)heap_caps_malloc(MOLE_LCD_STAGING_MAX_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_staging) {
        ESP_LOGW(TAG, "Failed to allocate %u-byte PSRAM staging buffer — partial-rect flush disabled",
                 (unsigned)MOLE_LCD_STAGING_MAX_BYTES);
    } else {
        ESP_LOGI(TAG, "PSRAM packed-staging buffer allocated: %u bytes @ %p",
                 (unsigned)MOLE_LCD_STAGING_MAX_BYTES, s_staging);
    }

    /* 7. Backlight on */
    ESP_RETURN_ON_ERROR(backlight_on(), TAG, "backlight_on");

    /* 8. Fill black */
    memset(s_fb, 0x00, fb_bytes);
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_draw_bitmap(s_panel,
            0, 0, MOLE_LCD_WIDTH, MOLE_LCD_HEIGHT,
            (const void *)s_fb),
        TAG, "initial fill black");

    ESP_LOGI(TAG, "SPD2010 LCD ready — 412x412, backlight on, screen black");
    return ESP_OK;
}

/* Pushes the (x,y,w,h) window ALREADY WRITTEN into s_fb to the panel, staging
 * it through s_staging in <=MOLE_LCD_STAGING_MAX_PX-sized row-chunks so the
 * panel always receives a tightly-packed (no stride gap) buffer — see
 * mole_lcd_flush_rect's header comment (mole_lcd.h) for why this is required
 * (the panel's draw_bitmap has no stride parameter; a raw strided pointer into
 * s_fb produces a sheared image for any w < MOLE_LCD_WIDTH). Shared by
 * mole_lcd_blit (after it writes the caller's data into s_fb) and
 * mole_lcd_flush_rect (which reads a window someone else already painted into
 * s_fb, e.g. the fluid drawer). */
static esp_err_t lcd_push_rect_staged(int x, int y, int w, int h)
{
    if (!s_staging) return ESP_ERR_NO_MEM;
    int rows_per_chunk = (int)(MOLE_LCD_STAGING_MAX_PX / (size_t)w);
    if (rows_per_chunk < 1) rows_per_chunk = 1; /* w alone should never exceed 412 <= MAX_PX */
    for (int y0 = 0; y0 < h; y0 += rows_per_chunk) {
        int chunk_h = h - y0;
        if (chunk_h > rows_per_chunk) chunk_h = rows_per_chunk;
        for (int row = 0; row < chunk_h; row++) {
            memcpy(s_staging + (size_t)row * w,
                   s_fb + (size_t)(y + y0 + row) * MOLE_LCD_WIDTH + x,
                   (size_t)w * sizeof(uint16_t));
        }
        esp_err_t e = esp_lcd_panel_draw_bitmap(s_panel, x, y + y0, x + w, y + y0 + chunk_h,
                                                 (const void *)s_staging);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}

esp_err_t mole_lcd_blit(int x, int y, int w, int h, const uint16_t *rgb565)
{
    if (!s_panel || !s_fb) return ESP_ERR_INVALID_STATE;
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return ESP_ERR_INVALID_ARG;
    if (x + w > MOLE_LCD_WIDTH || y + h > MOLE_LCD_HEIGHT) return ESP_ERR_INVALID_ARG;

    /* Write the region into the PSRAM framebuffer */
    for (int row = 0; row < h; row++) {
        uint16_t *dst = s_fb + (y + row) * MOLE_LCD_WIDTH + x;
        const uint16_t *src = rgb565 + row * w;
        memcpy(dst, src, (size_t)w * sizeof(uint16_t));
    }

    /* Push the updated rectangle to the panel. Full-width rows (w ==
     * MOLE_LCD_WIDTH) have no stride gap, so a direct pointer into s_fb is
     * already tightly packed — skip the staging copy in that (common,
     * mole_lcd_flush_rows-equivalent) case. Any narrower window MUST go
     * through the packed-staging path (see lcd_push_rect_staged). */
    if (w == MOLE_LCD_WIDTH) {
        return esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h,
                                         (const void *)(s_fb + (size_t)y * MOLE_LCD_WIDTH + x));
    }
    return lcd_push_rect_staged(x, y, w, h);
}

esp_err_t mole_lcd_fill(uint16_t color)
{
    if (!s_panel || !s_fb) return ESP_ERR_INVALID_STATE;
    size_t n = (size_t)MOLE_LCD_WIDTH * MOLE_LCD_HEIGHT;
    for (size_t i = 0; i < n; i++) s_fb[i] = color;
    return esp_lcd_panel_draw_bitmap(s_panel,
        0, 0, MOLE_LCD_WIDTH, MOLE_LCD_HEIGHT,
        (const void *)s_fb);
}

int mole_lcd_width(void)  { return MOLE_LCD_WIDTH; }
int mole_lcd_height(void) { return MOLE_LCD_HEIGHT; }

uint16_t *mole_lcd_framebuffer(void) { return s_fb; }

esp_err_t mole_lcd_flush(void)
{
    if (!s_panel || !s_fb) return ESP_ERR_INVALID_STATE;
    return esp_lcd_panel_draw_bitmap(s_panel,
        0, 0, MOLE_LCD_WIDTH, MOLE_LCD_HEIGHT,
        (const void *)s_fb);
}

esp_err_t mole_lcd_flush_rows(int y0, int y1)
{
    if (!s_panel || !s_fb) return ESP_ERR_INVALID_STATE;
    if (y0 < 0) y0 = 0;
    if (y1 > MOLE_LCD_HEIGHT) y1 = MOLE_LCD_HEIGHT;
    if (y1 <= y0) return ESP_OK;                       /* nothing to push */
    /* full-width rows are contiguous in the framebuffer → one tight transfer */
    return esp_lcd_panel_draw_bitmap(s_panel,
        0, y0, MOLE_LCD_WIDTH, y1,
        (const void *)(s_fb + (size_t)y0 * MOLE_LCD_WIDTH));
}

esp_err_t mole_lcd_flush_rect(int x, int y, int w, int h)
{
    if (!s_panel || !s_fb) return ESP_ERR_INVALID_STATE;
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return ESP_ERR_INVALID_ARG;
    if (x + w > MOLE_LCD_WIDTH || y + h > MOLE_LCD_HEIGHT) return ESP_ERR_INVALID_ARG;

    if (w == MOLE_LCD_WIDTH) {
        /* No stride gap — same fast path as mole_lcd_flush_rows. */
        return esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h,
                                         (const void *)(s_fb + (size_t)y * MOLE_LCD_WIDTH + x));
    }
    return lcd_push_rect_staged(x, y, w, h);
}
