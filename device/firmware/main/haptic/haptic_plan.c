/* SPDX-License-Identifier: MIT: haptic_plan.c (haptic_plan.h). */
#include "haptic_plan.h"
#include <string.h>

bool haptic_plan_pattern(haptic_plan_t *p, const int *segs, int n, int strength)
{
    if (n < 1 || n > HAPTIC_SEGS) return false;
    memset(p, 0, sizeof(*p));
    p->strength = (uint8_t)(strength < 0 ? 0 : strength > 100 ? 100 : strength);
    int left = HAPTIC_MAX_MS;
    bool on = false;
    for (int i = 0; i < n; i++) {
        if (segs[i] < 0) return false;
        const int ms = segs[i] < left ? segs[i] : left;
        if (!(i & 1) && ms > 0) on = true;
        p->seg[i] = (uint16_t)ms;
        left -= ms;
        p->n = i + 1;
        if (!left) break;                          /* capped: the rest is dropped */
    }
    return on;
}

bool haptic_plan_one(haptic_plan_t *p, int ms, int strength)
{
    return haptic_plan_pattern(p, &ms, 1, strength);
}

uint32_t haptic_plan_ms(const haptic_plan_t *p)
{
    uint32_t ms = 0;
    for (int i = 0; i < p->n; i++) ms += p->seg[i];
    return ms;
}

uint32_t haptic_plan_duty(const haptic_plan_t *p, int bits)
{
    return (uint32_t)p->strength * (1u << bits) / 100u;
}

int64_t haptic_plan_quiet_until(const haptic_plan_t *p, int64_t start_us)
{
    return start_us + ((int64_t)haptic_plan_ms(p) + HAPTIC_SETTLE_MS) * 1000;
}
