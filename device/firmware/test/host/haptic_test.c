/* haptic_plan.c on the host: a buzz's segments, its cap, its duty, the named patterns and the shake gate's quiet window. */
#include "haptic_plan.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    haptic_plan_t p;
    /* one buzz: one on segment; strength clamped */
    assert(haptic_plan_one(&p, 300, 100) && p.n == 1 && p.seg[0] == 300 && haptic_plan_ms(&p) == 300);
    assert(haptic_plan_one(&p, 300, 250) && p.strength == 100);
    assert(haptic_plan_one(&p, 300, -5) && p.strength == 0);
    assert(!haptic_plan_one(&p, 0, 100) && !haptic_plan_one(&p, -10, 100));
    /* the cap: one buzz at most HAPTIC_MAX_MS */
    assert(haptic_plan_one(&p, 60000, 100) && haptic_plan_ms(&p) == HAPTIC_MAX_MS);
    /* a double tap */
    const int tap2[] = { 80, 60, 80 };
    assert(haptic_plan_pattern(&p, tap2, 3, 70) && p.n == 3 && p.seg[1] == 60 && haptic_plan_ms(&p) == 220);
    /* a pattern over the cap: the crossing segment cut, the rest dropped */
    const int longp[] = { 900, 900, 900, 900 };
    assert(haptic_plan_pattern(&p, longp, 4, 100) && p.n == 3 && p.seg[2] == 200 && haptic_plan_ms(&p) == HAPTIC_MAX_MS);
    /* rejected: too many segments, none, a negative one, nothing on */
    const int nine[9] = { 10, 10, 10, 10, 10, 10, 10, 10, 10 }, neg[] = { 80, -1, 80 }, off[] = { 0, 500 };
    assert(!haptic_plan_pattern(&p, nine, 9, 100) && !haptic_plan_pattern(&p, nine, 0, 100));
    assert(!haptic_plan_pattern(&p, neg, 3, 100) && !haptic_plan_pattern(&p, off, 2, 100));
    /* duty: 100 is always on (1 << bits), 0 is off */
    assert(haptic_plan_one(&p, 10, 100) && haptic_plan_duty(&p, 10) == 1024);
    assert(haptic_plan_one(&p, 10, 50) && haptic_plan_duty(&p, 10) == 512);
    assert(haptic_plan_one(&p, 10, 0) && haptic_plan_duty(&p, 10) == 0);
    /* the shake gate: deaf from the start until HAPTIC_SETTLE_MS after the end, not a us longer */
    assert(HAPTIC_SETTLE_MS == 250);
    assert(haptic_plan_pattern(&p, tap2, 3, 100));
    const int64_t t0 = 5000000, until = haptic_plan_quiet_until(&p, t0);
    assert(until == t0 + (220 + 250) * 1000);
    const int64_t end = t0 + 220 * 1000;
    assert(end + 249 * 1000 < until && !(end + 250 * 1000 < until));
    /* a capped buzz is quiet for the cap plus the settle, never for what was asked */
    assert(haptic_plan_one(&p, 60000, 100) && haptic_plan_quiet_until(&p, 0) == (HAPTIC_MAX_MS + 250) * 1000);
    /* the named patterns: these four, each resolves, something on, under HAPTIC_NAMED_MAX_MS; strength passes through */
    const char *const names[] = { "tick", "tap", "double", "nudge" };
    int named = 0;
    for (const char *n; (n = haptic_plan_name(named)); named++) {
        assert(named < 4 && !strcmp(n, names[named]));
        assert(haptic_plan_named(&p, n, 40) && p.strength == 40);
        assert(haptic_plan_ms(&p) > 0 && haptic_plan_ms(&p) < HAPTIC_NAMED_MAX_MS);
    }
    assert(named == 4 && !haptic_plan_name(-1));
    assert(haptic_plan_named(&p, "double", 100) && p.n == 3 && p.seg[0] == 60 && p.seg[1] == 90 && p.seg[2] == 60);
    assert(haptic_plan_named(&p, "tick", 100) && haptic_plan_ms(&p) <= 40);   /* a tick, not a buzz */
    /* a wheel's click: its own, firmer than the touch buzz yet a tick (under 30 ms); a dial's own ms and strength pass
     * through; a long turn's ticks keep the shake gate deaf between them (one every 60 ms at most: 22 + 250 > 60) */
    assert(haptic_plan_wheel(&p, 0, 0) && p.n == 1 && p.seg[0] == HAPTIC_WHEEL_MS && p.strength == HAPTIC_WHEEL_STRENGTH);
    assert(HAPTIC_WHEEL_MS > HAPTIC_TOUCH_MS && HAPTIC_WHEEL_MS < 30 && HAPTIC_WHEEL_STRENGTH > HAPTIC_TOUCH_STRENGTH);
    assert(haptic_plan_wheel(&p, 18, 95) && p.seg[0] == 18 && p.strength == 95);
    assert(haptic_plan_wheel(&p, 0, 0) && haptic_plan_quiet_until(&p, 0) > 60 * 1000);
    /* unknown: refused, whatever its case */
    assert(!haptic_plan_named(&p, "bogus", 100) && !haptic_plan_named(&p, "Double", 100) && !haptic_plan_named(&p, "", 100));
    assert(!haptic_plan_named(&p, NULL, 100));
    printf("haptic_test: ok\n");
    return 0;
}
