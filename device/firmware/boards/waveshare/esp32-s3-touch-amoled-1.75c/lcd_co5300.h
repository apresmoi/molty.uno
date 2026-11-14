/*
 * SPDX-License-Identifier: MIT
 *
 * lcd_co5300.h: CO5300 466×466 AMOLED over quad SPI (Waveshare AMOLED-1.75C).
 *
 * One RGB565 framebuffer in PSRAM, byte-swapped (panel wire order). The CO5300 takes
 * windows on even rows and columns only, so the framebuffer mirrors what the panel
 * shows: blits are copied into it, and a window's odd edge row goes out from it as
 * part of an even pair.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t lcd_co5300_init(void);
uint16_t *lcd_co5300_framebuffer(void);
esp_err_t lcd_co5300_flush(void);
esp_err_t lcd_co5300_flush_rows(int y0, int y1);
/** w×h packed px (wire order) at (x, y), x and w even; returns once px may be reused. */
esp_err_t lcd_co5300_blit(int x, int y, int w, int h, const uint16_t *px);
