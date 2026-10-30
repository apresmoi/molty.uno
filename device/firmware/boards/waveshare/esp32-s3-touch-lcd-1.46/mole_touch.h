/*
 * mole_touch.h — SPD2010 capacitive touch (Waveshare 1.46), IDF i2c_master API.
 *
 * Ported from Waveshare 1.46 demo bundle: ESP-IDF/.../Touch_Driver/Touch_SPD2010.c.
 * Shares the board's I2C bus (board.c; touch @ 0x53). Touch reset is on
 * the TCA9554 expander pin 1 (LCD reset is pin 2 — independent).
 *
 * Polled (no INT pin needed): mole_touch_poll() runs one status->HDP exchange
 * and returns the current finger positions in panel pixel coords (0..411).
 */
#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOLE_TOUCH_MAX_POINTS 5

/* Add the SPD2010 touch device on `bus`, reset it, prime the firmware handshake. */
esp_err_t mole_touch_init(i2c_master_bus_handle_t bus);

/* Poll current touch points. Fills xs/ys (panel px) up to `max`; returns count. */
int mole_touch_poll(uint16_t *xs, uint16_t *ys, int max);

#ifdef __cplusplus
}
#endif
