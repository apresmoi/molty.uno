/*
 * SPDX-License-Identifier: MIT
 *
 * board.h — Waveshare ESP32-S3-Touch-LCD-1.28: the pin map.
 *
 * ESP32-S3R2 (2 MB quad PSRAM in package) + 16 MB W25Q128 flash. The USB-C port is a
 * CH343P USB-UART bridge on UART0 (GPIO43/44): flashing and logs, no native USB.
 * From Waveshare's 1.28 demo (ESP32-S3-Touch-LCD-1.28-Test: DEV_Config.h,
 * LCD_1in28.cpp, CST816S.*, QMI8658.*) and the wiki.
 */
#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"

/* ── I2C bus: CST816S touch (0x15), QMI8658 IMU (0x6B, or 0x6A) ─────────────── */
#define BOARD_I2C_PORT      I2C_NUM_0
#define BOARD_I2C_SDA       6
#define BOARD_I2C_SCL       7

/* ── Display: GC9A01A 240×240 round IPS, 4-wire SPI (lcd_gc9a01.c) ──────────── */
#define LCD_WIDTH           240
#define LCD_HEIGHT          240
#define LCD_SPI_HOST        SPI2_HOST
#define LCD_PIN_DC          8
#define LCD_PIN_CS          9
#define LCD_PIN_SCLK        10
#define LCD_PIN_MOSI        11
#define LCD_PIN_MISO        12      /* wired, unused: the panel is write-only here  */
#define LCD_PIN_RST         14
#define LCD_PIN_BL          2       /* HIGH = backlight on (the demo's PWM pin)     */
#define LCD_SPI_CLK_HZ      (80 * 1000 * 1000)   /* the demo's 80 MHz, SPI mode 0  */

/* ── Touch: CST816S (touch_cst816s.c) ──────────────────────────────────────── */
#define TOUCH_ADDR          0x15
#define TOUCH_PIN_RST       13
#define TOUCH_PIN_INT       5       /* unused: polled, like the 1.46               */

/* ── IMU: QMI8658; chip → device axes, as verified on this board by
 *    experiments/arduino/1.28/ws_stream (ax = +chip_y, ay = +chip_x) ────────── */
#define BOARD_IMU_AXES      { 2, 1, 3 }

/* ── Button + battery ──────────────────────────────────────────────────────── */
#define BOARD_BTN_BOOT      GPIO_NUM_0  /* pairing: accept (there is no second button) */
#define BOARD_BAT_ADC_GPIO  1           /* battery through a 1:3 divider (wiki)     */
#define BOARD_BAT_DIVIDER   3
