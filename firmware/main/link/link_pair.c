#include "link_pair.h"
#include "link_client.h"
#include "link_identity.h"
#include "link_protocol.h"
#include "link_audio.h"
#include "mole_creature.h"
#include "molty_board.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

/* The device decides just before the relay's 120 s pairing window closes. */
#define PAIR_TIMEOUT_US 110000000LL

static char s_req[96], s_previous[32], s_name[128], s_code[5];
static char *s_state;
static bool s_boot_released, s_power_released;
static atomic_bool s_active, s_down, s_tap;
static int64_t s_deadline;
/* Most recent decision, RAM only, for GET /api/link (seq 0 = none since boot).
 * Written by result() and read by link_pair_status(), both under the link
 * client's config lock. */
static struct { unsigned seq; bool ok; char name[128], reason[16]; } s_last;

bool link_pair_active(void) { return atomic_load(&s_active); }

void link_pair_status(cJSON *j)
{
    if (atomic_load(&s_active)) {
        cJSON *p = cJSON_AddObjectToObject(j, "pair");
        int64_t left = s_deadline - esp_timer_get_time();
        if (p) {
            cJSON_AddStringToObject(p, "agent_name", s_name);
            cJSON_AddStringToObject(p, "code", s_code);
            cJSON_AddNumberToObject(p, "expires_in_s", left > 0 ? (double)((left + 999999) / 1000000) : 0);
        }
    } else cJSON_AddNullToObject(j, "pair");
    if (s_last.seq) {
        cJSON *l = cJSON_AddObjectToObject(j, "last_pair");
        if (l) {
            cJSON_AddNumberToObject(l, "seq", s_last.seq);
            cJSON_AddStringToObject(l, "agent_name", s_last.name);
            cJSON_AddBoolToObject(l, "ok", s_last.ok);
            if (s_last.ok) cJSON_AddNullToObject(l, "reason");
            else cJSON_AddStringToObject(l, "reason", s_last.reason);
        }
    } else cJSON_AddNullToObject(j, "last_pair");
}

/* Called by the one touch poller (molty_touch_poll); never runs I2C a second time. */
void link_pair_touch(int count)
{
    bool down = count > 0;
    bool old = atomic_exchange(&s_down, down);
    if (down && !old) atomic_store(&s_tap, true);
}

/* Accept = a touch tap or the board's accept button; reject = its reject button (if
 * any) or the timeout. The buttons are the board's (molty_board_init configures them). */
esp_err_t link_pair_init(void)
{
    return ESP_OK;
}

/* Static prompt: the 4-digit code alone, condensed in the fluid. A lone token is
 * centred with no ORP accent, and "hold" keeps it (no return to the creature) until
 * the decision. The agent name is shown by the console and returned in GET /api/link. */
esp_err_t link_ui_show_pair(const char *agent_name, const char *code)
{
    (void)agent_name;
    esp_err_t e = mole_creature_select("fluid");
    if (e != ESP_OK) return e;
    cJSON *j = cJSON_CreateObject();
    if (!j) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(j, "state", "text"); cJSON_AddStringToObject(j, "text", code);
    cJSON_AddBoolToObject(j, "hold", true);
    char *body = cJSON_PrintUnformatted(j); cJSON_Delete(j);
    if (!body) return ESP_ERR_NO_MEM;
    e = mole_creature_state_set(body); free(body);
    return e;
}

static void result(const char *req, bool ok, const char *reason, const char *agent_name)
{
    s_last.seq++; s_last.ok = ok;
    snprintf(s_last.name, sizeof(s_last.name), "%s", agent_name ? agent_name : "");
    snprintf(s_last.reason, sizeof(s_last.reason), "%s", reason ? reason : "");
    cJSON *j = link_pair_frame(req, ok, reason);
    if (!j) return;
    if (ok) {
        link_identity_rotate();
        /* Relay approval deletes the old code asynchronously. Carry the new
         * one in that same transaction so the following link_code frame cannot
         * race that deletion. The owner task also announces this rotation. */
        cJSON_AddStringToObject(j, "link_code", link_identity_code());
    }
    link_send_json(j);
    cJSON_Delete(j);
}

void link_pair_cancel(void)
{
    if (!atomic_exchange(&s_active, false)) return;
    /* back to what was shown — the creature when that was text or the demo reel: the
     * reel never comes back on its own (link_restore_body) */
    if (s_previous[0] && mole_creature_select(s_previous) == ESP_OK && s_state) {
        char *body = link_restore_body(s_state);
        if (body) mole_creature_state_set(body);
        free(body);
    }
    free(s_state); s_state = NULL;
}

void link_pair_request(const cJSON *args)
{
    cJSON *req = cJSON_GetObjectItemCaseSensitive(args, "req");
    cJSON *name = cJSON_GetObjectItemCaseSensitive(args, "agent_name");
    cJSON *code = cJSON_GetObjectItemCaseSensitive(args, "code");
    if (!cJSON_IsString(req) || !req->valuestring[0] || strlen(req->valuestring) >= sizeof(s_req)) return;
    const char *who = cJSON_IsString(name) && strlen(name->valuestring) < sizeof(s_name) ? name->valuestring : NULL;
    /* a board with neither touch nor an accept button could never say yes */
    bool can_accept = molty_board_has(MOLTY_CAP_TOUCH) || molty_board_has(MOLTY_CAP_BTN_ACCEPT);
    if (!can_accept || link_pair_active() || link_audio_busy() || !cJSON_IsString(name) || strlen(name->valuestring) > 120 ||
        !cJSON_IsString(code) || strlen(code->valuestring) != 4 || strspn(code->valuestring, "0123456789") != 4) {
        result(req->valuestring, false, "rejected", who); return;
    }
    const char *cur = mole_creature_current();
    snprintf(s_previous, sizeof(s_previous), "%s", cur ? cur : "");
    if (cur && mole_creature_state_get(&s_state) != ESP_OK) { result(req->valuestring, false, "rejected", who); return; }
    strcpy(s_req, req->valuestring);
    strcpy(s_name, name->valuestring); strcpy(s_code, code->valuestring);
    s_deadline = esp_timer_get_time() + PAIR_TIMEOUT_US;
    atomic_store(&s_active, true);
    if (link_ui_show_pair(name->valuestring, code->valuestring) != ESP_OK) {
        result(s_req, false, "rejected", s_name); link_pair_cancel(); return;
    }
    atomic_store(&s_tap, false);
    s_boot_released = !molty_button_pressed(MOLTY_BTN_ACCEPT);
    s_power_released = !molty_button_pressed(MOLTY_BTN_REJECT);
}

void link_pair_tick(void)
{
    if (!link_pair_active()) return;
    bool boot = molty_button_pressed(MOLTY_BTN_ACCEPT);
    if (!boot) s_boot_released = true;
    bool accept = atomic_exchange(&s_tap, false) || (boot && s_boot_released);
    bool power = molty_button_pressed(MOLTY_BTN_REJECT);
    if (!power) s_power_released = true;
    bool reject = power && s_power_released;
    bool expired = esp_timer_get_time() >= s_deadline;
    if (accept || expired || reject) {
        result(s_req, accept && !expired && !reject, expired ? "timeout" : "rejected", s_name);
        link_pair_cancel();
    }
}
