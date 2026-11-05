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
 * The framebuffer lives in PSRAM (412×412×2 = ~340 KB RGB565); mole_lcd_flush /
 * mole_lcd_flush_rows push it. mole_lcd_blit sends a caller's packed window instead
 * (the fluid's: drawn in internal RAM, no framebuffer behind it).
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
 * Requires the board's I2C bus (board.c creates it; the TCA9554 expander that
 * drives the LCD reset sits on it).
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
 * Send w×h packed RGB565 (wire order) px to the panel window (x, y, w, h) and wait
 * until it went out: px may be reused on return. The SPD2010 takes windows only at x
 * and w multiples of 4. px in internal DMA-capable RAM goes out as is (anywhere else
 * spi_master bounces it through internal RAM). The framebuffer is not touched.
 */
esp_err_t mole_lcd_blit(int x, int y, int w, int h, const uint16_t *px);

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

#ifdef __cplusplus
}
#endif
