/*
 * SPDX-License-Identifier: MIT
 *
 * board.h — Waveshare ESP32-S3-Touch-LCD-1.46: the pin map.
 *
 * ESP32-S3R8 module (16 MB QIO flash, 8 MB octal PSRAM), native USB on the USB-C
 * port. From Waveshare's 1.46 demo bundle (Display_SPD2010.h, TCA9554PWR.h,
 * Audio_PCM5101.h, MIC_MSM.h, Gyro_QMI8658.h) and the board schematic.
 */
#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"

/* ── I2C bus: QMI8658 (0x6B), TCA9554 expander (0x20), SPD2010 touch (0x53) ── */
#define BOARD_I2C_PORT      I2C_NUM_0
#define BOARD_I2C_SDA       11
#define BOARD_I2C_SCL       10

/* ── Display: SPD2010 412×412 round, quad SPI (mole_lcd.c) ───────────────── */
#define LCD_BCLK            40
#define LCD_DATA0           46
#define LCD_DATA1           45
#define LCD_DATA2           42
#define LCD_DATA3           41
#define LCD_CS              21
#define LCD_TE              18
#define LCD_BL_GPIO          5      /* HIGH = backlight on                          */
/* LCD reset = TCA9554 EXIO2, touch reset = EXIO1 (mole_lcd.c, mole_touch.c)     */

/* ── Audio (mole_audio.c): PCM5101 DAC on I2S TX, MSM mic on I2S RX ─────────── */
#define AUDIO_TX_BCLK       48
#define AUDIO_TX_LRC        38
#define AUDIO_TX_DOUT       47
#define AUDIO_RX_CLK        15
#define AUDIO_RX_WS          2
#define AUDIO_RX_DIN        39

/* ── IMU: QMI8658, SA0 high; chip → device axes (qmi8658.h) ─────────────────── */
#define BOARD_IMU_ADDR      0x6B
#define BOARD_IMU_AXES      { -2, -1, 3 }

/* ── Buttons (active low, internal pull-up) + soft-power latch ──────────────── */
#define BOARD_BTN_BOOT      GPIO_NUM_0  /* pairing: accept                          */
#define BOARD_BTN_PWR       GPIO_NUM_6  /* pairing: reject                          */
#define BOARD_PWR_LATCH     GPIO_NUM_7  /* HIGH within ~100 ms keeps battery power   */
