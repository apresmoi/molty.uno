/*
 * SPDX-License-Identifier: MIT
 *
 * fluid_ask.c — the screen of a question: sectors, reading order, label layout
 * (fluid_ask.h). Plain C: built into the firmware and by test/host/ask_test.c.
 */
#include "fluid_ask.h"

#include <math.h>

#define TWO_PI 6.2831853f

/* label centres on the bisector, as a fraction of the radius, per option count */
static const float RADIUS_FRAC[FLUID_ASK_MAX + 1] = { 0.f, 0.f, 0.48f, 0.46f, 0.52f, 0.56f };

static int clamp_n(int n) { return n < 1 ? 1 : n > FLUID_ASK_MAX ? FLUID_ASK_MAX : n; }

/* relay/src/core/commands.ts COLORS: green, red, amber, blue, violet */
const uint32_t FLUID_ASK_COLORS[FLUID_ASK_MAX] = { 0x3ccf96, 0xff6b6b, 0xffa94d, 0x4dabf7, 0xb197fc };

uint32_t fluid_ask_color(int32_t given, int i) {
    if (given >= 0) return (uint32_t)given & 0xFFFFFFu;
    return FLUID_ASK_COLORS[i < 0 ? 0 : i % FLUID_ASK_MAX];
}

void fluid_ask_arc(int n, int i, int stage, float *a0, float *a1) {
    n = clamp_n(n);
    if (n == 1) { *a0 = 0.f; *a1 = TWO_PI; return; }
    int sector[FLUID_ASK_MAX];
    fluid_ask_order(n, sector);
    int k = sector[i < 0 ? 0 : i >= n ? n - 1 : i];
    /* the gap is in px along the arc: an angle at the arc's radius */
    float r = FLUID_ASK_ARC_R * (float)stage * 0.5f, g = FLUID_ASK_ARC_GAP * ((float)stage / 412.f) / r;
    *a0 = (float)k * TWO_PI / (float)n + g;
    *a1 = (float)(k + 1) * TWO_PI / (float)n - g;
}

void fluid_ask_arc_look(const fluid_ask_screen_t *s, int i, float speed, float *amt, float *hw) {
    *amt = 0.f; *hw = FLUID_ASK_ARC_HW;
    if (s->phase == FLUID_ASK_SHOW) {
        float q = (s->t * speed - 0.2f) / 0.4f;            /* the labels' hold ramp */
        q = q < 0.f ? 0.f : q > 1.f ? 1.f : q;
        *amt = q * q * (3.f - 2.f * q);
    } else if (s->phase == FLUID_ASK_PICK) {
        if (i == s->pick) {
            float b = 1.f - s->pick_t / FLUID_ASK_HOLD_S;  /* the flare settles over the hold */
            b = b < 0.f ? 0.f : b > 1.f ? 1.f : b;
            *amt = 1.f + 0.35f * b;
            *hw = FLUID_ASK_ARC_HW * (1.f + 0.7f * b);
        } else {
            float f = 1.f - s->pick_t * 2.f;               /* as the labels melt (0.8 at 0.5 s) */
            *amt = f < 0.f ? 0.f : f;
        }
    }
}

int fluid_ask_sector(int n, float dx, float dy) {
    n = clamp_n(n);
    if (n == 1 || (dx == 0.f && dy == 0.f)) return 0;
    /* clockwise from 12 o'clock, y down; + 0.f folds −0 into +0 so that straight up is 0 */
    float a = atan2f(dx + 0.f, -dy + 0.f);
    if (a < 0.f) a += TWO_PI;
    /* atan2f lands an ulp either side of a divider (straight down is 0.99999994 of
     * π): within 1e-5 of a turn (0.01 px at the rim) a point is ON the divider, and
     * belongs to the sector clockwise of it — 360° wraps to sector 0 */
    int k = (int)(a * (1.f / TWO_PI) * (float)n + 1e-5f);
    return k < 0 ? 0 : k % n;
}

void fluid_ask_order(int n, int sector[FLUID_ASK_MAX]) {
    n = clamp_n(n);
    float x[FLUID_ASK_MAX], y[FLUID_ASK_MAX];
    for (int k = 0; k < n; k++) {
        float a = ((float)k + 0.5f) * TWO_PI / (float)n;
        x[k] = sinf(a); y[k] = -cosf(a);
        sector[k] = k;
    }
    /* reading order of the sector centres: rows top to bottom (mirror pairs share a row
     * up to rounding), left to right within one */
    for (int i = 1; i < n; i++) for (int j = i; j > 0; j--) {
        int a = sector[j - 1], b = sector[j];
        bool later = fabsf(y[a] - y[b]) > 1e-3f ? y[a] > y[b] : x[a] > x[b];
        if (!later) break;
        sector[j - 1] = b; sector[j] = a;
    }
}

