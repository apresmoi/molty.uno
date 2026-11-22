/* nap_test: main/fluid/screen_nap.c: the screen goes off the set time after the creature fell asleep (battery or
 * USB, never for 0 or unknown power), never while something is up, and wakes for the right things; a pick-up wakes
 * a nap, a knock, a bump or a truck going by does not. */
#include "screen_nap.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

#define S 1000u

/* the screen on, asleep or not, busy or not, from t0 to t1 (ms) a frame at a time: the ms it turned off at, 0 none */
static uint32_t run(nap_t *n, uint32_t t0, uint32_t t1, bool asleep, bool busy, int power)
{
    for (uint32_t t = t0; t < t1; t += 33)
        if (nap_tick(n, t, asleep, busy, power, NAP_BATTERY_S_DEFAULT, NAP_USB_S_DEFAULT)) return t;
    return 0;
}

static void test_battery_usb_never(void)
{
    nap_t n = { 0 };
    assert(!run(&n, 0, 300 * S, false, false, NAP_POWER_BATTERY));            /* awake: never */
    uint32_t off = run(&n, 300 * S, 400 * S, true, false, NAP_POWER_BATTERY); /* asleep on battery: 1 min */
    assert(off >= 360 * S && off < 360 * S + 100 && n.napping);
    nap_on(&n);
    off = run(&n, 0, 700 * S, true, false, NAP_POWER_USB);                    /* on USB: 10 min */
    assert(off >= 600 * S && off < 600 * S + 100);
    nap_on(&n);
    assert(!run(&n, 0, 36000 * S, true, false, NAP_POWER_UNKNOWN));           /* power not known: never */
    for (uint32_t t = 0; t < 36000 * S; t += 33)                              /* "never" on battery */
        assert(!nap_tick(&n, t, true, false, NAP_POWER_BATTERY, 0, NAP_USB_S_DEFAULT));
    for (uint32_t t = 0; t < 36000 * S; t += 33)
        assert(!nap_tick(&n, t, true, false, NAP_POWER_USB, NAP_BATTERY_S_DEFAULT, 0));
    /* unplugged while it slept on USB: the battery's time, counted from when it fell asleep */
    nap_on(&n);
    assert(!run(&n, 0, 50 * S, true, false, NAP_POWER_USB));
    off = run(&n, 50 * S, 100 * S, true, false, NAP_POWER_BATTERY);
    assert(off >= 60 * S && off < 60 * S + 100);
}

static void test_busy(void)
{
    nap_t n = { 0 };
    assert(!run(&n, 0, 3600 * S, true, true, NAP_POWER_BATTERY));             /* a scene, a question, ... up: never */
    /* 50 s asleep, a question for 5 s, asleep again: 1 min from there */
    assert(!run(&n, 0, 50 * S, true, false, NAP_POWER_BATTERY));
    assert(!run(&n, 50 * S, 55 * S, true, true, NAP_POWER_BATTERY));
    const uint32_t off = run(&n, 55 * S, 200 * S, true, false, NAP_POWER_BATTERY);
    assert(off >= 115 * S && off < 115 * S + 100);
    /* woken for a moment: counts from 0 again */
    nap_on(&n);
    assert(!run(&n, 0, 59 * S, true, false, NAP_POWER_BATTERY));
    assert(!run(&n, 59 * S, 60 * S, false, false, NAP_POWER_BATTERY));
    assert(!run(&n, 60 * S, 119 * S, true, false, NAP_POWER_BATTERY));
}

static void test_wakes(void)
{
    nap_t n = { 0 };
    /* off by hand: only what always woke it */
    nap_hand_off(&n);
    assert(nap_wakes(&n, NAP_WAKE_TOUCH) && nap_wakes(&n, NAP_WAKE_BUTTON) && nap_wakes(&n, NAP_WAKE_SHAKE) && nap_wakes(&n, NAP_WAKE_SHOW));
    assert(!nap_wakes(&n, NAP_WAKE_LIFT) && !nap_wakes(&n, NAP_WAKE_AGENT));
    /* a nap: every one */
    assert(run(&n, 0, 100 * S, true, false, NAP_POWER_BATTERY));
    for (int w = NAP_WAKE_TOUCH; w <= NAP_WAKE_AGENT; w++) assert(nap_wakes(&n, (nap_wake_t)w));
    /* the human turned it off by hand while it napped: it stays off for the agent and a pick-up */
    nap_hand_off(&n);
    assert(!nap_wakes(&n, NAP_WAKE_AGENT) && !nap_wakes(&n, NAP_WAKE_LIFT) && nap_wakes(&n, NAP_WAKE_TOUCH));
    /* back on: no longer a nap */
    assert(run(&n, 0, 100 * S, true, false, NAP_POWER_BATTERY));
    nap_on(&n);
    assert(!n.napping && !nap_wakes(&n, NAP_WAKE_AGENT));
}

