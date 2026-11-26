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

bool haptic_plan_wheel(haptic_plan_t *p, int ms, int strength)
{
    return haptic_plan_one(p, ms > 0 ? ms : HAPTIC_WHEEL_MS, strength > 0 ? strength : HAPTIC_WHEEL_STRENGTH);
}

bool haptic_wheel_due(uint32_t *last_ms, uint32_t now_ms)
{
    if (*last_ms && now_ms - *last_ms < HAPTIC_WHEEL_GAP_MS) return false;
    *last_ms = now_ms ? now_ms : 1;
    return true;
}

static const struct { const char *name; int n; int seg[3]; } NAMED[] = {
    { "tick",   1, { 30 } },
    { "tap",    1, { 60 } },
    { "double", 3, { 60, 90, 60 } },
    { "nudge",  1, { 350 } },
};
#define N_NAMED ((int)(sizeof(NAMED) / sizeof(NAMED[0])))

bool haptic_plan_named(haptic_plan_t *p, const char *name, int strength)
{
    for (int i = 0; name && i < N_NAMED; i++)
        if (!strcmp(name, NAMED[i].name)) return haptic_plan_pattern(p, NAMED[i].seg, NAMED[i].n, strength);
    return false;
}

const char *haptic_plan_name(int i) { return i >= 0 && i < N_NAMED ? NAMED[i].name : NULL; }

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
