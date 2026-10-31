/* listen_react.c on the host: when Molty reacts while the human speaks. */
#include "listen_react.h"
#include <assert.h>
#include <stdio.h>

/* levels every 100 ms from t: a string of 's' (speech 0.8), 'q' (quiet 0.05), 'm' (in between 0.3);
 * returns the first reaction (and its time) */
static int feed(react_t *r, uint32_t *t, const char *pat, bool blocked, uint32_t *at) {
    int out = REACT_NONE;
    for (const char *p = pat; *p; p++) {
        const float l = *p == 's' ? 0.8f : *p == 'q' ? 0.05f : 0.3f;
        int x = react_level(r, l, *t, blocked);
        if (x && !out) { out = x; if (at) *at = *t; }
        *t += 100;
    }
    return out;
}

int main(void) {
    react_t r; uint32_t t, at;
    /* a pause after speech: a nod once quiet for 400 ms */
    react_init(&r, NULL, REACT_IDLE_LIVELY); t = 1000;
    assert(feed(&r, &t, "ssssssqqqq", false, &at) == REACT_NOD && at == 1900);
    /* once per pause: more quiet, no second nod; speech too short (< 600 ms) does not arm it */
    assert(feed(&r, &t, "qqqqqqqqqqqqqqqqqqqqqqqqq", false, NULL) == REACT_NONE);
    react_init(&r, NULL, REACT_IDLE_LIVELY); t = 1000;
    assert(feed(&r, &t, "sssssqqqqqqq", false, NULL) == REACT_NONE);
    /* the first second: nothing (the speech before it still arms the pause after it) */
    react_init(&r, NULL, REACT_IDLE_LIVELY); t = 0;
    assert(feed(&r, &t, "sssssssqqq", false, NULL) == REACT_NONE);
    assert(feed(&r, &t, "q", false, &at) == REACT_NOD && at == 1000);
    react_init(&r, NULL, REACT_IDLE_LIVELY);
    assert(react_hint(&r, REACT_HINT_PAUSE, 500, false) == REACT_NONE && react_hint(&r, REACT_HINT_QUESTION, 999, false) == REACT_NONE);
    /* the gap: 2.5 s between reactions (calm: 5 s) */
    react_init(&r, NULL, REACT_IDLE_LIVELY); t = 1000;
    assert(feed(&r, &t, "ssssssqqqq", false, &at) == REACT_NOD && at == 1900);
    assert(react_hint(&r, REACT_HINT_QUESTION, 3000, false) == REACT_NONE);   /* 1.1 s later */
    assert(react_hint(&r, REACT_HINT_QUESTION, 4400, false) == REACT_GLANCE_LEFT);
    assert(react_hint(&r, REACT_HINT_QUESTION, 7000, false) == REACT_GLANCE_RIGHT);   /* alternates */
    react_init(&r, NULL, REACT_IDLE_CALM);
    assert(react_hint(&r, REACT_HINT_PAUSE, 2000, false) == REACT_NOD);
    assert(react_hint(&r, REACT_HINT_PAUSE, 5000, false) == REACT_NONE && react_hint(&r, REACT_HINT_PAUSE, 7000, false) == REACT_NOD);
    /* idle life off / an owner of the screen: never (and a blocked one is dropped, not queued) */
    react_init(&r, NULL, REACT_IDLE_OFF);
    assert(react_hint(&r, REACT_HINT_PAUSE, 3000, false) == REACT_NONE);
    t = 1000; assert(feed(&r, &t, "ssssssqqqqqqqq", false, NULL) == REACT_NONE);
    react_init(&r, NULL, REACT_IDLE_LIVELY);
    assert(react_hint(&r, REACT_HINT_PAUSE, 3000, true) == REACT_NONE);
    assert(react_hint(&r, REACT_HINT_QUESTION, 3100, false) == REACT_GLANCE_LEFT);   /* the blocked one never counted */
    /* the box's pause hint and the device's own pause: one nod, not two */
    react_init(&r, NULL, REACT_IDLE_LIVELY); t = 1000;
    assert(feed(&r, &t, "ssssssss", false, NULL) == REACT_NONE);
    assert(react_hint(&r, REACT_HINT_PAUSE, t, false) == REACT_NOD);
    assert(feed(&r, &t, "qqqqqqq", false, NULL) == REACT_NONE);
    /* a device pause within 1.5 s of the box's pause hint is the same pause: no nod (gap 0 here) */
    react_init(&r, NULL, REACT_IDLE_LIVELY);
    r.cfg.gap_ms = 0;
    assert(react_hint(&r, REACT_HINT_PAUSE, 1000, false) == REACT_NOD);
    t = 1000; assert(feed(&r, &t, "sssssssqqqq", false, NULL) == REACT_NONE);
    /* ...and a hint 1.5 s+ before does not suppress the device's next pause */
    react_init(&r, NULL, REACT_IDLE_LIVELY);
    r.cfg.gap_ms = 0;
    assert(react_hint(&r, REACT_HINT_PAUSE, 1000, false) == REACT_NOD);
    t = 1100; assert(feed(&r, &t, "ssssssssssssssssssssqqqq", false, &at) == REACT_NOD && at == 3400);
    /* perk: a voice after >= 1 s of quiet */
    react_init(&r, NULL, REACT_IDLE_LIVELY); t = 1000;
    assert(feed(&r, &t, "qqqqqqqqqq", false, NULL) == REACT_NONE);
    assert(feed(&r, &t, "s", false, &at) == REACT_PERK);
    react_init(&r, NULL, REACT_IDLE_LIVELY); t = 1000;
    assert(feed(&r, &t, "qqqqqqqqs", false, NULL) == REACT_NONE);                /* 900 ms quiet: no */
    react_init(&r, NULL, REACT_IDLE_LIVELY); t = 1000;
    assert(feed(&r, &t, "qqqqqqqqqqm", false, NULL) == REACT_NONE);              /* not loud enough */
    puts("REACT PASS: pause nod once per pause, 600 ms speech arms it, warmup 1 s, gap 2.5 s / calm 5 s, question glances alternate, idle off, owners drop, box/device pause dedupe, perk after 1 s quiet");
    return 0;
}