int fluid_ask_pick(int n, float dx, float dy) {
    n = clamp_n(n);
    int sector[FLUID_ASK_MAX], k = fluid_ask_sector(n, dx, dy);
    fluid_ask_order(n, sector);
    for (int i = 0; i < n; i++) if (sector[i] == k) return i;
    return 0;
}

bool fluid_ask_fits(int n, int k, int len, float fs, float x, float y, float r, float sc) {
    n = clamp_n(n);
    const float hw = (float)len * 0.6f * fs * 0.5f, hh = 1.05f * fs * 0.5f;
    const float rmax = r - FLUID_ASK_RIM * sc, gap = FLUID_ASK_GAP * sc, step = TWO_PI / (float)n;
    /* divider rays (sin a, −cos a) at the sector's two edges */
    const float a0 = (float)k * step, a1 = (float)(k + 1) * step;
    const float u0x = sinf(a0), u0y = -cosf(a0), u1x = sinf(a1), u1y = -cosf(a1);
    for (int c = 0; c < 4; c++) {
        float px = x + (c & 1 ? hw : -hw), py = y + (c & 2 ? hh : -hh);
        if (px * px + py * py > rmax * rmax) return false;
        if (n == 1) continue;
        /* clockwise of the first ray and anticlockwise of the second, by at least gap
         * (n = 2: both are the vertical line; n ≥ 3: a convex wedge) */
        if (u0x * py - u0y * px < gap) return false;
        if (-(u1x * py - u1y * px) < gap) return false;
    }
    return true;
}

float fluid_ask_layout(int n, const int *lens, int stage, float *cx, float *cy) {
    n = clamp_n(n);
    const float sc = (float)stage / 412.f, r = (float)stage * 0.5f, cap = FLUID_ASK_FS_MAX * sc;
    int sector[FLUID_ASK_MAX];
    fluid_ask_order(n, sector);
    float fs = cap;
    for (int i = 0; i < n; i++) {
        int k = sector[i];
        float a = ((float)k + 0.5f) * TWO_PI / (float)n, d = RADIUS_FRAC[n] * r;
        float x = n == 1 ? 0.f : sinf(a) * d, y = n == 1 ? 0.f : -cosf(a) * d;
        cx[i] = r + x; cy[i] = r + y;
        int len = lens[i] < 1 ? 1 : lens[i];
        /* the largest size that fits this label (a bigger box contains the smaller one) */
        float lo = 4.f * sc, hi = cap;
        if (!fluid_ask_fits(n, k, len, hi, x, y, r, sc)) {
            for (int it = 0; it < 24; it++) {
                float mid = 0.5f * (lo + hi);
                if (fluid_ask_fits(n, k, len, mid, x, y, r, sc)) lo = mid; else hi = mid;
            }
            hi = lo;
        }
        if (hi < fs) fs = hi;
    }
    /* a hair under the bisected limit: a box that touches the boundary must still fit
     * when its centre is recomputed from the stage px returned (float rounding) */
    return fs * 0.999f;
}

int fluid_ask_follow(fluid_ask_screen_t *s, bool active, unsigned seq, int pick, int n, bool requested, bool *keep) {
    int did = 0;
    *keep = false;
    if (s->phase != FLUID_ASK_NONE && requested) s->requested = true;
    if (active && seq != s->seq) {                         /* a new question replaces any */
        s->phase = FLUID_ASK_SHOW; s->seq = seq; s->pick = -1;
        s->t = s->pick_t = 0.f; s->requested = false;
        did |= FLUID_ASK_DID_SHOW;
    } else if (s->phase == FLUID_ASK_SHOW && (!active || seq != s->seq)) {   /* ended unanswered */
        s->phase = FLUID_ASK_NONE;
        *keep = s->requested;
        return did | FLUID_ASK_DID_LEAVE;
    }
    if (s->phase == FLUID_ASK_SHOW && pick >= 0) {         /* answered (maybe before it was drawn) */
        s->phase = FLUID_ASK_PICK; s->pick = pick < clamp_n(n) ? pick : 0; s->pick_t = 0.f;
        did |= FLUID_ASK_DID_PICK;
    }
    /* the chosen one stays a beat; an explicit request (the agent moved on) cuts it short */
    if (s->phase == FLUID_ASK_PICK && (s->pick_t >= FLUID_ASK_HOLD_S || requested)) {
        s->phase = FLUID_ASK_NONE;
        *keep = requested || s->requested;
        did |= FLUID_ASK_DID_LEAVE;
    }
    return did;
}

unsigned fluid_ask_tappable(const fluid_ask_screen_t *s, float speed) {
    return s->phase == FLUID_ASK_SHOW && s->t * speed >= FLUID_ASK_READY ? s->seq : 0u;
}
