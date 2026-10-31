/* SPDX-License-Identifier: MIT — listen_react.c (listen_react.h). */
#include "listen_react.h"
#include <string.h>

static const react_cfg_t DEFAULT_CFG = REACT_CFG_DEFAULT;

void react_init(react_t *r, const react_cfg_t *cfg, int idle)
{
    memset(r, 0, sizeof(*r));
    r->cfg = cfg ? *cfg : DEFAULT_CFG;
    r->idle = idle;
}

/* May a reaction play at t? Plays it (bookkeeping) when so. */
static int play(react_t *r, int what, uint32_t t, bool blocked)
{
    if (what == REACT_NONE || r->idle == REACT_IDLE_OFF || blocked || t < r->cfg.warmup_ms) return REACT_NONE;
    const uint32_t gap = r->idle == REACT_IDLE_CALM ? r->cfg.calm_gap_ms : r->cfg.gap_ms;
    if (r->reacted && t - r->last_at < gap) return REACT_NONE;
    r->reacted = true; r->last_at = t; r->count++;
    return what;
}

int react_level(react_t *r, float level, uint32_t t, bool blocked)
{
    const uint32_t dt = r->have_t && t > r->last_t ? t - r->last_t : 100;
    r->last_t = t; r->have_t = true;
    int what = REACT_NONE;
    if (level >= r->cfg.speech_level) {
        /* a voice after a long quiet: perk up */
        if (r->quiet_run >= r->cfg.perk_quiet_ms && level > r->cfg.perk_level) what = REACT_PERK;
        r->speech_run += dt; r->quiet_run = 0;
        if (r->speech_run >= r->cfg.speech_ms) r->armed = true;
    } else if (level < r->cfg.quiet_level) {
        r->quiet_run += dt; r->speech_run = 0;
        if (r->armed && r->quiet_run >= r->cfg.pause_ms) {
            r->armed = false;                     /* once per pause: speech must re-arm it */
            const bool box_had_it = r->hinted && t - r->hint_at < r->cfg.hint_dedupe_ms;
            if (!box_had_it) what = REACT_NOD;
        }
    } else {
        r->speech_run = 0;                        /* in between: neither run grows */
    }
    return play(r, what, t, blocked);
}

int react_hint(react_t *r, int hint, uint32_t t, bool blocked)
{
    if (hint == REACT_HINT_PAUSE) {
        r->hinted = true; r->hint_at = t;
        r->armed = false;                         /* this pause is the box's: no second nod */
        return play(r, REACT_NOD, t, blocked);
    }
    if (hint == REACT_HINT_QUESTION) {
        const int g = r->glance ? REACT_GLANCE_RIGHT : REACT_GLANCE_LEFT;
        const int out = play(r, g, t, blocked);
        if (out != REACT_NONE) r->glance ^= 1;
        return out;
    }
    return REACT_NONE;
}
