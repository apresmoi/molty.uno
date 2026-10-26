/*
 * SPDX-License-Identifier: MIT
 *
 * board.h — My Vendor My-Model: the pin map. Every pin of the board in one place,
 * with where it came from (vendor demo file, schematic page).
 */
#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"

/* ── Display ───────────────────────────────────────────────────────────────── */
#define LCD_WIDTH           240
#define LCD_HEIGHT          240
/* #define LCD_PIN_SCLK  ..   LCD_PIN_MOSI ..   LCD_PIN_CS ..   LCD_PIN_DC ..
 * #define LCD_PIN_RST   ..   LCD_PIN_BL   .. (and its polarity)                    */

/* ── I2C bus (touch, IMU, expanders...) ────────────────────────────────────── */
#define BOARD_I2C_PORT      I2C_NUM_0
/* #define BOARD_I2C_SDA ..   BOARD_I2C_SCL ..                                       */

/* ── Buttons: active low with a pull-up is the usual wiring ─────────────────── */
#define BOARD_BTN_ACCEPT    GPIO_NUM_0  /* BOOT: pairing accept                       */
/* #define BOARD_BTN_REJECT .. if the board has a second button                      */
