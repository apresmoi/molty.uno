/*
 * SPDX-License-Identifier: MIT
 *
 * director_test.c — host test for main/fluid/fluid_director.c: what the fluid shows and
 * the creature's idle life, driven the way mole_fluid.c drives it (requests, then
 * fd_tick + fd_take once per ~30 fps frame; every fd_take is one melt).
 *
 * Checks: boot lands on the creature (the blob only until one loads); re-requesting the
 * shown state / a mood never melts it or resets its solidity; show returns to the
 * creature 1.5 s after the last word (held text stays); idle flash gaps and lengths
 * under a fixed seed; sleep and waking; agent mood holds; shake; status changes never
 * change the state; ending the reel (pairing / restore) never re-enables it.
 *
 * Build + run: test/host/director_build.sh
 */
#include "fluid_director.h"
#include "cute_spec.h"

#include <math.h>
#include <stdio.h>

static int s_fail = 0, s_pass = 0;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define DT (1.f / 30.f)

static fluid_director_t D;
static int s_melts;                       /* transitions executed (each one melts the shape) */

static void frame(bool text_done) {
    fd_tick(&D, DT, text_done);
    int st, fx;
    if (fd_take(&D, &st, &fx)) s_melts++;
}
static void run(float s, bool text_done) { for (int n = (int)lrintf(s / DT); n > 0; n--) frame(text_done); }
/* a request arrives between frames, then the frame runs (mole_fluid.c's order) */
static void request(int st, bool hold) { fd_request_state(&D, st, 0, hold); frame(false); }

static void boot(bool creature) {
    fd_init(&D, NULL, 12345);
    s_melts = 0;
    fd_set_creature(&D, creature);        /* the stored / default .molty loads on frame one */
    frame(false);
}

static void test_boot(void) {
    boot(true);
    CHECK(D.cur == FD_ST_CREATURE && !D.autoplay, "boot shows state %d autoplay %d", D.cur, D.autoplay);
    CHECK(fd_mood(&D) == CM_IDLE, "boot mood %d", fd_mood(&D));
    run(40.f, false);
    CHECK(D.cur == FD_ST_CREATURE && !D.autoplay && s_melts == 1, "40 s later: state %d, %d melts (no reel)", D.cur, s_melts);

    boot(false);                          /* no creature (a bad file and no default) */
    CHECK(D.cur == FD_ST_BLOB, "no creature: blob, got %d", D.cur);
    run(2.f, false);
    fd_set_creature(&D, true);            /* one arrives (upload) */
    frame(false);
    CHECK(D.cur == FD_ST_CREATURE, "creature loaded later: shown, got %d", D.cur);
}

/* the guard: the shown state / a mood again changes nothing. Without it every request
 * re-enters the state, which resets its clock: solidity falls back to 0 and it melts. */
static void test_guard(void) {
    boot(true);
    run(3.f, false);
    CHECK(fd_solidity(&D, 1.f) > 0.999f, "settled after 3 s: %.3f", fd_solidity(&D, 1.f));
    int melts = s_melts;
    float min_sol = 1.f;
    static const int moods[] = { CM_HAPPY, CM_THINKING, CM_LISTENING, CM_IDLE, CM_THINKING };
    for (int i = 0; i < 25; i++) {        /* a chatty agent: state + mood every 200 ms */
        fd_request_state(&D, FD_ST_CREATURE, 0, false);
        fd_agent_mood(&D, moods[i % 5], -1.f);
        for (int k = 0; k < 6; k++) { frame(false); if (fd_solidity(&D, 1.f) < min_sol) min_sol = fd_solidity(&D, 1.f); }
    }
    CHECK(s_melts == melts, "repeated creature/mood requests melted it %d times", s_melts - melts);
    CHECK(min_sol > 0.999f, "solidity dipped to %.3f", min_sol);
    CHECK(fd_mood(&D) == CM_THINKING, "the last mood blends in: %d", fd_mood(&D));

    request(FD_ST_FX, false);             /* a different state still transitions */
    CHECK(D.cur == FD_ST_FX && s_melts == melts + 1, "effect: state %d", D.cur);
    fd_request_state(&D, FD_ST_FX, 1, false); frame(false);
    CHECK(s_melts == melts + 2 && D.cur_fx == 1, "another effect is another state");
    request(FD_ST_TEXT, false); request(FD_ST_TEXT, false);
    CHECK(s_melts == melts + 4, "text always restarts the reader");
}

static void test_show_returns(void) {
    boot(true);
    run(2.f, false);
    request(FD_ST_TEXT, false);
    CHECK(D.cur == FD_ST_TEXT, "show: text");
    run(3.f, false);                      /* reading */
    CHECK(D.cur == FD_ST_TEXT, "still reading");
    run(1.4f, true);                      /* holding the last word */
    CHECK(D.cur == FD_ST_TEXT, "last word held 1.4 s");
    run(0.5f, false);                     /* streamed words arrived: reading again */
    run(1.4f, true);
    CHECK(D.cur == FD_ST_TEXT, "the return waits for the new last word");
    run(0.2f, true);
    CHECK(D.cur == FD_ST_CREATURE, "back to the creature 1.5 s after the last word, got %d", D.cur);

    request(FD_ST_TEXT, true);            /* the pairing code / "hi": held */
    run(120.f, true);
    CHECK(D.cur == FD_ST_TEXT, "held text stays");
    request(FD_ST_CREATURE, false);       /* the restore */
    CHECK(D.cur == FD_ST_CREATURE, "restore: creature");
}

