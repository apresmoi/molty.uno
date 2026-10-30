#pragma once
#include "cJSON.h"
#include "esp_err.h"
typedef esp_err_t (*link_cmd_handler_t)(const cJSON *args, cJSON **result);
esp_err_t link_cmd_register(const char *name, link_cmd_handler_t handler);
void link_cmd_init(void);
esp_err_t link_cmd_run(const char *name, const cJSON *args, cJSON **result);
/* The creature in that mood. hold_s < 0: the mood's default (happy / surprised revert
 * to idle after a few seconds, the rest persist); 0: until the next mood; > 0: seconds. */
esp_err_t link_cmd_mood(const char *mood, double hold_s);
const char *link_cmd_current_mood(void);
/* Shared with the local reader HTTP endpoint; preserves its routing semantics. */
esp_err_t link_ui_set_text(const char *text, int len);
