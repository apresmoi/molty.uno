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
static const float RADIUS_FRAC[FLUID_ASK_MAX + 1] = { 0.f, 0.f, 0.48f, 0.50f, 0.58f, 0.62f };

static int clamp_n(int n) { return n < 1 ? 1 : n > FLUID_ASK_MAX ? FLUID_ASK_MAX : n; }

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
    return fs;
}