static void test_idle_flashes(void) {
    boot(true);
    int n = 0, happy = 0, surprised = 0, bad_gap = 0, bad_len = 0;
    float since_end = 0.f, flash_t = 0.f;
    int prev = CM_IDLE;
    for (int i = 0; i < (int)(7200.f / DT); i++) {       /* 2 h, poked every 4 min (no sleep) */
        if (i % (int)(240.f / DT) == 0) fd_user(&D);
        frame(false);
        int m = fd_mood(&D);
        if (m != CM_IDLE && prev == CM_IDLE) {
            n++;
            if (m == CM_HAPPY) happy++; else if (m == CM_SURPRISED) surprised++;
            if (since_end < 20.f - 2 * DT || since_end > 60.f + 2 * DT) bad_gap++;
            flash_t = 0.f;
        }
        if (m != CM_IDLE) flash_t += DT;
        if (m == CM_IDLE && prev != CM_IDLE) {
            float want = prev == CM_HAPPY ? 2.0f : 1.2f;
            if (fabsf(flash_t - want) > 2 * DT) bad_len++;
            since_end = 0.f;
        }
        if (m == CM_IDLE) since_end += DT;
        prev = m;
    }
    CHECK(n >= 7200 / 62 && n <= 7200 / 21, "%d flashes in 2 h", n);
    CHECK(bad_gap == 0, "%d gaps outside 20..60 s", bad_gap);
    CHECK(bad_len == 0, "%d flashes of the wrong length", bad_len);
    CHECK(happy > surprised && surprised > 0 && happy + surprised == n, "happy %d surprised %d", happy, surprised);
    CHECK(D.cur == FD_ST_CREATURE && s_melts == 1, "flashes never melt: %d", s_melts);

    /* same seed, same life */
    fluid_director_t a, b;
    fd_init(&a, NULL, 777); fd_init(&b, NULL, 777);
    fd_set_creature(&a, true); fd_set_creature(&b, true);
    bool same = true;
    for (int i = 0; i < (int)(600.f / DT); i++) {
        fd_tick(&a, DT, false); fd_take(&a, NULL, NULL);
        fd_tick(&b, DT, false); fd_take(&b, NULL, NULL);
        if (fd_mood(&a) != fd_mood(&b)) same = false;
    }
    CHECK(same, "a fixed seed repeats");

    /* no flashes while the agent holds a mood */
    boot(true);
    fd_agent_mood(&D, CM_THINKING, -1.f);
    bool other = false;
    for (int i = 0; i < (int)(240.f / DT); i++) { frame(false); if (fd_mood(&D) != CM_THINKING) other = true; }
    CHECK(!other, "thinking persists, no flash over it");
}

static void test_sleep(void) {
    boot(true);
    run(299.f, false);
    CHECK(!fd_asleep(&D), "awake at 299 s");
    run(2.f, false);
    CHECK(fd_asleep(&D) && fd_mood(&D) == CM_SLEEPY, "asleep after 5 min: mood %d", fd_mood(&D));
    bool flashed = false;
    for (int i = 0; i < (int)(300.f / DT); i++) { frame(false); if (fd_mood(&D) != CM_SLEEPY) flashed = true; }
    CHECK(!flashed, "no flashes while asleep");
    fd_user(&D); frame(false);
    CHECK(!fd_asleep(&D) && fd_mood(&D) == CM_IDLE, "touch wakes");
    run(301.f, false);
    fd_activity(&D); frame(false);
    CHECK(!fd_asleep(&D), "agent activity wakes");
    run(301.f, false);
    fd_agent_mood(&D, CM_HAPPY, -1.f); frame(false);
    CHECK(fd_mood(&D) == CM_HAPPY, "a mood wakes into it");
    CHECK(s_melts == 1, "sleeping and waking never melt: %d", s_melts);

    boot(true);                           /* an agent's persistent mood is not dozed over */
    fd_agent_mood(&D, CM_LISTENING, -1.f);
    run(600.f, false);
    CHECK(!fd_asleep(&D) && fd_mood(&D) == CM_LISTENING, "listening held 10 min");
}

