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

/* every reaction over a pattern (repeated `reps` times): kinds and times */
#define LOG_N 64
static int s_n, s_what[LOG_N]; static uint32_t s_at[LOG_N];
static void collect(react_t *r, uint32_t *t, const char *pat, int reps, bool blocked) {
    for (int k = 0; k < reps; k++)
        for (const char *p = pat; *p; p++) {
            const float l = *p == 's' ? 0.8f : *p == 'q' ? 0.05f : 0.3f;
            int x = react_level(r, l, *t, blocked);
            if (x && s_n < LOG_N) { s_what[s_n] = x; s_at[s_n++] = *t; }
            *t += 100;
        }
}
static uint32_t longest_still(uint32_t from, uint32_t to) {
    uint32_t prev = from, worst = 0;
    for (int i = 0; i < s_n; i++) { if (s_at[i] - prev > worst) worst = s_at[i] - prev; prev = s_at[i]; }
    return to - prev > worst ? to - prev : worst;
}

/* the attentive baseline: micro-motions tied to the voice's rhythm, never frozen while it talks */
static void test_baseline(void) {
    react_t r; uint32_t t;
    /* rhythmic speech ("sss" + a dip): no pause nods (never 600 ms of voice), but a blink at the
     * first dip 2.5 s into the voice, then glance left, blink, glance right — one per beat */
    react_init(&r, NULL, REACT_IDLE_LIVELY); t = 1000; s_n = 0;
    collect(&r, &t, "sssm", 50, false);                         /* 20 s */
    assert(s_n >= 6 && s_what[0] == REACT_BLINK && s_at[0] == 3700);
    assert(s_what[1] == REACT_GLANCE_LEFT && s_what[2] == REACT_BLINK && s_what[3] == REACT_GLANCE_RIGHT && s_what[4] == REACT_BLINK);
    for (int i = 0; i < s_n; i++) assert(s_at[i] % 400 == 100);  /* always on a dip (the 'm' of each "sssm") */
    for (int i = 1; i < s_n; i++) assert(s_at[i] - s_at[i - 1] >= r.cfg.beat_ms && s_at[i] - s_at[i - 1] <= r.cfg.beat_ms + 400);
    assert(longest_still(1000, t) <= r.cfg.beat_ms + 400 + 300 && r.baselines == (uint32_t)s_n);
    /* a voice without dips: the beat comes beat_late_ms later anyway */
    react_init(&r, NULL, REACT_IDLE_LIVELY); t = 1000; s_n = 0;
    collect(&r, &t, "s", 60, false);
    assert(s_n >= 1 && s_what[0] == REACT_BLINK && s_at[0] == 1000 + r.cfg.beat_ms + r.cfg.beat_late_ms);
    /* calm idle life: a slower beat; off: none; a screen owner: none; silence: none (no voice heard) */
    react_init(&r, NULL, REACT_IDLE_CALM); t = 1000; s_n = 0;
    collect(&r, &t, "sssm", 50, false);
    assert(s_n >= 3 && s_at[0] == 6100 && s_at[1] - s_at[0] >= r.cfg.calm_beat_ms);
    react_init(&r, NULL, REACT_IDLE_OFF); t = 1000; s_n = 0;
    collect(&r, &t, "sssm", 50, false); assert(s_n == 0);
    react_init(&r, NULL, REACT_IDLE_LIVELY); t = 1000; s_n = 0;
    collect(&r, &t, "sssm", 50, true); assert(s_n == 0);
    react_init(&r, NULL, REACT_IDLE_LIVELY); t = 1000; s_n = 0;
    collect(&r, &t, "q", 200, false); assert(s_n == 0);
    /* the voice stopped (> beat_hold_ms ago): no more beats */
    react_init(&r, NULL, REACT_IDLE_LIVELY); t = 1000; s_n = 0;
    collect(&r, &t, "sssm", 4, false); collect(&r, &t, "m", 100, false);
    assert(s_n == 0);
    /* a real reaction (the pause nod) resets the beat: the next micro-motion a beat after it */
    react_init(&r, NULL, REACT_IDLE_LIVELY); t = 1000; s_n = 0;
    collect(&r, &t, "ssssssqqqq", 1, false); collect(&r, &t, "sssm", 20, false);
    assert(s_n >= 2 && s_what[0] == REACT_NOD && s_at[0] == 1900 && s_what[1] == REACT_BLINK && s_at[1] >= 1900 + r.cfg.beat_ms);
}

/* the relay's reaction (a reflex, an agent's react) plays over the listening: never cut short */
static void test_external(void) {
    react_t r; uint32_t t;
    /* a peek at 3.0 s (2.2 s long): the blink due at 3.7 s waits; nothing until it ended + settle,
     * and the next beat counts from its end */
    react_init(&r, NULL, REACT_IDLE_LIVELY); t = 1000; s_n = 0;
    collect(&r, &t, "sssm", 5, false);                           /* to 3.0 s */
    react_external(&r, 3000, 2200);
    collect(&r, &t, "sssm", 40, false);
    assert(s_n >= 1 && s_at[0] >= 3000 + 2200 + r.cfg.beat_ms && s_what[0] == REACT_BLINK);
    /* a pause nod or a box hint during it: dropped (not queued) */
    react_init(&r, NULL, REACT_IDLE_LIVELY); t = 1000; s_n = 0;
    react_external(&r, 1000, 2200);
    collect(&r, &t, "ssssssqqqq", 1, false);                      /* the nod would be at 1.9 s */
    assert(s_n == 0);
    assert(react_hint(&r, REACT_HINT_QUESTION, 3000, false) == REACT_NONE);
    assert(react_hint(&r, REACT_HINT_QUESTION, 3200 + r.cfg.ext_settle_ms - 1, false) == REACT_NONE);
    assert(react_hint(&r, REACT_HINT_QUESTION, 3200 + r.cfg.ext_settle_ms, false) == REACT_GLANCE_LEFT);
    /* a second, longer one extends the hold; a shorter one never shortens it */
    react_init(&r, NULL, REACT_IDLE_LIVELY);
    react_external(&r, 2000, 2200); react_external(&r, 2100, 300);
    assert(react_hint(&r, REACT_HINT_PAUSE, 4300, false) == REACT_NONE && react_hint(&r, REACT_HINT_PAUSE, 4700, false) == REACT_NOD);
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
    test_baseline(); test_external();
    puts("REACT PASS: baseline micro-motions on the voice's dips (lively 2.5 s, calm 5 s, never frozen, off / owners / silence: none), the relay's reactions never cut short; pause nod once per pause, 600 ms speech arms it, warmup 1 s, gap 2.5 s / calm 5 s, question glances alternate, idle off, owners drop, box/device pause dedupe, perk after 1 s quiet");
    return 0;
}
