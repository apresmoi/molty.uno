/*
 * SPDX-License-Identifier: MIT
 *
 * board.h: the pin map of the Waveshare ESP32-S3-Touch-AMOLED-1.75C.
 *
 * ESP32-S3R8 (8 MB octal PSRAM in package) + 32 MB QIO flash (esptool on the board;
 * the schematic's XM25QH128 is 16 MB), native USB on the USB-C port (GPIO19/20).
 * Sources, all cross-checked against each other:
 *   [S] ESP32-S3-Touch-AMOLED-1.75C-schematic.pdf (GPIO table, KEYS, QMI8658, AXP2101)
 *   [B] Waveshare BSP waveshare/esp32_s3_touch_amoled_1_75c 3.0.0 (the ESP-IDF demos' BSP)
 *   [A] the Arduino demos' board_config/pin_config.h
 * Not the 1.75 (non-C) sibling: its pinout differs (LCD/touch reset, MCLK, I/O expander).
 */
#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"

/* ── I2C bus [S][B][A]: CST9217 0x5A, QMI8658 0x6B, AXP2101 0x34, ES8311 0x18,
 *    ES7210 0x40, PCF85063 RTC 0x51 (unused) ─────────────────────────────────── */
#define BOARD_I2C_PORT      I2C_NUM_0
#define BOARD_I2C_SDA       15
#define BOARD_I2C_SCL       14

/* ── Display: CO5300 466×466 AMOLED, quad SPI (lcd_co5300.c) [S][B][A] ─────── */
#define LCD_WIDTH           466
#define LCD_HEIGHT          466
#define LCD_COL_OFFSET      6       /* panel RAM columns 6..471 are the glass [B] set_gap */
#define LCD_SCLK            38
#define LCD_DATA0           4
#define LCD_DATA1           5
#define LCD_DATA2           6
#define LCD_DATA3           7
#define LCD_CS              12
#define LCD_RST             1       /* active low                                    */
#define LCD_TE              13      /* the panel's tearing effect [S]: an input, counted (lcd_co5300_te); no backlight
                                     * pin: brightness is panel command 0x51 */

/* ── Touch: CST9217, polled (touch_cst9217.c) [S][B][A] ─────────────────────── */
#define TOUCH_ADDR          0x5A
#define TOUCH_RST           2       /* active low; INT (GPIO11) unused: polled       */

/* ── Audio (audio_es.c) [S][B][A]: one duplex I2S port, ES8311 DAC + NS4150B amp,
 *    ES7210 ADC with the two mics on MIC1/MIC2 (MIC3 = AEC loopback, unused) ──── */
#define AUDIO_MCLK          16
#define AUDIO_BCLK          9
#define AUDIO_LRCK          45
#define AUDIO_DOUT          8       /* to ES8311 DSDIN                               */
#define AUDIO_DIN           10      /* from ES7210 SDOUT1                            */
#define AUDIO_PA            46      /* HIGH = speaker amp on [B] pa_reverted = false */

/* ── IMU: QMI8658, SA0 to GND = 0x6B [S][A]. Chip → device axes (qmi8658.h): the
 *    silkscreen [S p.2] puts chip +X toward the top of the screen and +Y toward its
 *    left, and the Arduino widgets demo shows the UI upright at chip X ≈ +1 g. The
 *    device frame wants the gravity direction (fluid: dot_dx = +ax, dot_dy = -ay),
 *    as the Immersive_block demo moves blocks (screen dx = +acc_y, dy = +acc_x):
 *    ax = +chip_y, ay = -chip_x.
 *    TODO(hardware): confirm by tilting, the fluid must run downhill. ─────────── */
#define BOARD_IMU_ADDR      0x6B
#define BOARD_IMU_AXES      { 2, -1, 3 }

/* ── PMU: AXP2101 [S]. DCDC1 = VCC3V3 (system, AMOLED FPC), ALDO1 = A3V3 (codecs);
 *    the vendor's AXP demo leaves only these two on ──────────────────────────── */
#define PMU_ADDR            0x34

/* ── Buttons [S KEYS] ──────────────────────────────────────────────────────── */
#define BOARD_BTN_BOOT      GPIO_NUM_0  /* pairing: accept; external pull-up, LOW = pressed */
#define BOARD_BTN_PWR       GPIO_NUM_3  /* pairing: reject; SYS_OUT, the PWR key through a
                                         * BSS138 inverter: HIGH = pressed. The key is also the
                                         * AXP2101 PWRON: a long press powers the board off. */

/* ── Vibration motor (haptic.h): not on the board. A 3 V coin motor (80 mA) behind a 2N2222
 *    low-side switch, soldered to the bare GPIO17 pad: GPIO17 → 470 Ω → base, HIGH = on ── */
#define BOARD_HAPTIC_GPIO   17