static void test_agent_moods(void) {
    boot(true);
    fd_agent_mood(&D, CM_HAPPY, -1.f);
    run(2.9f, false);
    CHECK(fd_mood(&D) == CM_HAPPY, "happy holds ~3 s");
    run(0.2f, false);
    CHECK(fd_mood(&D) == CM_IDLE, "happy reverts by default");
    fd_agent_mood(&D, CM_SURPRISED, -1.f);
    run(2.1f, false);
    CHECK(fd_mood(&D) == CM_IDLE, "surprised reverts by default");
    fd_agent_mood(&D, CM_HAPPY, 0.f);
    run(30.f, false);
    CHECK(fd_mood(&D) == CM_HAPPY, "hold_s 0: until the next mood");
    fd_agent_mood(&D, CM_THINKING, 5.f);
    run(4.9f, false);
    CHECK(fd_mood(&D) == CM_THINKING, "hold_s 5: held");
    run(0.2f, false);
    CHECK(fd_mood(&D) == CM_IDLE, "hold_s 5: then idle");
    fd_agent_mood(&D, CM_SPEAKING, -1.f);
    run(60.f, false);
    CHECK(fd_mood(&D) == CM_SPEAKING, "speaking persists");
    fd_agent_mood(&D, CM_IDLE, -1.f);
    CHECK(fd_mood(&D) == CM_IDLE, "idle ends it");
    CHECK(fd_default_hold(CM_LISTENING) < 0 && fd_default_hold(CM_THINKING) < 0 && fd_default_hold(CM_HAPPY) > 0,
          "default holds");
}

static void test_shake(void) {
    boot(true);
    fd_agent_mood(&D, CM_THINKING, -1.f);
    fd_shake(&D); frame(false);
    CHECK(fd_mood(&D) == CM_SURPRISED, "shake: surprised over thinking");
    run(1.3f, false);
    CHECK(fd_mood(&D) == CM_THINKING, "then back to the held mood");
    boot(true);
    run(301.f, false);
    fd_shake(&D); frame(false);
    CHECK(!fd_asleep(&D) && fd_mood(&D) == CM_SURPRISED, "a shake wakes it, surprised");
    CHECK(s_melts == 1, "shakes never melt");
}

static void test_status(void) {
    boot(true);
    run(3.f, false);
    int melts = s_melts;
    float t0 = D.state_t;
    CHECK(fd_status(&D) == FD_STATUS_OK, "ok at boot");
    fd_set_status(&D, FD_STATUS_WIFI);
    run(9.8f, false);
    CHECK(fd_status(&D) == FD_STATUS_OK, "not before 10 s (boot joins take seconds)");
    run(0.4f, false);
    CHECK(fd_status(&D) == FD_STATUS_WIFI, "no Wi-Fi shows");
    fd_set_status(&D, FD_STATUS_RELAY); run(10.1f, false);
    CHECK(fd_status(&D) == FD_STATUS_RELAY, "relay offline shows");
    fd_set_status(&D, FD_STATUS_AUTH); run(10.1f, false);
    CHECK(fd_status(&D) == FD_STATUS_AUTH, "auth failed shows");
    fd_set_status(&D, FD_STATUS_OK); frame(false);
    CHECK(fd_status(&D) == FD_STATUS_OK, "ok hides at once");
    fd_set_status(&D, FD_STATUS_RELAY); run(5.f, false);
    fd_set_status(&D, FD_STATUS_OK); run(1.f, false);
    fd_set_status(&D, FD_STATUS_RELAY); run(5.f, false);
    CHECK(fd_status(&D) == FD_STATUS_OK, "a blip under 10 s never shows");
    CHECK(s_melts == melts && D.cur == FD_ST_CREATURE, "status never changes the state (%d melts)", s_melts - melts);
    CHECK(D.state_t > t0 + 40.f && fd_solidity(&D, 1.f) > 0.999f, "nor its clock");
    CHECK(fd_status_from(false, false, true) == FD_STATUS_WIFI && fd_status_from(true, false, true) == FD_STATUS_AUTH
          && fd_status_from(true, false, false) == FD_STATUS_RELAY && fd_status_from(true, true, false) == FD_STATUS_OK,
          "status precedence");
}

static void test_reel(void) {
    boot(true);
    fd_autoplay(&D);
    run(20.f, true);                      /* text steps run to their (immediate) end */
    CHECK(D.autoplay && s_melts >= 5, "the reel runs on request: %d melts", s_melts);
    /* pairing: the code (held text) ends the reel; the restore asks for the creature */
    request(FD_ST_TEXT, true);
    CHECK(!D.autoplay && D.cur == FD_ST_TEXT, "pairing code ends the reel");
    run(30.f, true);
    request(FD_ST_CREATURE, false);
    int melts = s_melts;
    run(120.f, false);
    CHECK(!D.autoplay && D.cur == FD_ST_CREATURE && s_melts == melts, "restore never re-enables the reel");
    CHECK(fd_mood(&D) == CM_IDLE || fd_mood(&D) == CM_HAPPY || fd_mood(&D) == CM_SURPRISED,
          "the reel's last mood does not stick: %d", fd_mood(&D));
}

int main(void) {
    printf("director host test\n");
    test_boot();
    test_guard();
    test_show_returns();
    test_idle_flashes();
    test_sleep();
    test_agent_moods();
    test_shake();
    test_status();
    test_reel();
    printf("%d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