static void test_hand_on_restarts(void)
{
    nap_t n = { 0 };
    assert(!run(&n, 0, 50 * S, true, false, NAP_POWER_BATTERY));
    nap_on(&n);                                                                /* the button, the cat still asleep */
    const uint32_t off = run(&n, 50 * S, 200 * S, true, false, NAP_POWER_BATTERY);
    assert(off >= 110 * S && off < 110 * S + 100);
}

/* ── pick-up ── */

/* a trace at 50 Hz from t0 for ms: gravity tilted by deg about x, plus a wobble (g, Hz) on x and z */
static uint32_t feed(nap_lift_t *l, uint32_t t0, uint32_t ms, float deg, float wob, float hz)
{
    for (uint32_t t = t0; t < t0 + ms; t += 20) {
        const float r = deg * 0.017453293f, w = wob * sinf(6.2831853f * hz * t / 1000.f);
        if (nap_lift_feed(l, w, sinf(r), cosf(r) + w, t)) return t;
    }
    return 0;
}

static void test_lift(void)
{
    nap_lift_t l;
    nap_lift_reset(&l);
    assert(!feed(&l, 0, 5000, 0.f, 0.f, 0.f) && l.ref_ok);                     /* at rest */
    const uint32_t t = feed(&l, 5000, 2000, 45.f, 0.02f, 3.f);                /* picked up, tilted to look at it */
    assert(t >= 5000 + NAP_LIFT_HOLD_MS && t < 6000);
    /* picked up by a shaky hand: a 0.4 g jitter at 9 Hz that swings it back toward the rest each beat still wakes it
     * within the second (the low-pass) */
    nap_lift_reset(&l);
    assert(!feed(&l, 0, 2000, 0.f, 0.f, 0.f));
    uint32_t woke = 0;
    for (uint32_t k = 2000; k < 3000 && !woke; k += 20) {
        const float r = 45.f * 0.017453293f, w = 0.4f * sinf(6.2831853f * 9.f * k / 1000.f);
        if (nap_lift_feed(&l, 0.f, sinf(r) - w, cosf(r) + w, k)) woke = k;
    }
    assert(woke);
    /* a truck going by: 0.3 g at 12 Hz for 20 s; a desk bump: 1.5 g for two samples; a knock that tips it 40 deg
     * for 200 ms and back; a slow lean of 20 deg that stays */
    nap_lift_reset(&l);
    assert(!feed(&l, 0, 2000, 0.f, 0.f, 0.f));
    assert(!feed(&l, 2000, 20000, 0.f, 0.3f, 12.f));
    for (uint32_t k = 0; k < 2; k++) assert(!nap_lift_feed(&l, 1.5f, 0.f, 1.f, 22000 + 20 * k));
    assert(!feed(&l, 22040, 1000, 0.f, 0.f, 0.f));
    assert(!feed(&l, 23040, 200, 40.f, 0.f, 0.f));
    assert(!feed(&l, 23240, 1000, 0.f, 0.f, 0.f));
    assert(!feed(&l, 24240, 10000, 20.f, 0.05f, 8.f));
    /* the rest it compares with is the one after it went off: a Molty that napped leaning wakes when it turns more */
    nap_lift_reset(&l);
    assert(!feed(&l, 0, 2000, 60.f, 0.f, 0.f));
    assert(!feed(&l, 2000, 3000, 70.f, 0.f, 0.f));
    assert(feed(&l, 5000, 2000, 10.f, 0.f, 0.f));
}

int main(void)
{
    assert(nap_valid_s(0) && nap_valid_s(NAP_MIN_S) && nap_valid_s(NAP_MAX_S) && !nap_valid_s(5) && !nap_valid_s(-1) && !nap_valid_s(NAP_MAX_S + 1));
    test_battery_usb_never();
    test_busy();
    test_wakes();
    test_hand_on_restarts();
    test_lift();
    printf("NAP PASS: the screen naps after the set time asleep, never while busy, wakes for the right things; a pick-up wakes it, a bump does not\n");
    return 0;
}
