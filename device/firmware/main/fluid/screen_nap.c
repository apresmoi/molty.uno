/*
 * SPDX-License-Identifier: MIT
 *
 * screen_nap.c: screen_nap.h.
 */
#include "screen_nap.h"
#include <math.h>
#include <string.h>

bool nap_valid_s(long s) { return s == 0 || (s >= NAP_MIN_S && s <= NAP_MAX_S); }

bool nap_tick(nap_t *n, uint32_t now_ms, bool asleep, bool busy, int power, uint32_t battery_s, uint32_t usb_s)
{
    const uint32_t s = power == NAP_POWER_BATTERY ? battery_s : power == NAP_POWER_USB ? usb_s : 0;
    if (!asleep || busy || s == 0) { n->counting = false; return false; }
    if (!n->counting) { n->counting = true; n->since_ms = now_ms; return false; }
    if (now_ms - n->since_ms < s * 1000u) return false;
    n->counting = false;
    n->napping = true;
    return true;
}

void nap_on(nap_t *n) { n->counting = false; n->napping = false; }

void nap_hand_off(nap_t *n) { n->napping = false; }

bool nap_wakes(const nap_t *n, nap_wake_t why) { return why < NAP_WAKE_LIFT || n->napping; }

void nap_lift_reset(nap_lift_t *l) { memset(l, 0, sizeof(*l)); }

bool nap_lift_feed(nap_lift_t *l, float ax, float ay, float az, uint32_t ms)
{
    const float a[3] = { ax, ay, az };
    if (!l->primed) { memcpy(l->g, a, sizeof(a)); l->primed = true; l->t0 = ms; return false; }
    for (int i = 0; i < 3; i++) l->g[i] += NAP_LIFT_ALPHA * (a[i] - l->g[i]);
    if (!l->ref_ok) {
        if (ms - l->t0 >= NAP_LIFT_SETTLE_MS) { memcpy(l->ref, l->g, sizeof(l->g)); l->ref_ok = true; }
        return false;
    }
    const float gg = sqrtf(l->g[0] * l->g[0] + l->g[1] * l->g[1] + l->g[2] * l->g[2]);
    const float rr = sqrtf(l->ref[0] * l->ref[0] + l->ref[1] * l->ref[1] + l->ref[2] * l->ref[2]);
    const float dot = l->g[0] * l->ref[0] + l->g[1] * l->ref[1] + l->g[2] * l->ref[2];
    /* (a gravity far from 1 g, falling or thrown, says nothing of the way it faces) */
    const bool tilt = gg > 0.5f && rr > 0.5f && dot < gg * rr * cosf(NAP_LIFT_DEG * 0.017453293f);
    if (!tilt) { l->tilted = false; return false; }
    if (!l->tilted) { l->tilted = true; l->tilt_ms = ms; }
    return ms - l->tilt_ms >= NAP_LIFT_HOLD_MS;
}
