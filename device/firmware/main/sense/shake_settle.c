/* SPDX-License-Identifier: MIT — shake_settle.c (shake_settle.h). */
#include "shake_settle.h"
#include <math.h>
#include <string.h>

static const settle_cfg_t DEFAULT_CFG = SETTLE_CFG_DEFAULT;

void settle_init(settle_t *s, const settle_cfg_t *cfg, uint32_t t0_ms)
{
    memset(s, 0, sizeof(*s));
    s->cfg = cfg ? *cfg : DEFAULT_CFG;
    s->t0 = t0_ms;
}

static void cap(settle_t *s)
{
    s->done = s->capped = true;
    s->at = s->cfg.cap_ms;
}

bool settle_tick(settle_t *s, uint32_t now_ms)
{
    if (!s->done && (int32_t)(now_ms - s->t0) >= (int32_t)s->cfg.cap_ms) cap(s);
    return s->done;
}

bool settle_feed(settle_t *s, float ax, float ay, float az, uint32_t ms)
{
    if (s->done) return true;
    if ((int32_t)(ms - s->t0) < 0) return false;          /* a sample from before the listen */
    const float mag = sqrtf(ax * ax + ay * ay + az * az);
    const bool still = fabsf(mag - 1.f) < s->cfg.tol_g;
    if (!still) s->run = false;
    else if (!s->run) { s->run = true; s->run_from = ms; }
    if (s->run && ms - s->run_from >= s->cfg.still_ms) {
        const uint32_t at = s->run_from - s->t0 + s->cfg.still_ms;   /* where stillness is CONFIRMED */
        if (at >= s->cfg.cap_ms) cap(s);
        else { s->done = true; s->at = at; }
        return true;
    }
    return settle_tick(s, ms);
}
