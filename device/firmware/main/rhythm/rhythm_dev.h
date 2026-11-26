/*
 * SPDX-License-Identifier: MIT
 *
 * rhythm_dev.h: the beat player's device side (rhythm_dev.c; the shared part is rhythm.h).
 */
#pragma once
#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"

esp_err_t rhythm_dev_init(void);
/** The audio task, between its jobs, while rhythm_wants_speaker(): plays until it ends or job_waiting() says a job
 *  wants the speaker (it fades and gives way). */
void rhythm_dev_play(bool (*job_waiting)(void));
/* the link's commands (cap "rhythm": boards with a speaker) */
esp_err_t rhythm_cmd_play(const cJSON *args, cJSON **out);
esp_err_t rhythm_cmd_set(const cJSON *args, cJSON **out);
esp_err_t rhythm_cmd_stop(const cJSON *args, cJSON **out);
