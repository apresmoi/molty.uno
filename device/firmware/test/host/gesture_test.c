/*
 * SPDX-License-Identifier: MIT
 *
 * gesture_test.c — host test for main/sense/gesture.c on synthetic 50 Hz IMU traces
 * (device frame, g; the board lying flat: gravity on z).
 *
 * Checks: a shake fires once (and reports its peak / reversals / span); a single knock,
 * with or without ringing, does not; a slow or quick tilt does not; a gentle wobble
 * does not; the 2 s cooldown holds under continuous shaking; reset forgets a half-done
 * shake; the event rate limit (taps ≤ 4/s).
 *
 * Build + run: test/host/gesture_build.sh
 */
#include "gesture.h"

#include <math.h>
#include <stdio.h>

static int s_fail = 0, s_pass = 0;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define PI 3.14159265f
#define SAMPLE_MS 20

static gesture_t G;
static uint32_t s_ms;
static int s_shakes;
static gesture_shake_t s_last;
static uint32_t s_at[16];

static void feed(float ax, float ay, float az) {
    gesture_shake_t sh;
    if (gesture_feed(&G, ax, ay, az, s_ms, &sh)) { if (s_shakes < 16) s_at[s_shakes] = s_ms; s_shakes++; s_last = sh; }
    s_ms += SAMPLE_MS;
}
static void rest(float s) { for (int n = (int)(s * 1000) / SAMPLE_MS; n > 0; n--) feed(0.f, 0.f, 1.f); }
/* a hand shaking along x: amp g at hz, for s seconds */
static void shake(float amp, float hz, float s) {
    float t = 0.f;
    for (int n = (int)(s * 1000) / SAMPLE_MS; n > 0; n--, t += SAMPLE_MS / 1000.f)
        feed(amp * sinf(2.f * PI * hz * t), 0.f, 1.f);
}
static void start(void) { gesture_init(&G, NULL); s_ms = 1000; s_shakes = 0; rest(1.f); }

static void test_shake_once(void) {
    start();
    shake(2.0f, 5.f, 1.2f);
    rest(3.f);
    CHECK(s_shakes == 1, "a 1.2 s shake fired %d times", s_shakes);
    CHECK(s_last.reversals >= 3 && s_last.peak_g > 1.5f && s_last.peak_g < 2.6f && s_last.ms <= 800,
          "shake data: peak %.2f g, %d reversals, %u ms", s_last.peak_g, s_last.reversals, (unsigned)s_last.ms);
    CHECK(s_at[0] - 2000 <= 700, "detected %u ms after it began", (unsigned)(s_at[0] - 2000));

    start();                              /* a brisk 8 Hz shake, still a hand */
    shake(2.5f, 8.f, 0.8f);
    CHECK(s_shakes == 1, "8 Hz shake fired %d times", s_shakes);

    start();                              /* along y, upside down (gravity −z) */
    float t = 0.f;
    for (int n = 0; n < 60; n++, t += 0.02f) feed(0.f, 1.8f * sinf(2.f * PI * 4.f * t), -1.f);
    CHECK(s_shakes == 1, "y-axis shake fired %d times", s_shakes);
}

static void test_not_shakes(void) {
    start();                              /* one knock: a single spike */
    feed(3.5f, 0.f, 1.f);
    rest(2.f);
    CHECK(s_shakes == 0, "single spike fired");

    start();                              /* a knock that rings for a few samples */
    feed(3.f, 0.f, 1.f); feed(-2.2f, 0.f, 1.f); feed(1.8f, 0.f, 1.f); feed(-1.5f, 0.f, 1.f); feed(1.3f, 0.f, 1.f);
    rest(2.f);
    CHECK(s_shakes == 0, "ringing knock fired");

    start();                              /* four alternating knocks, 0.4 s apart: too slow (1.2 s > window) */
    for (int k = 0; k < 4; k++) { feed(k & 1 ? -3.f : 3.f, 0.f, 1.f); rest(0.4f); }
    CHECK(s_shakes == 0, "spaced knocks fired");

    start();                              /* slow tilt: flat → on its side → flat, 1 s each way, ×5 */
    for (int r = 0; r < 5; r++) {
        for (int n = 0; n <= 50; n++) { float a = PI / 2 * n / 50.f; feed(sinf(a), 0.f, cosf(a)); }
        for (int n = 50; n >= 0; n--) { float a = PI / 2 * n / 50.f; feed(sinf(a), 0.f, cosf(a)); }
    }
    CHECK(s_shakes == 0, "slow tilt fired");

    start();                              /* a quick turn: 90° in 0.2 s, back after 1 s */
    for (int n = 0; n <= 10; n++) { float a = PI / 2 * n / 10.f; feed(sinf(a), 0.f, cosf(a)); }
    for (int n = 0; n < 50; n++) feed(1.f, 0.f, 0.f);
    for (int n = 10; n >= 0; n--) { float a = PI / 2 * n / 10.f; feed(sinf(a), 0.f, cosf(a)); }
    rest(1.f);
    CHECK(s_shakes == 0, "quick tilt fired");

    start();                              /* carrying it: a gentle wobble */
    shake(0.8f, 3.f, 3.f);
    CHECK(s_shakes == 0, "gentle wobble fired");
}

