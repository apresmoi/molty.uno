/*
 * SPDX-License-Identifier: MIT
 *
 * fluid_director.c — what the fluid shows, and the creature's idle life (fluid_director.h).
 * Plain C: built into the firmware and by test/host/director_test.c.
 */
#include "fluid_director.h"
#include "fluid_sim.h"      /* FLUID_FX_*, fluid_clamp01 */
#include "cute_spec.h"      /* CM_* moods */
#include "cute_act.h"       /* CA_* actions */

#include <stddef.h>

const char *const FD_STATUS_NAMES[FD_STATUS_COUNT] = { "ok", "wifi", "relay", "auth" };
const char *const FD_IDLE_NAMES[FD_IDLE_COUNT] = { "off", "calm", "lively" };

const fd_cfg_t FD_CFG_DEFAULT = {
    .lively_min_s = 10.f, .lively_max_s = 40.f,
    .calm_min_s = 20.f, .calm_max_s = 60.f,
    .yawn_lead_s = 3.f,
    .act_ttl_s = 3.f,
    .convo_window_s = 8.f, .convo_gap_s = 60.f,
    .sleep_after_s = 300.f,
    .text_return_s = 1.5f,
    .status_after_s = 10.f,       /* boot: Wi-Fi join + TLS take a few seconds */
    .work_ttl_s = 120.f, .work_gap_min_s = 0.5f, .work_gap_max_s = 1.6f,
};

/* flashes: a say that ended well (happy), a shake (surprised) */
#define FLASH_HAPPY_S      2.0f
#define FLASH_SURPRISED_S  1.2f   /* the pose needs ~0.5–1 s to blend in */
#define CONVO_DELAY_S      1.0f   /* the creature settles back before the flash */

/* idle life's library: (action, weight) for lively; calm keeps only the small ones */
typedef struct { int8_t act; float w; bool calm; } idle_item_t;
static const idle_item_t IDLE_LIB[] = {
    { CA_GLANCE_LEFT, 3.f, true }, { CA_GLANCE_RIGHT, 3.f, true }, { CA_BLINK, 3.f, true },
    { CA_LOOK_UP, 2.f, true }, { CA_WOBBLE, 1.5f, false }, { CA_STRETCH, 1.5f, false },
};
#define IDLE_N ((int)(sizeof(IDLE_LIB) / sizeof(IDLE_LIB[0])))
/* `working`'s library: (action, weight); the big two come up most */
typedef struct { int8_t act; float w; } work_item_t;
static const work_item_t WORK_LIB[] = {
    { CA_INSPECT, 3.f }, { CA_PADDLE, 3.f }, { CA_PEEK, 2.f }, { CA_GLANCE_LEFT, 1.5f },
    { CA_GLANCE_RIGHT, 1.5f }, { CA_STRETCH, 1.f }, { CA_WIGGLE, 1.f },
};
#define WORK_N ((int)(sizeof(WORK_LIB) / sizeof(WORK_LIB[0])))
/* an agent's quick moods, when it gives no hold_s */
#define HOLD_HAPPY_S       3.0f
#define HOLD_SURPRISED_S   2.0f

/* main.js REEL (the demo, only on "autoplay"): st −1 = keep the state, only change the
 * creature's mood (mood −1 = keep). dur ≤ 0: the text runs to its end. */
typedef struct { int8_t st, fx, mood; float dur; } reel_step_t;
static const reel_step_t REEL[] = {
    { FD_ST_BLOB, 0, -1, 3.2f },
    { FD_ST_CREATURE, 0, CM_IDLE, 2.8f },
    { -1, 0, CM_HAPPY, 2.6f },
    { FD_ST_FX, FLUID_FX_RIPPLE, -1, 2.8f },
    { FD_ST_TEXT, 0, -1, 0.f },
    { FD_ST_CREATURE, 0, CM_SPEAKING, 3.4f },
    { -1, 0, CM_THINKING, 3.0f },
    { FD_ST_FX, FLUID_FX_VORTEX, -1, 2.8f },
    { FD_ST_BLOB, 0, -1, 2.4f },
    { FD_ST_CREATURE, 0, CM_LISTENING, 2.6f },
    { -1, 0, CM_SLEEPY, 3.0f },
    { FD_ST_FX, FLUID_FX_WAVE, -1, 2.8f },
};
#define REEL_N ((int)(sizeof(REEL) / sizeof(REEL[0])))

