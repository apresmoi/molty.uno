/*
 * SPDX-License-Identifier: MIT
 *
 * gesture.c — shake detection + event rate limit (gesture.h). Plain C: built into the
 * firmware and by test/host/gesture_test.c.
 */
#include "gesture.h"

#include <math.h>
#include <string.h>

const gesture_cfg_t GESTURE_CFG_DEFAULT = {
    .alpha = 0.1f, .peak_g = 1.2f, .rearm_g = 0.6f,
    .reversals = 3, .window_ms = 800, .min_gap_ms = 50, .cooldown_ms = 2000,
};

void gesture_init(gesture_t *g, const gesture_cfg_t *cfg) {
    memset(g, 0, sizeof(*g));
    g->cfg = cfg ? *cfg : GESTURE_CFG_DEFAULT;
}

void gesture_reset(gesture_t *g) {
    gesture_cfg_t cfg = g->cfg;
    gesture_init(g, &cfg);
}

bool gesture_feed(gesture_t *g, float ax, float ay, float az, uint32_t ms, gesture_shake_t *out) {
    const float a[3] = { ax, ay, az };
    if (!isfinite(ax) || !isfinite(ay) || !isfinite(az)) return false;
    if (!g->primed) { memcpy(g->g, a, sizeof(a)); g->primed = true; return false; }
    float d[3], mag2 = 0.f;
    int axis = 0;
    for (int i = 0; i < 3; i++) {
        g->g[i] += g->cfg.alpha * (a[i] - g->g[i]);           /* gravity: a slow low-pass */
        d[i] = a[i] - g->g[i];
        mag2 += d[i] * d[i];
        if (fabsf(d[i]) > fabsf(d[axis])) axis = i;
    }
    const float mag = sqrtf(mag2);
    if (g->cooling) {
        if ((int32_t)(ms - g->cool_until) < 0) { g->in_peak = false; return false; }
        g->cooling = false;
    }

    bool start;
    if (g->in_peak) {
        gesture_peak_t *p = &g->peaks[g->npeaks - 1];
        start = false;
        if (mag < g->cfg.rearm_g) g->in_peak = false;
        else if (mag > g->cfg.peak_g && d[p->axis] * (float)p->sign < 0.f) start = true;   /* flipped mid-stroke */
        else if (mag > p->mag) p->mag = mag;
    } else start = mag > g->cfg.peak_g;
    if (!start) return false;

    g->in_peak = true;
    if (g->npeaks) {                                          /* ringing faster than a hand: same stroke */
        gesture_peak_t *last = &g->peaks[g->npeaks - 1];
        if (ms - last->ms < g->cfg.min_gap_ms) { if (mag > last->mag) last->mag = mag; return false; }
    }
    int keep = 0;                                             /* drop peaks that left the window */
    for (int k = 0; k < g->npeaks; k++)
        if (ms - g->peaks[k].ms <= g->cfg.window_ms) g->peaks[keep++] = g->peaks[k];
    g->npeaks = keep;
    if (g->npeaks == GESTURE_PEAKS) { memmove(g->peaks, g->peaks + 1, sizeof(g->peaks[0]) * (GESTURE_PEAKS - 1)); g->npeaks--; }
    g->peaks[g->npeaks++] = (gesture_peak_t){ ms, mag, (int8_t)axis, (int8_t)(d[axis] >= 0.f ? 1 : -1) };

    /* the alternating chain that ends at this peak, on one axis */
    int rev = 0;
    float pk = mag;
    uint32_t first = ms;
    for (int k = g->npeaks - 1; k > 0; k--) {
        const gesture_peak_t *p0 = &g->peaks[k - 1], *p1 = &g->peaks[k];
        if (p0->axis != p1->axis || p0->sign == p1->sign) break;
        rev++; first = p0->ms;
        if (p0->mag > pk) pk = p0->mag;
    }
    if (rev < g->cfg.reversals) return false;
    if (out) *out = (gesture_shake_t){ pk, rev, ms - first };
    g->npeaks = 0; g->in_peak = false;
    g->cooling = true; g->cool_until = ms + g->cfg.cooldown_ms;
    return true;
}

bool gesture_limit_allow(gesture_limit_t *l, uint32_t now_ms) {
    if (l->used && now_ms - l->last_ms < l->min_ms) return false;
    l->used = true; l->last_ms = now_ms;
    return true;
}
