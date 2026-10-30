/*
 * SPDX-License-Identifier: MIT
 *
 * fluid_director.h — what the fluid shows, and what the creature does when nobody
 * drives it.
 *
 * Plain C, no FreeRTOS / ESP-IDF: the fluid task (mole_fluid.c) owns one director, feeds
 * it requests, events and time, and executes the transitions it hands out (fd_take);
 * test/host/director_test.c runs the same code.
 *
 *   presence     boot lands on the creature (the blob only while none is loaded).
 *                Re-requesting the shown state is a no-op, so a repeated mood or state
 *                never melts the creature; moods blend inside it. The demo reel runs
 *                only on an explicit "autoplay".
 *   idle life    the creature's own idle (blinks, glances, breathing: cute_rig.c) plus a
 *                short mood flash every 20–60 s. After 5 min without touch or agent
 *                activity it dozes off (sleepy); a touch, button, shake or command wakes it.
 *   agent moods  quick moods (happy, surprised) revert to idle after a hold; the others
 *                persist until the next command. hold_s overrides either.
 *   reader       "text" returns to the creature 1.5 s after its last word, unless the
 *                request held it (the pairing code, the Improv "hi").
 *   status       Wi-Fi / relay / auth trouble as a debounced indicator code for the
 *                screen's edge: never a state or mood change.
 *
 *            request(st)            last word + 1.5 s
 *   boot ──▶ CREATURE ──────────▶ TEXT ─────────────────┐
 *            ▲   │ mood: blend,        (hold: stays)     │
 *            │   │ flash 20–60 s, sleepy after 5 min     │
 *            └───┴───────────────────────────────────────┘
 *   "autoplay" only: the reel (blob ▸ creature ▸ effects ▸ text) until any other request.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* States, in mole_fluid.c's order (its per-state relax rates are indexed by them). */
enum { FD_ST_BLOB, FD_ST_THINKING, FD_ST_TEXT, FD_ST_FX, FD_ST_CREATURE, FD_ST_COUNT };

/* Status indicator codes (fd_status), worst first in fd_status_from. */
enum { FD_STATUS_OK, FD_STATUS_WIFI, FD_STATUS_RELAY, FD_STATUS_AUTH, FD_STATUS_COUNT };
extern const char *const FD_STATUS_NAMES[FD_STATUS_COUNT];   /* "ok" "wifi" "relay" "auth" */

/* Tunables (seconds). FD_CFG_DEFAULT holds the shipped values. */
typedef struct {
    float flash_min_s, flash_max_s;   /* idle gap between two mood flashes: 20..60     */
    float sleep_after_s;              /* no touch / agent activity this long: sleepy   */
    float text_return_s;              /* reader: last word → back to the creature      */
    float status_after_s;             /* trouble must last this long before it shows   */
} fd_cfg_t;
extern const fd_cfg_t FD_CFG_DEFAULT;

typedef struct {
    fd_cfg_t cfg;
    uint32_t rng;                     /* xorshift32, seeded: host tests are repeatable */
    bool  has_creature;
    int   cur, cur_fx;                /* the state on screen                           */
    bool  pending;                    /* a transition to (next, next_fx) awaits fd_take */
    int   next, next_fx;
    bool  want_creature;              /* asked for the creature while none was loaded  */
    float state_t;                    /* s since cur was entered: the shape's solidity */
    bool  text_hold;                  /* text: hold the last word (no return)          */
    float text_done_t;                /* s the reader has held its last word           */
    bool  autoplay;
    int   reel;                       /* reel step, -1 = restart                       */
    float reel_t;
    int   held;                       /* the agent's mood (CM_*)                       */
    float held_left;                  /* s until it reverts to idle; < 0: until the next command */
    int   flash;                      /* short overlay mood, -1 = none                 */
    float flash_left;
    float next_flash;                 /* s of idle life until the next flash           */
    float quiet;                      /* s since the last touch / agent activity       */
    bool  asleep;
    int   status, shown;              /* reported / on screen                          */
    float status_t;                   /* s the reported status has differed from shown */
} fluid_director_t;

/** Boot: autoplay off, the creature wanted (shown once fd_set_creature(true)). cfg NULL =
 *  FD_CFG_DEFAULT. */
void  fd_init(fluid_director_t *d, const fd_cfg_t *cfg, uint32_t seed);
/** A creature (.molty) is (not) loaded. Loading one while the creature was wanted but the
 *  blob stood in for it moves to the creature. */
void  fd_set_creature(fluid_director_t *d, bool loaded);
/** An explicit state (HTTP / link). Ends autoplay. Re-requesting the shown (or already
 *  pending) state is a no-op; "text" always restarts the reader. text_hold: text only. */
void  fd_request_state(fluid_director_t *d, int st, int fx, bool text_hold);
/** The demo reel, from its first step; any fd_request_state ends it. */
void  fd_autoplay(fluid_director_t *d);
/** The agent's mood. hold_s < 0: the mood's default (fd_default_hold), 0: until the next
 *  command, > 0: that many seconds, then idle. Counts as agent activity. */
void  fd_agent_mood(fluid_director_t *d, int mood, float hold_s);
/** Agent activity without a mood (show, state, ...): wakes, restarts the quiet timers. */
void  fd_activity(fluid_director_t *d);
/** A touch or a button: wakes, restarts the sleep timer. */
void  fd_user(fluid_director_t *d);
/** The board was shaken: wakes, a brief surprised flash (over any mood). */
void  fd_shake(fluid_director_t *d);
/** Report the link status (FD_STATUS_*); shown after status_after_s, hidden at once. */
void  fd_set_status(fluid_director_t *d, int status);
/** Advance by dt seconds. text_done: the reader is holding its last word. */
void  fd_tick(fluid_director_t *d, float dt, bool text_done);
/** The transition to execute now, if any (resets the state clock). */
bool  fd_take(fluid_director_t *d, int *st, int *fx);

/** The mood the creature shows: a flash, else sleepy while asleep, else the held mood. */
int   fd_mood(const fluid_director_t *d);
/** 0..1: how settled (crystallised) the shown shape is; speed = the fluid's speed. */
float fd_solidity(const fluid_director_t *d, float speed);
int   fd_status(const fluid_director_t *d);
bool  fd_asleep(const fluid_director_t *d);
/** Default hold of an agent mood: happy / surprised revert, the rest persist (< 0). */
float fd_default_hold(int mood);
/** The indicator for a link state: no Wi-Fi > auth failed > relay offline > ok. */
int   fd_status_from(bool wifi, bool relay, bool auth_failed);

#ifdef __cplusplus
}
#endif
