/* SPDX-License-Identifier: MIT — see ring_input.h */
#include "ring_input.h"
#include <math.h>

static float angle_of(float x, float y) { return atan2f(y, x) * 57.29578f; }

bool ring_grabs(float r0, float x, float y) { return r0 > 0.f && x * x + y * y >= r0 * r0; }

void ring_down(ring_drag_t *d, float x, float y, float t) {
    *d = (ring_drag_t){ .down = true, .a = angle_of(x, y), .t = t };
}

int ring_move(ring_drag_t *d, float step, float x, float y, float t, bool *fast) {
    *fast = d->dps > RING_FAST_DPS;
    if (!d->down || step <= 0.f || x * x + y * y < RING_MIN_R * RING_MIN_R) return 0;
    const float a = angle_of(x, y);
    float da = a - d->a;                                 /* counter-clockwise, -180..180 */
    da -= 360.f * floorf((da + 180.f) / 360.f);
    d->a = a;
    const float dt = t - d->t;
    d->t = t;
    if (dt > 0.f) d->dps += (fabsf(da) / dt - d->dps) * fminf(1.f, dt / 0.08f);   /* ~80 ms smoothing */
    *fast = d->dps > RING_FAST_DPS;
    d->acc -= da; d->turned -= da;
    const int clicks = (int)(d->acc / step);             /* towards zero: what is left carries on */
    d->acc -= clicks * step;
    return clicks;
}

void ring_up(ring_drag_t *d) { d->down = false; d->dps = 0.f; d->acc = 0.f; }
