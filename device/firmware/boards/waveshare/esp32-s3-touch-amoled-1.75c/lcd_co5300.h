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
#include <stdbool.h>
#include "esp_err.h"
#include "molty_board.h"

esp_err_t lcd_co5300_init(void);
uint16_t *lcd_co5300_framebuffer(void);
/** The panel asleep (false: brightness 0, display off, sleep in) or awake; RAM is kept. */
esp_err_t lcd_co5300_power(bool on);
esp_err_t lcd_co5300_level(uint8_t level);   /* brightness 0..255 */
esp_err_t lcd_co5300_flush(void);
esp_err_t lcd_co5300_flush_rows(int y0, int y1);
/** w×h packed px (wire order) at (x, y), x and w even; returns once px may be reused. */
esp_err_t lcd_co5300_blit(int x, int y, int w, int h, const uint16_t *px);
/** As lcd_co5300_blit, but it returns once the pixels are queued (px, when the DMA reads it as is, read until the next
 *  blit or lcd_co5300_blit_wait, which wait for it): the caller draws the next window meanwhile. */
esp_err_t lcd_co5300_blit_async(int x, int y, int w, int h, const uint16_t *px);
esp_err_t lcd_co5300_blit_wait(void);
/** us in the async blits since the last call, added into out[3]: the mirror's copy, waiting for the panel, the commands and queueing. */
void lcd_co5300_blit_prof(uint32_t *out);
/** Off: an async blit on whole row pairs skips the framebuffer mirror (only an odd edge row's pair is sent from it, and
 *  /api/framebuffer draws the frame afresh): the caller repaints in full before it sends odd edges again. */
void lcd_co5300_mirror(bool on);
/** The panel's tearing-effect pulses on LCD_TE (molty_display_te). */
void lcd_co5300_te(molty_te_t *out, bool window);
/** Until the pulses' total passes after (true), timeout_ms at most, or no TE line (false) (molty_display_te_wait). */
bool lcd_co5300_te_wait(uint32_t after, int timeout_ms);
/** EXPERIMENT (the bench's stuck=1): the next panel call counts a transfer that never ends (its recovery shown). */
void lcd_co5300_inject(void);
