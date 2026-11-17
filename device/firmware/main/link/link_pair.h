#pragma once
#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"
esp_err_t link_pair_init(void);
void link_pair_request(const cJSON *args);
void link_pair_tick(void);
void link_pair_cancel(void);
bool link_pair_active(void);
/* Touch path: a tap at (dx, dy) px from the screen's centre while a pairing waits (on its accept / reject arc: that
 * answer; elsewhere nothing). */
void link_pair_tap(float dx, float dy);
/* Adds "pair" (awaiting the tap, or null) and "last_pair" (most recent
 * decision, or null) to j. Caller holds the link client's config lock. */
void link_pair_status(cJSON *j);
esp_err_t link_ui_show_pair(const char *agent_name, const char *code);
