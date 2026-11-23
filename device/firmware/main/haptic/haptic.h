/*
 * SPDX-License-Identifier: MIT
 *
 * haptic.h: the vibration motor, on boards that wire one (molty_board_t.haptic_gpio).
 *
 *   motor    a switch on the board's haptic_gpio, HIGH = on (the 1.75C's coin motor behind a
 *            2N2222). The board's early_init holds it LOW from the first milliseconds;
 *            haptic_init takes it over as LEDC PWM (timer 0, channel 0, 25 kHz: above hearing),
 *            only once the hardware check passed (molty_board_mismatch).
 *   buzz     haptic_buzz plays a plan (haptic_plan.h) and returns at once: a one-shot
 *            esp_timer steps its segments and ends with the motor off. A new buzz replaces
 *            the running one. Every failure leaves the motor off.
 *   mic      never into the open mic: the motor sits next to the microphones, so a buzz that
 *            arrives while the mic streams (a listen) is dropped, not queued (ESP_ERR_INVALID_STATE,
 *            error "listening"). Every caller goes through haptic_buzz.
 *   quiet    the motor shakes the IMU: sense.c resets the shake detector while
 *            haptic_quiet() (the buzz, then HAPTIC_SETTLE_MS).
 *   cues     the device's own buzzes (haptic_cue): a question with buttons or a pairing code
 *            appears (double), an answer tapped (tick). Nothing else buzzes by itself.
 *
 * Boards without a motor: haptic_buzz answers ESP_ERR_NOT_SUPPORTED, haptic_quiet is false.
 *
 * Args (HTTP and the link's buzz): {ms} or {pattern: "<name>" | [on, off, on, ...]}, strength?
 *       (a name of haptic_plan.h's; strength 0..100, 100 when absent; ms and the pattern's whole
 *       capped at 2000).
 * HTTP: POST /api/haptic → {ok:true, ms, strength[, pattern]} (what started, after the cap),
 *         {ok:false, error:"no_motor"} | "listening" | "unknown_pattern" | "invalid_args".
 *       GET /api/diag reports "haptic": true when a motor is wired and its PWM came up.
 * Link: cmd buzz (haptic_cmd, cap "haptic") → {ms, strength[, pattern]}; refused no_motor,
 *       listening, unknown_pattern, invalid_args.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "haptic_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

/** After molty_board_init: the motor's PWM, off. A no-op without a motor pin. */
void      haptic_init(void);
/** True when the board wires a motor and its PWM came up. */
bool      haptic_available(void);
/** Play p (replacing a running buzz); returns at once. ESP_ERR_NOT_SUPPORTED without a motor,
 *  ESP_ERR_INVALID_STATE while the mic is open (dropped). Any task. */
esp_err_t haptic_buzz(const haptic_plan_t *p);
/** The device's own buzz at one of its moments: the named pattern at full strength; dropped like any
 *  buzz (no motor, the mic open). */
esp_err_t haptic_cue(const char *name);
/** True while a buzz runs and HAPTIC_SETTLE_MS after it: the IMU feels the motor. */
bool      haptic_quiet(void);

esp_err_t haptic_http_register(httpd_handle_t server);
/** The link's buzz command (link_cmd.h's handler). */
esp_err_t haptic_cmd(const cJSON *args, cJSON **result);

#ifdef __cplusplus
}
#endif
