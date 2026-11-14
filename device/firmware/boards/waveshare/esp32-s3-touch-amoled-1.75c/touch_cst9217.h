/*
 * SPDX-License-Identifier: MIT
 *
 * touch_cst9217.h: CST9217 capacitive touch (Waveshare AMOLED-1.75C), polled.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* Reset the chip, add it on `bus` and check it answers with its firmware's checkcode. */
esp_err_t cst9217_init(i2c_master_bus_handle_t bus);

/* One finger at most, in framebuffer pixels; returns the count. */
int cst9217_poll(uint16_t *xs, uint16_t *ys, int max);
