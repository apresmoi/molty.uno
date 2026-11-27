/*
 * SPDX-License-Identifier: MIT
 *
 * fps_lab_dev.h: the frame rate lab on the device (fps_lab.h, FPS-LAB.md).
 *   GET  /api/lab            the knobs, their defaults, and the summaries of what they turned on
 *   GET  /api/lab?raw=<n>    the newest n frame records as CSV (rec on)
 *   POST /api/lab            {"<knob>": <int>, ...} or {"reset": 1}: set, answered as GET
 * Its task (made at the first POST, PSRAM) makes and frees the rings and logs a few `lab` lines every 10 s while a knob
 * is off its default.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"

esp_err_t fps_lab_http_register(httpd_handle_t server);
/** The knobs (GET /api/diag's "lab"). */
cJSON *fps_lab_knobs_json(void);
