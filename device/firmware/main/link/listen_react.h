/*
 * SPDX-License-Identifier: MIT
 *
 * listen_react.h — Molty reacts while the human speaks (no agent needed): a small nod at a
 * natural pause, a glance at a question, perking up when a voice starts after quiet, and an
 * attentive baseline (a blink, a small glance) so it never looks frozen while they talk.
 *
 * Plain C, no FreeRTOS / ESP-IDF: link_audio.c feeds it the mic level of every live 100 ms
 * chunk and the relay's hints (cap "hints": {"t":"hint","k":"pause"|"question"}, from the
 * voice box); test/host/react_test.c drives it. Each call returns the reaction to play now
 * (REACT_NONE most of the time); link_audio.c plays it as a one-shot action (cute_act.h).
 * Visual only — the mic is open — and subtle:
 *
 *   pause      box hint "pause"                                   → nod
 *              device: speech (level >= speech_level) for >= speech_ms, then quiet
 *              (level < quiet_level) for >= pause_ms               → nod, once per pause, and
 *              not when a box pause hint came within hint_dedupe_ms (the box judged it)
 *   question   box hint "question"                                → glance (left, right, …)
 *   perk       quiet for >= perk_quiet_ms, then level > perk_level → look up
 *   baseline   engaged listening: while a voice is heard (speech within beat_hold_ms) and
 *              nothing moved Molty for beat_ms (calm: calm_beat_ms) — counted from the voice's
 *              start, the last reaction, or the end of the relay's react — a micro-motion at
 *              the next dip in the voice (speech → below speech_level: the speech's rhythm),
 *              or beat_late_ms later without one: blink, glance left, blink, glance right, …
 *   external   the relay's reaction (a reflex / an agent's react: react_external) plays over
 *              the listening: nothing of ours starts during it, nor within ext_settle_ms after,
 *              so a peek or a bounce is never cut short by a nod.
 *   laughter   not built: no cheap detector tells laughter from speech reliably on 100 ms
 *              levels, and a wrong smile is worse than none.
 *   rules      nothing in the first warmup_ms of the listen; >= gap_ms between two
 *              reactions (calm idle life: calm_gap_ms); never with idle life off; never
 *              while something else owns the screen (`blocked`: a question, a pairing,
 *              an update). A reaction that may not play now is dropped, never queued.
 * Times are ms since the listen began.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { REACT_NONE = 0, REACT_NOD, REACT_GLANCE_LEFT, REACT_GLANCE_RIGHT, REACT_PERK, REACT_BLINK };
enum { REACT_HINT_PAUSE = 1, REACT_HINT_QUESTION = 2 };
/* idle life levels, as fluid_director.h FD_IDLE_* */
enum { REACT_IDLE_OFF = 0, REACT_IDLE_CALM = 1, REACT_IDLE_LIVELY = 2 };

typedef struct {
    uint32_t warmup_ms, gap_ms, calm_gap_ms;
    float    speech_level, quiet_level, perk_level;
    uint32_t speech_ms, pause_ms, perk_quiet_ms, hint_dedupe_ms;
    uint32_t beat_ms, calm_beat_ms, beat_hold_ms, beat_late_ms, ext_settle_ms;
} react_cfg_t;
#define REACT_CFG_DEFAULT { .warmup_ms = 1000, .gap_ms = 2500, .calm_gap_ms = 5000, \
    .speech_level = 0.5f, .quiet_level = 0.15f, .perk_level = 0.6f, \
    .speech_ms = 600, .pause_ms = 400, .perk_quiet_ms = 1000, .hint_dedupe_ms = 1500, \
    .beat_ms = 2500, .calm_beat_ms = 5000, .beat_hold_ms = 1200, .beat_late_ms = 1000, .ext_settle_ms = 500 }

typedef struct {
    react_cfg_t cfg;
    int      idle;
    bool     reacted;          /* any reaction yet                        */
    uint32_t last_at;          /* the last reaction                        */
    bool     hinted;           /* a box pause hint came                    */
    uint32_t hint_at;
    uint32_t last_t;           /* the previous level's time                */
    bool     have_t;
    uint32_t speech_run, quiet_run;
    bool     armed;            /* speech long enough: the next pause nods  */
    int      glance;           /* alternates left / right                  */
    uint32_t count;            /* reactions played (diag)                  */
    bool     voiced, heard;    /* the last level was speech; any speech yet */
    uint32_t voiced_at, voice0;/* the last speech level; the first one      */
    int      beat;             /* the baseline's next micro-motion          */
    bool     ext;              /* the relay reacted (react_external)        */
    uint32_t ext_until;        /* ...and its reaction plays until then      */
    uint32_t baselines;        /* baseline micro-motions played (diag)      */
} react_t;

/** cfg NULL = REACT_CFG_DEFAULT; idle = REACT_IDLE_*. */
void react_init(react_t *r, const react_cfg_t *cfg, int idle);
/** A live chunk's mic level (0..1, vad_level) at t_ms. */
int  react_level(react_t *r, float level, uint32_t t_ms, bool blocked);
/** A relay hint (REACT_HINT_*) at t_ms. */
int  react_hint(react_t *r, int hint, uint32_t t_ms, bool blocked);
/** The relay showed a reaction at t_ms (a reflex, an agent's react: an act or a react mood)
 *  that plays for hold_ms: ours wait for it (and ext_settle_ms more). */
void react_external(react_t *r, uint32_t t_ms, uint32_t hold_ms);
