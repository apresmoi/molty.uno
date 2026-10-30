/*
 * SPDX-License-Identifier: MIT
 *
 * lcd_gc9a01.h — GC9A01A 240×240 round panel over 4-wire SPI (Waveshare 1.28).
 *
 * One RGB565 framebuffer in PSRAM, byte-swapped (panel wire order), like the 1.46's.
 * Flushes are full frames or full-width row bands, so a flush is one window + one
 * RAMWR stream (esp_lcd panel IO splits it into DMA-sized transfers).
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t lcd_gc9a01_init(void);
uint16_t *lcd_gc9a01_framebuffer(void);
esp_err_t lcd_gc9a01_flush(void);
esp_err_t lcd_gc9a01_flush_rows(int y0, int y1);