/* continuous shaking: at most one per cooldown; and the cooldown really is 2 s */
static void test_cooldown(void) {
    start();
    shake(2.0f, 5.f, 6.f);
    CHECK(s_shakes >= 2 && s_shakes <= 3, "6 s of shaking fired %d times", s_shakes);
    for (int i = 1; i < s_shakes && i < 16; i++)
        CHECK(s_at[i] - s_at[i - 1] >= 2000, "shakes %u ms apart", (unsigned)(s_at[i] - s_at[i - 1]));

    start();
    shake(2.0f, 5.f, 0.8f);
    CHECK(s_shakes == 1, "first burst");
    rest(0.6f);                           /* a second burst inside the cooldown: ignored */
    shake(2.0f, 5.f, 0.8f);
    CHECK(s_shakes == 1, "burst within 2 s fired (%d)", s_shakes);
    rest(1.5f);                           /* past it: counts again */
    shake(2.0f, 5.f, 0.8f);
    CHECK(s_shakes == 2, "burst after the cooldown: %d", s_shakes);
}

static void test_reset(void) {
    start();
    shake(2.0f, 5.f, 0.3f);               /* half a shake... */
    gesture_reset(&G);                    /* ...then pairing takes the device */
    shake(2.0f, 5.f, 0.25f);
    CHECK(s_shakes == 0, "reset forgets earlier peaks");
}

static void test_limit(void) {
    gesture_limit_t l = { .min_ms = 250 };
    int ok = 0;
    for (uint32_t t = 0; t < 1000; t += 100) ok += gesture_limit_allow(&l, 5000 + t);
    CHECK(ok == 4, "taps every 100 ms for 1 s: %d allowed (≤ 4/s)", ok);
    gesture_limit_t z = { .min_ms = 2000 };
    CHECK(gesture_limit_allow(&z, 0) && !gesture_limit_allow(&z, 1999) && gesture_limit_allow(&z, 2000), "first at t=0, then 2 s");
}

/* a shake's phantom taps are guarded; a lone knock is not */
static void test_tap_guard(void) {
    gesture_t g; gesture_init(&g, NULL); gesture_shake_t sh; uint32_t t = 1000;
    gesture_feed(&g, 0, 0, 1, t, &sh);
    CHECK(!gesture_tap_guarded(&g, t), "quiet: not guarded");
    t += 20; gesture_feed(&g, 3, 0, 1, t, &sh); t += 20; gesture_feed(&g, 0, 0, 1, t, &sh);
    CHECK(!gesture_tap_guarded(&g, t), "one knock: a finger's tap is real");
    bool shook = false;
    for (int i = 0; i < 24 && !shook; i++) { t += 60; shook = gesture_feed(&g, i % 2 ? -3.f : 3.f, 0, 1, t, &sh); if (i == 2) CHECK(gesture_tap_guarded(&g, t), "strokes under way: guarded"); }
    CHECK(shook, "the shake fired");
    CHECK(gesture_tap_guarded(&g, t + GESTURE_TAP_SHAKE_MS - 1), "just after a shake: guarded");
    CHECK(!gesture_tap_guarded(&g, t + GESTURE_TAP_SHAKE_MS), "a second later: free");
    t += 100; gesture_feed(&g, 3, 0, 1, t, &sh);
    CHECK(gesture_tap_guarded(&g, t + GESTURE_TAP_SHAKE_MS - 1), "still shaking in the cooldown: the guard follows");
    t += 5000; CHECK(!gesture_tap_guarded(&g, t), "strokes long gone: free");
}

