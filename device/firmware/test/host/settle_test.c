/* shake_settle.c on the host: where a shake's listen begins (the handling noise is over). */
#include "shake_settle.h"
#include <assert.h>
#include <stdio.h>

/* 50 Hz samples from t0: shaken (3 g strokes) until `until`, then resting (1 g) */
static bool run(settle_t *s, uint32_t t0, int until, uint32_t to) {
    for (uint32_t t = 0; t <= to; t += 20) {
        const bool shaken = until < 0 || (int)t < until;
        if (settle_feed(s, shaken ? (t % 40 ? 3.f : -3.f) : 0.02f, 0.f, shaken ? 0.4f : 0.99f, t0 + t)) return true;
    }
    return false;
}

int main(void) {
    settle_t s;
    /* resting from the start: settled at 150 (stillness confirmed after 150 ms) */
    settle_init(&s, NULL, 1000);
    assert(!settle_feed(&s, 0, 0, 1, 1000) && !settle_feed(&s, 0, 0, 1, 1140));
    assert(settle_feed(&s, 0, 0, 1, 1160) && s.at == 150 && !s.capped);
    /* shaken for 350 ms: the still run starts at 360 → confirmed (settled) at 510, known at 520 */
    settle_init(&s, NULL, 5000);
    assert(run(&s, 5000, 350, 1000) && s.at == 510 && !s.capped);
    /* a still moment shorter than 150 ms does not settle it */
    settle_init(&s, NULL, 0);
    for (uint32_t t = 0; t < 100; t += 20) settle_feed(&s, 3, 0, 0, t);
    for (uint32_t t = 100; t < 200; t += 20) settle_feed(&s, 0, 0, 1, t);     /* 100 ms still */
    for (uint32_t t = 200; t < 300; t += 20) settle_feed(&s, 3, 0, 0, t);
    assert(!s.done);
    for (uint32_t t = 300; t <= 460; t += 20) settle_feed(&s, 0, 0, 1, t);
    assert(s.done && s.at == 450);
    /* shaken all along: capped at 600 ms (by a sample's clock, or by the tick) */
    settle_init(&s, NULL, 0);
    assert(run(&s, 0, -1, 2000) && s.at == 600 && s.capped);
    settle_init(&s, NULL, 0);
    assert(!settle_tick(&s, 599) && settle_tick(&s, 600) && s.at == 600 && s.capped);
    /* stillness confirmed past the cap (run from 460 → 610): the cap */
    settle_init(&s, NULL, 0);
    assert(run(&s, 0, 450, 1000) && s.at == 600 && s.capped);
    /* ...just inside it (run from 440 → 590): not capped */
    settle_init(&s, NULL, 0);
    assert(run(&s, 0, 430, 1000) && s.at == 590 && !s.capped);
    /* tolerance: 1.1 g is still, 1.2 g is not */
    settle_init(&s, NULL, 0);
    for (uint32_t t = 0; t <= 160; t += 20) settle_feed(&s, 0, 0, 1.1f, t);
    assert(s.done && s.at == 150);
    settle_init(&s, NULL, 0);
    for (uint32_t t = 0; t <= 300; t += 20) settle_feed(&s, 0, 0, 1.2f, t);
    assert(!s.done);
    /* samples from before the listen do not count; it is sticky */
    settle_init(&s, NULL, 1000);
    for (uint32_t t = 700; t < 1000; t += 20) assert(!settle_feed(&s, 0, 0, 1, t));
    assert(!s.done);
    assert(settle_feed(&s, 0, 0, 1, 1000) == false && settle_feed(&s, 0, 0, 1, 1160) && s.at == 150);
    assert(settle_feed(&s, 3, 3, 3, 1180) && s.at == 150);
    puts("SETTLE PASS: rest → 150, shaken 350 ms → 510 (confirmed, not the run start), short stillness ignored, cap 600 (sample and tick), tolerance, sticky");
    return 0;
}
