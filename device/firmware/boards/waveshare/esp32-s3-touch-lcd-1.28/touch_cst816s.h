/*
 * SPDX-License-Identifier: MIT
 *
 * touch_cst816s.h — CST816S capacitive touch (Waveshare 1.28), IDF i2c_master API.
 * Single touch, polled (the INT pin is not needed), coordinates in panel px.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

esp_err_t cst816s_init(i2c_master_bus_handle_t bus);
/** Current touch: fills xs[0]/ys[0]; returns 0 or 1. */
int cst816s_poll(uint16_t *xs, uint16_t *ys, int max);
