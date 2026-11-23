#include "link_pair.h"
#include "link_client.h"
#include "link_identity.h"
#include "link_protocol.h"
#include "link_audio.h"
#include "link_ask.h"
#include "mole_creature.h"
#include "mole_fluid.h"
#include "fluid_ask.h"
#include "molty_board.h"
#include "esp_timer.h"
#include "haptic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

/* The device decides just before the relay's 120 s pairing window closes. */
#define PAIR_TIMEOUT_US 110000000LL

static char s_req[96], s_name[128], s_code[5];
static bool s_boot_released, s_power_released;
static atomic_bool s_active;
static atomic_int s_tap = -1;              /* the answer a tap picked (link_pair_tap): 0 accept, 1 reject, -1 none */
static fluid_ask_hit_t s_hit;              /* where the question's answers are (set before its seq) */
static atomic_uint s_pair_seq;                  /* ... its number there (mole_fluid_ask_show) */
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

/* Touch path, on a touch-down edge at (dx, dy) px from the screen's centre, while a pairing waits: a tap on the
 * accept or the reject arc of the question the panel shows (mole_fluid_ask_shown) is that answer; any other tap
 * answers nothing. */
void link_pair_tap(float dx, float dy)
{
    if (!atomic_load(&s_active)) return;
    const unsigned seq = atomic_load(&s_pair_seq);
    if (!seq || mole_fluid_ask_shown() != seq) return;
    const int w = molty_display_width(), h = molty_display_height();
    const float r = (w < h ? w : h) * 0.5f;
    const int pick = r > 0.f ? fluid_ask_pick(&s_hit, dx / r, dy / r) : -1;
    int none = -1;
    if (pick >= 0) atomic_compare_exchange_strong(&s_tap, &none, pick);
}

/* Accept = a tap on the accept arc or the board's accept button; reject = a tap on the reject arc or its reject button (if
 * any) or the timeout. The buttons are the board's (molty_board_init configures them). */
esp_err_t link_pair_init(void)
{
    return ESP_OK;
}

/* The question: "PAIR WITH <AGENT>?", a reminder to check the code, the code, and accept / reject as rim arcs
 * (fluid_ask.h). It stays up until the decision, over whatever shows. */
esp_err_t link_ui_show_pair(const char *agent_name, const char *code)
{
    esp_err_t e = mole_creature_select("fluid");
    if (e != ESP_OK) return e;
    fluid_ask_q_t q;
    memset(&q, 0, sizeof(q));
    const char *who = agent_name && agent_name[0] ? agent_name : "an agent";
    int len = (int)strlen(who);
    if (len > FLUID_ASK_TITLE_MAX - 11) len = FLUID_ASK_TITLE_MAX - 11;   /* "pair with " + it + "?" */
    char title[64];
    snprintf(title, sizeof(title), "pair with %.*s?", len, who);
    strcpy(q.title, title);
    strcpy(q.detail, "check the code matches");
    snprintf(q.code, sizeof(q.code), "%.4s", code);
    q.n = 2;
    strcpy(q.label[0], "accept"); strcpy(q.label[1], "reject");
    q.rgb[0] = fluid_ask_color(-1, 0); q.rgb[1] = fluid_ask_color(-1, 1);
    s_hit = fluid_ask_hit(&q);
    unsigned seq = 0;
    e = mole_fluid_ask_show(&q, &seq);
    atomic_store(&s_pair_seq, e == ESP_OK ? seq : 0u);
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

/* the pairing ends: its question leaves the screen (answered: from that answer; else the rim lets go) */
static void end_pair(int answer)
{
    if (!atomic_exchange(&s_active, false)) return;
    if (answer >= 0) mole_fluid_ask_pick(answer);
    mole_fluid_ask_end();
    atomic_store(&s_pair_seq, 0u);
}

void link_pair_cancel(void) { end_pair(-1); }

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
    link_ask_cancel();   /* the code takes the screen; its event leaves before pairing mutes events */
    strcpy(s_req, req->valuestring);
    strcpy(s_name, name->valuestring); strcpy(s_code, code->valuestring);
    s_deadline = esp_timer_get_time() + PAIR_TIMEOUT_US;
    atomic_store(&s_active, true);
    if (link_ui_show_pair(name->valuestring, code->valuestring) != ESP_OK) {
        result(s_req, false, "rejected", s_name); link_pair_cancel(); return;
    }
    atomic_store(&s_tap, -1);
    haptic_cue("double");   /* the human must look and accept */
    s_boot_released = !molty_button_pressed(MOLTY_BTN_ACCEPT);
    s_power_released = !molty_button_pressed(MOLTY_BTN_REJECT);
}

void link_pair_tick(void)
{
    if (!link_pair_active()) return;
    bool boot = molty_button_pressed(MOLTY_BTN_ACCEPT);
    if (!boot) s_boot_released = true;
    const int tap = atomic_exchange(&s_tap, -1);
    bool accept = tap == 0 || (boot && s_boot_released);
    bool power = molty_button_pressed(MOLTY_BTN_REJECT);
    if (!power) s_power_released = true;
    bool reject = (power && s_power_released) || tap == 1;
    bool expired = esp_timer_get_time() >= s_deadline;
    if (accept || expired || reject) {
        if (tap >= 0) haptic_cue("tick");   /* the tap on accept / reject was taken */
        result(s_req, accept && !expired && !reject, expired ? "timeout" : "rejected", s_name);
        end_pair(tap >= 0 ? tap : accept && !expired && !reject ? 0 : -1);   /* a tap or the button: from that answer */
    }
}