static void test_tap_route(void) {
    CHECK(gesture_tap_route(true, 999, true, false, false) == GESTURE_TAP_IGNORE, "first second of a recording");
    CHECK(gesture_tap_route(true, 1000, true, false, false) == GESTURE_TAP_STOP_LISTEN, "stop and send");
    CHECK(gesture_tap_route(true, 1000, false, false, false) == GESTURE_TAP_DROP_LISTEN, "link down: drop");
    CHECK(gesture_tap_route(true, 5000, true, false, true) == GESTURE_TAP_IGNORE, "a shake's phantom never stops it");
    CHECK(gesture_tap_route(false, 0, true, true, false) == GESTURE_TAP_CANCEL_SAY, "say waiting: cancel");
    CHECK(gesture_tap_route(false, 0, true, true, true) == GESTURE_TAP_IGNORE, "...not by a phantom");
    CHECK(gesture_tap_route(false, 0, true, false, false) == GESTURE_TAP_GESTURE, "speech playing or idle: as bound");
}

static void test_tapper(void) {
    gesture_tapper_t t = { 0 };
    CHECK(gesture_tapper_feed(&t, 100, false) == GESTURE_TAPS_SINGLE, "no double bound: at once");
    CHECK(gesture_tapper_feed(&t, 100, true) == GESTURE_TAPS_NONE, "double bound: waits");
    CHECK(gesture_tapper_poll(&t, 100 + GESTURE_DOUBLE_TAP_MS) == GESTURE_TAPS_NONE, "inside the window");
    CHECK(gesture_tapper_poll(&t, 101 + GESTURE_DOUBLE_TAP_MS) == GESTURE_TAPS_SINGLE, "window over: one tap");
    CHECK(gesture_tapper_poll(&t, 2000) == GESTURE_TAPS_NONE, "reported once");
    CHECK(gesture_tapper_feed(&t, 3000, true) == GESTURE_TAPS_NONE, "first");
    CHECK(gesture_tapper_feed(&t, 3020, true) == GESTURE_TAPS_NONE, "a bounce is the same touch");
    CHECK(gesture_tapper_feed(&t, 3200, true) == GESTURE_TAPS_DOUBLE, "second: double");
    CHECK(gesture_tapper_poll(&t, 4000) == GESTURE_TAPS_NONE, "a double leaves no single");
}

static void test_binds(void) {
    uint32_t p = gesture_binds_default();
    CHECK(gesture_binds_valid(p), "default valid");
    CHECK(gesture_binds_get(p, GESTURE_IN_TAP) == GESTURE_DO_EVENT, "tap: event");
    CHECK(gesture_binds_get(p, GESTURE_IN_DOUBLE_TAP) == GESTURE_DO_TALK, "double tap: talk");
    CHECK(gesture_binds_get(p, GESTURE_IN_BUTTON_ACCEPT) == GESTURE_DO_EVENT, "accept: event");
    CHECK(gesture_binds_get(p, GESTURE_IN_BUTTON_REJECT) == GESTURE_DO_EVENT, "reject: event");
    p = gesture_binds_set(p, GESTURE_IN_BUTTON_REJECT, GESTURE_DO_STOP);
    CHECK(gesture_binds_get(p, GESTURE_IN_BUTTON_REJECT) == GESTURE_DO_STOP, "set");
    CHECK(gesture_binds_get(p, GESTURE_IN_TAP) == GESTURE_DO_EVENT, "others kept");
    CHECK(!gesture_binds_valid(0x9u), "an unknown action in NVS is refused");
    CHECK(!gesture_binds_valid(0x10000u), "extra bits are refused");
    CHECK(gesture_input_lookup("double_tap") == GESTURE_IN_DOUBLE_TAP && gesture_input_lookup("nose") < 0, "input names");
    CHECK(gesture_action_lookup("stop") == GESTURE_DO_STOP && gesture_action_lookup("dance") < 0, "action names");
}

int main(void) {
    printf("gesture host test\n");
    test_shake_once();
    test_not_shakes();
    test_cooldown();
    test_reset();
    test_limit();
    test_tap_guard();
    test_tap_route();
    test_tapper();
    test_binds();
    printf("%d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
