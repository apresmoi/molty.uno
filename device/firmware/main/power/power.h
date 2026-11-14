/*
 * SPDX-License-Identifier: MIT
 *
 * power.h — where the battery goes, and the human's two quiet switches.
 *
 *   screen   off / on: mole_fluid_screen (the fluid stops, the panel sleeps). The PWR
 *            button's `screen` binding (sense.c) toggles it; POST /api/power sets it.
 *   dnd      do not disturb: the relay lets agents speak, listen or ask only in reply to
 *            the human (an utterance's turn_id), docs/AGENT-LINK.md. The binding's double
 *            press toggles it; kept in NVS ("power"/"dnd"); the link reports it after each
 *            welcome and each change ({"t":"evt","name":"dnd","data":{"on":bool}}).
 *   history  once a minute, one sample: the battery (mV, gauge %, charging, USB power)
 *            and how the minute was spent (seconds with the screen on, seconds of audio,
 *            the link up, Wi-Fi RSSI, dnd), so a drain can be pinned on what ran. An
 *            "EVT pwr ..." log line each minute; the last 12 h in PSRAM, written to
 *            /spiffs/power.bin every 10 min so a flat battery's last hours survive.
 *
 * HTTP: GET /api/power → {battery: {mv, pct, charging, usb} | null, screen, dnd,
 *                         drain_pct_h (the gauge's fall over the last hour on battery, or null),
 *                         cols: [...], history: [[...], ...] (oldest first)}
 *       POST /api/power {screen?: bool, dnd?: bool} → the same.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** After SPIFFS and the board: loads dnd and the saved history, starts the 1 Hz sampler. */
esp_err_t power_start(void);

bool     power_dnd(void);
void     power_dnd_set(bool on);
/** Moves at every dnd change (never 0): the link resends the state when it differs from
 *  the generation the relay has (0 after each welcome). */
unsigned power_dnd_gen(void);

esp_err_t power_http_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
