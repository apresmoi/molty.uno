/*
 * SPDX-License-Identifier: MIT
 *
 * fluid_director.c — what the fluid shows, and the creature's idle life (fluid_director.h).
 * Plain C: built into the firmware and by test/host/director_test.c.
 */
#include "fluid_director.h"
#include "fluid_sim.h"      /* FLUID_FX_*, fluid_clamp01 */
#include "cute_spec.h"      /* CM_* moods */

#include <stddef.h>

const char *const FD_STATUS_NAMES[FD_STATUS_COUNT] = { "ok", "wifi", "relay", "auth" };

const fd_cfg_t FD_CFG_DEFAULT = {
    .flash_min_s = 20.f, .flash_max_s = 60.f,
    .sleep_after_s = 300.f,
    .text_return_s = 1.5f,
    .status_after_s = 10.f,       /* boot: Wi-Fi join + TLS take a few seconds */
};

/* idle flashes: happy twice as often as surprised */
#define FLASH_HAPPY_S      2.0f
#define FLASH_SURPRISED_S  1.2f   /* a shake, too: the pose needs ~0.5–1 s to blend in */
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

static float flash_gap(fluid_director_t *d) {
    return d->cfg.flash_min_s + (d->cfg.flash_max_s - d->cfg.flash_min_s) * frand(d);
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
    d->next_flash = flash_gap(d);
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
    d->flash = -1; d->asleep = false;
}

float fd_default_hold(int mood) {
    return mood == CM_HAPPY ? HOLD_HAPPY_S : mood == CM_SURPRISED ? HOLD_SURPRISED_S : -1.f;
}

void fd_activity(fluid_director_t *d) {
    d->asleep = false; d->quiet = 0.f;
    d->next_flash = flash_gap(d);          /* no random flash right after a command */
}

void fd_agent_mood(fluid_director_t *d, int mood, float hold_s) {
    if (mood < 0 || mood >= CUTE_N_MOODS) return;
    d->held = mood;
    d->held_left = mood == CM_IDLE ? -1.f : hold_s < 0.f ? fd_default_hold(mood) : hold_s == 0.f ? -1.f : hold_s;
    d->flash = -1;                         /* the agent's word wins over a flash */
    fd_activity(d);
}

void fd_user(fluid_director_t *d) { d->asleep = false; d->quiet = 0.f; }

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
        d->next_flash = flash_gap(d);
    }
    if (d->flash >= 0 && (d->flash_left -= dt) <= 0.f) d->flash = -1;

    /* idle life: only while the creature is on screen, idle, and nobody drives it */
    d->quiet += dt;
    if (!d->autoplay && !d->pending && d->cur == FD_ST_CREATURE && d->held == CM_IDLE) {
        if (!d->asleep && d->quiet >= d->cfg.sleep_after_s) { d->asleep = true; d->flash = -1; }
        if (!d->asleep && d->flash < 0 && (d->next_flash -= dt) <= 0.f) {
            bool happy = frand(d) < 2.f / 3.f;
            d->flash = happy ? CM_HAPPY : CM_SURPRISED;
            d->flash_left = happy ? FLASH_HAPPY_S : FLASH_SURPRISED_S;
            d->next_flash = flash_gap(d);
        }
    }

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
