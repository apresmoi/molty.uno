/*
 * SPDX-License-Identifier: MIT
 *
 * sense.h — device events: what the board feels, told to the agent over the link
 * ({"t":"evt","name":"shake|tap|button",...}, docs/AGENT-LINK.md) and to the fluid
 * (a shake → a surprised flash, a button → wakes it; works with no agent at all).
 * Also carries the link's health to the fluid's status mark.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the events task (after the fluid and the agent link). */
esp_err_t sense_start(void);
/** From the one touch poller (molty_touch_poll): n points at panel px xs/ys. Records a
 *  tap on each touch-down edge outside pairing (the pairing tap is pairing's). */
void sense_touch(int n, const uint16_t *xs, const uint16_t *ys);

#ifdef __cplusplus
}
#endif