static float frand(fluid_director_t *d) {
    uint32_t x = d->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    d->rng = x;
    return (float)(x >> 8) * (1.f / 16777216.f);
}

static float idle_gap(fluid_director_t *d) {
    bool calm = d->idle_life == FD_IDLE_CALM;
    float a = calm ? d->cfg.calm_min_s : d->cfg.lively_min_s, b = calm ? d->cfg.calm_max_s : d->cfg.lively_max_s;
    return a + (b - a) * frand(d);
}

/* weighted, never the last one again */
static int idle_pick(fluid_director_t *d) {
    bool calm = d->idle_life == FD_IDLE_CALM;
    float sum = 0.f;
    for (int i = 0; i < IDLE_N; i++) if ((!calm || IDLE_LIB[i].calm) && IDLE_LIB[i].act != d->last_idle) sum += IDLE_LIB[i].w;
    float r = frand(d) * sum;
    int pick = -1;
    for (int i = 0; i < IDLE_N; i++) {
        if ((calm && !IDLE_LIB[i].calm) || IDLE_LIB[i].act == d->last_idle) continue;
        pick = IDLE_LIB[i].act;
        if ((r -= IDLE_LIB[i].w) < 0.f) break;
    }
    return pick;
}

static int work_pick(fluid_director_t *d) {
    float sum = 0.f;
    for (int i = 0; i < WORK_N; i++) if (WORK_LIB[i].act != d->work_last) sum += WORK_LIB[i].w;
    float r = frand(d) * sum;
    int pick = -1;
    for (int i = 0; i < WORK_N; i++) {
        if (WORK_LIB[i].act == d->work_last) continue;
        pick = WORK_LIB[i].act;
        if ((r -= WORK_LIB[i].w) < 0.f) break;
    }
    return pick;
}

static void start_act(fluid_director_t *d, int act) {
    d->act_start = act;
    d->acting_left = cute_act_dur(act);
}

/* queue a transition; the creature falls back to the blob while none is loaded */
static void go(fluid_director_t *d, int st, int fx) {
    d->want_creature = st == FD_ST_CREATURE;
    if (st == FD_ST_CREATURE && !d->has_creature) st = FD_ST_BLOB;
    d->pending = true; d->next = st; d->next_fx = st == FD_ST_FX ? fx : 0;
}

static int target(const fluid_director_t *d) { return d->pending ? d->next : d->cur; }
static int target_fx(const fluid_director_t *d) { return d->pending ? d->next_fx : d->cur_fx; }

void fd_init(fluid_director_t *d, const fd_cfg_t *cfg, uint32_t seed) {
    *d = (fluid_director_t){ 0 };
    d->cfg = cfg ? *cfg : FD_CFG_DEFAULT;
    d->rng = seed ? seed : 0x6d6f6c74u;
    d->cur = FD_ST_BLOB;
    d->reel = -1;
    d->held = CM_IDLE; d->held_left = -1.f;
    d->flash = -1;
    d->idle_life = FD_IDLE_DEFAULT;
    d->last_idle = d->act_req = d->act_start = d->work_last = -1;
    d->since_convo = 1e9f;
    d->next_idle = idle_gap(d);
    go(d, FD_ST_CREATURE, 0);              /* home: the blob until a creature loads */
}

void fd_set_creature(fluid_director_t *d, bool loaded) {
    d->has_creature = loaded;
    if (loaded && d->want_creature && target(d) == FD_ST_BLOB) go(d, FD_ST_CREATURE, 0);
    else if (!loaded && target(d) == FD_ST_CREATURE) go(d, FD_ST_CREATURE, 0);   /* → blob */
}

void fd_request_state(fluid_director_t *d, int st, int fx, bool text_hold) {
    if (st < 0 || st >= FD_ST_COUNT) return;
    if (d->autoplay) { d->autoplay = false; d->held = CM_IDLE; d->held_left = -1.f; }
    if (st == FD_ST_TEXT) d->text_hold = text_hold;
    int mapped = st == FD_ST_CREATURE && !d->has_creature ? FD_ST_BLOB : st;
    /* the guard: the shown state again changes nothing (no ripple, no swirl, no melt) */
    if (st != FD_ST_TEXT && mapped == target(d) && (mapped != FD_ST_FX || fx == target_fx(d))) {
        d->want_creature = st == FD_ST_CREATURE;
        return;
    }
    go(d, st, fx);
}

void fd_autoplay(fluid_director_t *d) {
    d->autoplay = true; d->reel = -1; d->reel_t = 0.f;
    d->flash = -1; d->asleep = false; d->convo_left = 0.f;
}

float fd_default_hold(int mood) {
    return mood == CM_HAPPY ? HOLD_HAPPY_S : mood == CM_SURPRISED ? HOLD_SURPRISED_S : -1.f;
}

void fd_activity(fluid_director_t *d) {
    d->asleep = false; d->quiet = 0.f; d->yawned = false;
    d->next_idle = idle_gap(d);            /* no idle action right after a command */
}

void fd_agent_mood(fluid_director_t *d, int mood, float hold_s) {
    if (mood < 0 || mood >= CUTE_N_MOODS) return;
    d->held = mood;
    d->work_left = 0.f;                    /* any mood ends `working` */
    d->held_left = mood == CM_IDLE ? -1.f : hold_s < 0.f ? fd_default_hold(mood) : hold_s == 0.f ? -1.f : hold_s;
    d->flash = -1;                         /* the agent's word wins over a flash */
    fd_activity(d);
}

void fd_agent_work(fluid_director_t *d, float hold_s) {
    fd_agent_mood(d, CM_IDLE, -1.f);
    d->work_left = hold_s > 0.f && hold_s < d->cfg.work_ttl_s ? hold_s : d->cfg.work_ttl_s;
    d->work_gap = 0.f;                     /* the first one at once */
}

bool fd_working(const fluid_director_t *d) { return d->work_left > 0.f; }

void fd_user(fluid_director_t *d) { d->asleep = false; d->quiet = 0.f; d->yawned = false; }

void fd_set_idle_life(fluid_director_t *d, int level) {
    if (level < 0 || level >= FD_IDLE_COUNT || level == d->idle_life) return;
    d->idle_life = level;
    d->next_idle = idle_gap(d);
    if (level == FD_IDLE_OFF) d->convo_left = 0.f;
}

void fd_set_busy(fluid_director_t *d, bool busy, bool covered) {
    if ((busy || covered) && !d->busy) d->work_left = 0.f;   /* something took the screen: the work is over */
    d->busy = busy || covered; d->covered = covered;
}

void fd_agent_act(fluid_director_t *d, int act) {
    if (act < 0 || act >= CUTE_N_ACTS) return;
    d->act_req = act; d->act_req_ttl = d->cfg.act_ttl_s;
    fd_activity(d);
}

void fd_turn_spoken(fluid_director_t *d) {
    if (d->idle_life == FD_IDLE_OFF || d->since_convo < d->cfg.convo_gap_s) return;
    d->convo_left = d->cfg.convo_window_s; d->convo_delay = CONVO_DELAY_S;
}

int fd_take_act(fluid_director_t *d) {
    int a = d->act_start;
    d->act_start = -1;
    return a;
}

void fd_shake(fluid_director_t *d) {
    fd_user(d);
    d->flash = CM_SURPRISED; d->flash_left = FLASH_SURPRISED_S;
}

void fd_set_status(fluid_director_t *d, int status) {
    if (status >= 0 && status < FD_STATUS_COUNT) d->status = status;
}

static void reel_step(fluid_director_t *d) {
    d->reel = (d->reel + 1) % REEL_N; d->reel_t = 0.f;
    const reel_step_t *r = &REEL[d->reel];
    if (r->mood >= 0) { d->held = r->mood; d->held_left = -1.f; }
    if (r->st >= 0) { if (r->st == FD_ST_TEXT) d->text_hold = false; go(d, r->st, r->fx); }
}

