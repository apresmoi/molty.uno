/*
 * mole_lcd.h — SPD2010 LCD driver for Molty (IDF v5.5)
 *
 * Target: Waveshare ESP32-S3-Touch-LCD-1.46
 * Panel: SPD2010, 412×412, Quad-SPI
 *
 * Pins (from Waveshare 1.46 demo bundle: .../Display_SPD2010.h):
 *   BCLK=40, DATA0=46, DATA1=45, DATA2=42, DATA3=41, CS=21, TE=18
 *   Backlight: GPIO5 (HIGH = on)
 *   Reset: via TCA9554PWR EXIO_PIN1 (I2C expander, addr 0x20)
 *
 * The framebuffer lives in PSRAM (412×412×2 = ~340 KB RGB565).
 * mole_lcd_fill / mole_lcd_blit write into the PSRAM framebuffer and then
 * call esp_lcd_panel_draw_bitmap to push to the panel (no double-buffering).
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOLE_LCD_WIDTH   412
#define MOLE_LCD_HEIGHT  412

/**
 * Initialise the LCD panel.
 * Requires the I2C bus to be created already (mole_imu_init creates it;
 * call mole_lcd_init BEFORE starting the IMU task so the bus is shared).
 *
 * Sequence:
 *   1. Add TCA9554 on the bus, toggle LCD RST low→high.
 *   2. Configure Quad-SPI + SPI2 host.
 *   3. Create SPD2010 panel, reset + init.
 *   4. Allocate PSRAM framebuffer.
 *   5. Turn on backlight (GPIO5 HIGH).
 *   6. Fill screen black.
 */
esp_err_t mole_lcd_init(i2c_master_bus_handle_t bus);

/**
 * Write a rectangular region from rgb565 data into the PSRAM framebuffer,
 * then push that region to the panel via draw_bitmap.
 * (x,y) is the top-left corner, w×h is the region size.
 * The caller's buffer must be exactly w*h uint16_t values in row-major order.
 */
esp_err_t mole_lcd_blit(int x, int y, int w, int h, const uint16_t *rgb565);

/**
 * Fill the entire 412×412 panel with a single RGB565 colour.
 */
esp_err_t mole_lcd_fill(uint16_t color);

/** Returns MOLE_LCD_WIDTH (412). */
int mole_lcd_width(void);

/** Returns MOLE_LCD_HEIGHT (412). */
int mole_lcd_height(void);

/** Returns a pointer to the PSRAM framebuffer (412×412 uint16_t, row-major). */
uint16_t *mole_lcd_framebuffer(void);

/**
 * Flush the entire framebuffer to the panel.
 * Call after directly writing into mole_lcd_framebuffer().
 */
esp_err_t mole_lcd_flush(void);

/**
 * Flush only full-width rows [y0, y1) of the framebuffer to the panel (a
 * "windowed" partial update). These rows are contiguous in the framebuffer, so
 * this is a single tight transfer — push ~(y1-y0)/412 of the usual data. Used
 * for dirty-region rendering: flush only the band of rows that changed.
 */
esp_err_t mole_lcd_flush_rows(int y0, int y1);

/**
 * Flush an arbitrary (x,y,w,h) rectangle of the framebuffer to the panel —
 * general dirty-RECT partial flush (narrower than a full-width row band).
 *
 * WHY THIS EXISTS (the mole_lcd_blit stride bug,
 * dirty-rect packed staging): the panel's `draw_bitmap` (esp_lcd_spd2010.c
 * panel_spd2010_draw_bitmap) has NO stride/pitch parameter — it reads exactly
 * (x_end-x_start)*(y_end-y_start) CONTIGUOUS pixels starting at the pointer it
 * is given (confirmed: `len = (x_end-x_start)*(y_end-y_start)*bpp/8`, then one
 * contiguous `tx_color` of `len` bytes). The framebuffer's row stride is always
 * MOLE_LCD_WIDTH (412) pixels, so for w < MOLE_LCD_WIDTH, a raw sub-window
 * pointer into the framebuffer (`fb + y*412 + x`) is NOT the tightly-packed
 * w*h buffer the panel driver assumes — row 1 of the "window" the driver reads
 * actually still belongs to row 0 of the real framebuffer (it reads through
 * the 412-w gap), producing a diagonally-sheared/garbled image on any
 * partial-width blit. This is exactly what `mole_lcd_blit` (below) does when
 * w < MOLE_LCD_WIDTH — a latent bug (that function has no callers today, so
 * it has never been exercised at w < 412). `mole_lcd_flush_rect` is the fix:
 * it stages the dirty window's rows into a CONTIGUOUS scratch buffer (no gap)
 * before handing it to the panel, in chunks that each stay under
 * MOLE_LCD_STAGING_MAX_BYTES (65,536 B) so it works for any w<=412, any h,
 * with a single fixed-size
 * scratch allocation (no per-call malloc).
 *
 * (x,y,w,h) must already be clipped to [0,412)x[0,412) by the caller (mirrors
 * mole_lcd_blit's existing bounds contract — returns ESP_ERR_INVALID_ARG
 * otherwise). Reads directly from the live framebuffer (mole_lcd_framebuffer()
 * — caller must have already written the dirty pixels there), unlike
 * mole_lcd_blit which takes an external source.
 */
esp_err_t mole_lcd_flush_rect(int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif
