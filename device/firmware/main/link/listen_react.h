/*
 * SPDX-License-Identifier: MIT
 *
 * listen_react.h — Molty reacts while the human speaks (no agent needed): a small nod at a
 * natural pause, a glance at a question, perking up when a voice starts after quiet.
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

enum { REACT_NONE = 0, REACT_NOD, REACT_GLANCE_LEFT, REACT_GLANCE_RIGHT, REACT_PERK };
enum { REACT_HINT_PAUSE = 1, REACT_HINT_QUESTION = 2 };
/* idle life levels, as fluid_director.h FD_IDLE_* */
enum { REACT_IDLE_OFF = 0, REACT_IDLE_CALM = 1, REACT_IDLE_LIVELY = 2 };

typedef struct {
    uint32_t warmup_ms, gap_ms, calm_gap_ms;
    float    speech_level, quiet_level, perk_level;
    uint32_t speech_ms, pause_ms, perk_quiet_ms, hint_dedupe_ms;
} react_cfg_t;
#define REACT_CFG_DEFAULT { .warmup_ms = 1000, .gap_ms = 2500, .calm_gap_ms = 5000, \
    .speech_level = 0.5f, .quiet_level = 0.15f, .perk_level = 0.6f, \
    .speech_ms = 600, .pause_ms = 400, .perk_quiet_ms = 1000, .hint_dedupe_ms = 1500 }

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
} react_t;

/** cfg NULL = REACT_CFG_DEFAULT; idle = REACT_IDLE_*. */
void react_init(react_t *r, const react_cfg_t *cfg, int idle);
/** A live chunk's mic level (0..1, vad_level) at t_ms. */
int  react_level(react_t *r, float level, uint32_t t_ms, bool blocked);
/** A relay hint (REACT_HINT_*) at t_ms. */
int  react_hint(react_t *r, int hint, uint32_t t_ms, bool blocked);