void fd_tick(fluid_director_t *d, float dt, bool text_done) {
    d->state_t += dt;
    bool reading = d->cur == FD_ST_TEXT && !d->pending;
    d->text_done_t = reading && text_done ? d->text_done_t + dt : 0.f;

    if (d->autoplay) {
        d->reel_t += dt;
        if (d->reel < 0 || (reading ? text_done : d->reel_t >= REEL[d->reel].dur)) reel_step(d);
    } else if (reading && !d->text_hold && d->text_done_t >= d->cfg.text_return_s) {
        go(d, FD_ST_CREATURE, 0);          /* show / say: back to the creature */
    }

    if (d->held_left > 0.f && (d->held_left -= dt) <= 0.f) {
        d->held = CM_IDLE; d->held_left = -1.f;
        d->next_idle = idle_gap(d);
    }
    if (d->flash >= 0 && (d->flash_left -= dt) <= 0.f) d->flash = -1;
    if (d->acting_left > 0.f) d->acting_left -= dt;
    d->since_convo += dt;

    /* an agent's action: once the creature is on screen and nothing covers it */
    const bool shown = !d->autoplay && !d->pending && d->cur == FD_ST_CREATURE;
    if (d->act_req >= 0) {
        if (shown && !d->covered) { start_act(d, d->act_req); d->act_req = -1; }
        else if ((d->act_req_ttl -= dt) <= 0.f) d->act_req = -1;
    }

    /* working: the agent is busy, so is the creature (fd_agent_work woke it; work_ttl_s < sleep_after_s) */
    if (d->work_left > 0.f) {
        if ((d->work_left -= dt) <= 0.f) { d->work_left = 0.f; d->next_idle = idle_gap(d); }
        else if (shown && !d->busy && d->acting_left <= 0.f && d->act_start < 0 && d->act_req < 0 && (d->work_gap -= dt) <= 0.f) {
            d->work_last = work_pick(d);
            start_act(d, d->work_last);
            d->work_gap = d->cfg.work_gap_min_s + (d->cfg.work_gap_max_s - d->cfg.work_gap_min_s) * frand(d);
        }
    }

    /* idle life: only while the creature is on screen, idle, and nobody drives it */
    d->quiet += dt;
    if (shown && d->held == CM_IDLE) {
        if (!d->asleep && d->quiet >= d->cfg.sleep_after_s) { d->asleep = true; d->flash = -1; d->convo_left = 0.f; }
    }
    const bool free = shown && d->held == CM_IDLE && !d->asleep && !d->busy && d->flash < 0 && d->work_left <= 0.f &&
                      d->idle_life != FD_IDLE_OFF && d->acting_left <= 0.f && d->act_start < 0;
    if (free && !d->yawned && d->quiet >= d->cfg.sleep_after_s - d->cfg.yawn_lead_s) {
        d->yawned = true;                  /* dozing off: a yawn first */
        start_act(d, CA_YAWN);
    } else if (free && d->convo_left > 0.f) {
        if ((d->convo_delay -= dt) <= 0.f) {   /* a say ended well: a happy flash */
            d->flash = CM_HAPPY; d->flash_left = FLASH_HAPPY_S;
            d->convo_left = 0.f; d->since_convo = 0.f;
            d->next_idle = idle_gap(d);
        }
    } else if (free && (d->next_idle -= dt) <= 0.f) {
        int a = idle_pick(d);
        if (a >= 0) { start_act(d, a); d->last_idle = a; }
        d->next_idle = idle_gap(d);
    }
    if (d->convo_left > 0.f && (d->convo_left -= dt) <= 0.f) d->convo_left = 0.f;

    if (d->status == d->shown) d->status_t = 0.f;
    else if (d->status == FD_STATUS_OK) { d->shown = FD_STATUS_OK; d->status_t = 0.f; }
    else if ((d->status_t += dt) >= d->cfg.status_after_s) { d->shown = d->status; d->status_t = 0.f; }
}

void fd_replay(fluid_director_t *d) { d->state_t = 0.f; d->text_done_t = 0.f; }

bool fd_take(fluid_director_t *d, int *st, int *fx) {
    if (!d->pending) return false;
    d->pending = false;
    d->cur = d->next; d->cur_fx = d->next_fx;
    d->state_t = 0.f; d->text_done_t = 0.f;
    if (st) *st = d->cur;
    if (fx) *fx = d->cur_fx;
    return true;
}

int fd_mood(const fluid_director_t *d) {
    return d->flash >= 0 ? d->flash : d->asleep ? CM_SLEEPY : d->held;
}

float fd_solidity(const fluid_director_t *d, float speed) {
    /* crystallise the shape once the flow had time to arrive (text condenses faster) */
    float q = d->cur == FD_ST_TEXT ? (d->state_t * speed - 0.2f) / 0.4f : (d->state_t * speed - 0.7f) / 1.0f;
    q = fluid_clamp01(q);
    return q * q * (3.f - 2.f * q);
}

int  fd_status(const fluid_director_t *d) { return d->shown; }
bool fd_asleep(const fluid_director_t *d) { return d->asleep; }

int fd_status_from(bool wifi, bool relay, bool auth_failed) {
    return !wifi ? FD_STATUS_WIFI : auth_failed ? FD_STATUS_AUTH : !relay ? FD_STATUS_RELAY : FD_STATUS_OK;
}
