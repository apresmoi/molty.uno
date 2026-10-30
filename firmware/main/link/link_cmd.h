#pragma once
#include "cJSON.h"
#include "esp_err.h"
typedef esp_err_t (*link_cmd_handler_t)(const cJSON *args, cJSON **result);
esp_err_t link_cmd_register(const char *name, link_cmd_handler_t handler);
void link_cmd_init(void);
esp_err_t link_cmd_run(const char *name, const cJSON *args, cJSON **result);
/* A handler refusing with its own error code (the res's "error", e.g. self_update's
 * "bad_url"): return link_cmd_refuse("code"). The code must have static storage. Link
 * task only (commands run there); link_cmd_refusal() reads it back for LINK_CMD_REFUSED. */
#define LINK_CMD_REFUSED 0x7a01
esp_err_t link_cmd_refuse(const char *code);
const char *link_cmd_refusal(void);
/* The creature in that mood. hold_s < 0: the mood's default (happy / surprised revert
 * to idle after a few seconds, the rest persist); 0: until the next mood; > 0: seconds. */
esp_err_t link_cmd_mood(const char *mood, double hold_s);
const char *link_cmd_current_mood(void);
/* Count of explicit moods accepted so far (link `mood` command, local POST with a mood).
 * link_audio compares it across a job: completion reverts to idle only if unchanged. */
unsigned link_cmd_mood_gen(void);
/* A raw state set (local HTTP API, the link's `state` command): applied, and counted as an explicit mood when it
 * carries a top-level mood string (as the creature reads it), atomically against link_cmd_mood_idle_unless. */
esp_err_t link_cmd_state_set_explicit(const char *body);
/* A screen owner other than the audio jobs (the self-update's question-then-progress):
 * applies `body` and counts it like an explicit mood, under the same lock, so a listen/say
 * that started earlier and ends meanwhile does not put idle over it. */
esp_err_t link_cmd_screen_claim(const char *body);
/* A listen/say ended: back to idle unless an explicit mood was accepted since gen0. */
void link_cmd_mood_idle_unless(unsigned gen0);
/* Shared with the local reader HTTP endpoint; preserves its routing semantics. */
esp_err_t link_ui_set_text(const char *text, int len);
