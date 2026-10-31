/*
 * SPDX-License-Identifier: MIT
 *
 * director_test.c — host test for main/fluid/fluid_director.c: what the fluid shows and
 * the creature's idle life, driven the way mole_fluid.c drives it (requests, then
 * fd_tick + fd_take once per ~30 fps frame; every fd_take is one melt).
 *
 * Checks: boot lands on the creature (the blob only until one loads); re-requesting the
 * shown state / a mood never melts it or resets its solidity; show returns to the
 * creature 1.5 s after the last word (held text stays); idle life (lively / calm / off):
 * action gaps, the library, no immediate repeat, never over a screen owner, a fixed seed
 * repeats; the yawn before dozing off; sleep and waking; agent mood holds; agent actions
 * (wait for the creature, dropped after 3 s); the happy flash after a say; shake; status
 * changes never change the state; ending the reel (pairing / restore) never re-enables it.
 *
 * Build + run: test/host/director_build.sh
 */
#include "fluid_director.h"
#include "cute_spec.h"
#include "cute_act.h"

#include <math.h>
#include <stdio.h>

static int s_fail = 0, s_pass = 0;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define DT (1.f / 30.f)

static fluid_director_t D;
static int s_melts;                       /* transitions executed (each one melts the shape) */
/* actions handed out (fd_take_act), as mole_fluid.c starts them on the creature */
#define ACT_LOG 4096
static int s_nact, s_act[ACT_LOG];
static double s_now, s_act_t[ACT_LOG];   /* double: a float clock drifts ~0.1 s over 2 h */

static void frame(bool text_done) {
    fd_tick(&D, DT, text_done);
    int st, fx;
    if (fd_take(&D, &st, &fx)) s_melts++;
    int a = fd_take_act(&D);
    if (a >= 0 && s_nact < ACT_LOG) { s_act[s_nact] = a; s_act_t[s_nact++] = s_now; }
    s_now += DT;
}
static void run(float s, bool text_done) { for (int n = (int)lrintf(s / DT); n > 0; n--) frame(text_done); }
/* a request arrives between frames, then the frame runs (mole_fluid.c's order) */
static void request(int st, bool hold) { fd_request_state(&D, st, 0, hold); frame(false); }

static void boot(bool creature) {
    fd_init(&D, NULL, 12345);
    s_melts = 0; s_nact = 0; s_now = 0.f;
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

/* 2 h of idle life at one level, poked every 4 min (no dozing): the actions it played */
static void idle_run(int level, uint32_t seed) {
    fd_init(&D, NULL, seed);
    s_melts = 0; s_nact = 0; s_now = 0.f;
    fd_set_creature(&D, true);
    fd_set_idle_life(&D, level);
    for (int i = 0; i < (int)(7200.f / DT); i++) {
        if (i % (int)(240.f / DT) == 0) fd_user(&D);
        frame(false);
    }
}

static bool in_lib(int a, bool calm) {
    return a == CA_GLANCE_LEFT || a == CA_GLANCE_RIGHT || a == CA_BLINK || a == CA_LOOK_UP ||
           (!calm && (a == CA_WOBBLE || a == CA_STRETCH));
}

static void check_idle(const char *what, float lo, float hi, bool calm) {
    int repeats = 0, bad_gap = 0, outside = 0, kinds[CUTE_N_ACTS] = { 0 };
    for (int i = 0; i < s_nact; i++) {
        kinds[s_act[i]]++;
        if (!in_lib(s_act[i], calm)) outside++;
        if (i == 0) continue;
        if (s_act[i] == s_act[i - 1]) repeats++;
        /* the gap runs once the previous action is over */
        float gap = (float)(s_act_t[i] - s_act_t[i - 1]) - cute_act_dur(s_act[i - 1]);
        if (gap < lo - 2 * DT || gap > hi + 2 * DT) { bad_gap++; printf("  gap %.2f at %.1f s (%s after %s)\n", gap, s_act_t[i], cute_act_name(s_act[i]), cute_act_name(s_act[i - 1])); }
    }
    CHECK(s_nact >= (int)(7200 / (hi + 2.6f)) && s_nact <= (int)(7200 / lo), "%s: %d idle actions in 2 h", what, s_nact);
    CHECK(bad_gap == 0, "%s: %d gaps outside %.0f..%.0f s", what, bad_gap, lo, hi);
    CHECK(repeats == 0, "%s: the same action twice in a row %d times", what, repeats);
    CHECK(outside == 0, "%s: %d actions outside its library", what, outside);
    int used = 0;
    for (int a = 0; a < CUTE_N_ACTS; a++) used += kinds[a] > 0;
    CHECK(used == (calm ? 4 : 6), "%s: %d kinds of action used", what, used);
    CHECK(kinds[CA_BLINK] > kinds[CA_LOOK_UP] / 2, "%s: weights (blink %d, look up %d)", what, kinds[CA_BLINK], kinds[CA_LOOK_UP]);
    CHECK(fd_mood(&D) == CM_IDLE && D.cur == FD_ST_CREATURE && s_melts == 1, "%s: actions never change the mood or melt (%d)", what, s_melts);
}

static void test_idle_life(void) {
    boot(true);
    CHECK(D.idle_life == FD_IDLE_LIVELY && FD_IDLE_DEFAULT == FD_IDLE_LIVELY, "lively out of the box");
    idle_run(FD_IDLE_LIVELY, 12345);
    check_idle("lively", 10.f, 40.f, false);
    CHECK(s_act[0] != CA_YAWN, "no yawn while poked");
    idle_run(FD_IDLE_CALM, 12345);
    check_idle("calm", 20.f, 60.f, true);
    idle_run(FD_IDLE_OFF, 12345);
    CHECK(s_nact == 0, "off: %d idle actions", s_nact);

    /* same seed, same life */
    fluid_director_t a, b;
    fd_init(&a, NULL, 777); fd_init(&b, NULL, 777);
    fd_set_creature(&a, true); fd_set_creature(&b, true);
    bool same = true;
    for (int i = 0; i < (int)(600.f / DT); i++) {
        fd_tick(&a, DT, false); fd_take(&a, NULL, NULL);
        fd_tick(&b, DT, false); fd_take(&b, NULL, NULL);
        if (fd_take_act(&a) != fd_take_act(&b) || fd_mood(&a) != fd_mood(&b)) same = false;
    }
    CHECK(same, "a fixed seed repeats");

    /* never over an owner of the screen, never over an agent's mood, never on other states */
    boot(true);
    fd_set_busy(&D, true, false);                 /* a listen / say */
    for (int i = 0; i < (int)(200.f / DT); i++) { if (i % (int)(100.f / DT) == 0) fd_user(&D); frame(false); }
    CHECK(s_nact == 0, "busy: %d idle actions", s_nact);
    fd_set_busy(&D, false, true);                 /* a question */
    for (int i = 0; i < (int)(200.f / DT); i++) { if (i % (int)(100.f / DT) == 0) fd_user(&D); frame(false); }
    CHECK(s_nact == 0, "a question up: %d idle actions", s_nact);
    fd_set_busy(&D, false, false);
    fd_agent_mood(&D, CM_THINKING, 0.f);          /* the ack's thinking, an agent's mood */
    for (int i = 0; i < (int)(200.f / DT); i++) { if (i % (int)(100.f / DT) == 0) fd_user(&D); frame(false); }
    CHECK(s_nact == 0, "an agent's mood: %d idle actions", s_nact);
    fd_agent_mood(&D, CM_IDLE, -1.f);
    request(FD_ST_TEXT, true);                    /* pairing code / update screen: held text */
    for (int i = 0; i < (int)(200.f / DT); i++) { if (i % (int)(100.f / DT) == 0) fd_user(&D); frame(true); }
    CHECK(s_nact == 0, "text on screen: %d idle actions", s_nact);
    request(FD_ST_CREATURE, false);
    fd_autoplay(&D);
    for (int i = 0; i < (int)(100.f / DT); i++) frame(true);
    CHECK(s_nact == 0, "the demo reel: %d idle actions", s_nact);
    request(FD_ST_CREATURE, false);               /* back: idle life resumes */
    for (int i = 0; i < (int)(60.f / DT); i++) frame(false);
    CHECK(s_nact >= 1, "idle life resumes on the creature: %d", s_nact);

    /* an agent's command restarts the gap: no idle action right after it */
    boot(true);
    run(9.f, false);
    fd_activity(&D);
    run(9.5f, false);
    CHECK(s_nact == 0, "no idle action within 10 s of agent activity: %d", s_nact);
}

static void test_yawn(void) {
    boot(true);
    run(296.8f, false);
    CHECK(s_nact == 0 || s_act[s_nact - 1] != CA_YAWN, "no yawn before 297 s");
    int before = s_nact;
    float t_last = s_nact ? (float)s_act_t[s_nact - 1] + cute_act_dur(s_act[s_nact - 1]) : 0.f;
    run(4.4f, false);
    bool yawned = false;
    for (int i = before; i < s_nact; i++) if (s_act[i] == CA_YAWN) yawned = true;
    /* the yawn waits for an idle action still playing, so it lands by ~297 s or right after it */
    CHECK(yawned || t_last > 297.f, "a yawn before dozing off");
    CHECK(fd_asleep(&D) && fd_mood(&D) == CM_SLEEPY, "asleep at 301 s");
    int n = s_nact;
    run(600.f, false);
    CHECK(s_nact == n, "asleep: no actions (%d)", s_nact - n);
    fd_user(&D); run(250.f, false); fd_user(&D);  /* a new quiet spell yawns again */
    n = s_nact;
    run(301.f, false);
    int yawns = 0;
    for (int i = n; i < s_nact; i++) yawns += s_act[i] == CA_YAWN;
    CHECK(yawns == 1, "one yawn per quiet spell: %d", yawns);

    fd_init(&D, NULL, 5); fd_set_creature(&D, true); fd_set_idle_life(&D, FD_IDLE_OFF);
    s_nact = 0;
    for (int i = 0; i < (int)(302.f / DT); i++) frame(false);
    CHECK(s_nact == 0 && fd_asleep(&D), "off: dozes off without a yawn");
}

static void test_agent_acts(void) {
    boot(true);
    run(1.f, false);
    int n = s_nact;
    fd_agent_act(&D, CA_NOD);
    frame(false);
    CHECK(s_nact == n + 1 && s_act[n] == CA_NOD, "an agent's nod plays at once");
    CHECK(fd_mood(&D) == CM_IDLE && s_melts == 1, "an action changes no mood, no state");

    fd_set_busy(&D, true, false);                 /* listening: the creature is shown */
    fd_agent_act(&D, CA_BOUNCE); frame(false);
    CHECK(s_act[s_nact - 1] == CA_BOUNCE, "an agent's action plays during a listen");
    fd_set_busy(&D, false, true);                 /* a question covers it */
    n = s_nact;
    fd_agent_act(&D, CA_WIGGLE); run(2.5f, false);
    CHECK(s_nact == n, "covered by a question: the action waits");
    fd_set_busy(&D, false, false); frame(false);
    CHECK(s_nact == n + 1 && s_act[n] == CA_WIGGLE, "uncovered within 3 s: it plays");
    fd_set_busy(&D, false, true);
    n = s_nact;
    fd_agent_act(&D, CA_PEEK); run(3.1f, false);
    fd_set_busy(&D, false, false); run(1.f, false);
    CHECK(s_nact == n, "covered over 3 s: dropped");

    /* a say's action mark: the text is on screen; it plays when the creature is back */
    request(FD_ST_TEXT, false);
    n = s_nact;
    fd_agent_act(&D, CA_NOD); run(0.5f, true);
    CHECK(s_nact == n, "words on screen: the action waits");
    run(1.2f, true);                              /* 1.5 s after the last word: the creature */
    CHECK(D.cur == FD_ST_CREATURE && s_nact == n + 1 && s_act[n] == CA_NOD, "the creature is back: it plays");

    boot(true);                                   /* it wakes a sleeping creature */
    run(302.f, false);
    CHECK(fd_asleep(&D), "asleep");
    n = s_nact;
    fd_agent_act(&D, CA_WIGGLE); frame(false);
    CHECK(!fd_asleep(&D) && s_nact == n + 1, "an action wakes it and plays");
    fd_agent_act(&D, -1); fd_agent_act(&D, CUTE_N_ACTS); frame(false);
    CHECK(s_nact == n + 1, "unknown actions are ignored");
}

/* the reflex during a listen (services/relay reflexFire): the listen holds `listening`; a react mood
 * (mood {react:true} → the creature state + the mood) shows at once, without a melt, and the relay's
 * `listening` comes back the same way; its gestures and the device's baseline (act) play at once —
 * also with idle life off (that only stops Molty's own idle actions) */
static void test_listen_reacts(void) {
    boot(true);
    run(1.f, false);
    fd_set_busy(&D, true, false);                 /* the listen (mole_fluid: s_listen) */
    request(FD_ST_CREATURE, false); fd_agent_mood(&D, CM_LISTENING, 0.f); frame(false);
    const int melts = s_melts;
    CHECK(fd_mood(&D) == CM_LISTENING, "listening: %d", fd_mood(&D));
    static const int moods[] = { CM_SURPRISED, CM_HAPPY };
    for (int i = 0; i < 2; i++) {
        request(FD_ST_CREATURE, false); fd_agent_mood(&D, moods[i], 0.f); frame(false);
        CHECK(fd_mood(&D) == moods[i] && D.cur == FD_ST_CREATURE, "a reflex react mood shows over the listen: %d", fd_mood(&D));
        run(1.5f, false);
        CHECK(fd_mood(&D) == moods[i], "held until the relay's listening (hold_s 0): %d", fd_mood(&D));
        request(FD_ST_CREATURE, false); fd_agent_mood(&D, CM_LISTENING, 0.f); frame(false);
        CHECK(fd_mood(&D) == CM_LISTENING, "back to listening: %d", fd_mood(&D));
    }
    CHECK(s_melts == melts, "react moods never melt the creature (%d melts)", s_melts - melts);
    fd_set_idle_life(&D, FD_IDLE_OFF);
    static const int acts[] = { CA_NOD, CA_BOUNCE, CA_PEEK, CA_LOOK_UP, CA_WIGGLE, CA_WOBBLE, CA_GLANCE_LEFT, CA_GLANCE_RIGHT, CA_BLINK };
    for (int i = 0; i < (int)(sizeof(acts) / sizeof(acts[0])); i++) {
        const int n = s_nact;
        fd_agent_act(&D, acts[i]); frame(false);
        CHECK(s_nact == n + 1 && s_act[n] == acts[i], "%s plays at once over the listen (idle life off)", cute_act_name(acts[i]));
        CHECK(fd_mood(&D) == CM_LISTENING, "...and leaves the listening mood: %d", fd_mood(&D));
        run(cute_act_dur(acts[i]), false);
    }
}

static void test_convo(void) {
    boot(true);
    request(FD_ST_TEXT, false);                   /* a say: its words */
    fd_agent_mood(&D, CM_SPEAKING, -1.f);
    run(2.f, true);
    fd_agent_mood(&D, CM_IDLE, -1.f);             /* say.done: idle, the creature */
    fd_turn_spoken(&D);
    request(FD_ST_CREATURE, false);
    run(0.9f, false);
    CHECK(fd_mood(&D) == CM_IDLE, "the creature settles first");
    run(0.3f, false);
    CHECK(fd_mood(&D) == CM_HAPPY, "then a happy flash: %d", fd_mood(&D));
    run(2.1f, false);
    CHECK(fd_mood(&D) == CM_IDLE, "for 2 s");
    fd_turn_spoken(&D); run(5.f, false);
    CHECK(fd_mood(&D) == CM_IDLE, "at most one a minute");
    run(60.f, false);
    fd_turn_spoken(&D);
    fd_agent_mood(&D, CM_LISTENING, -1.f);        /* the next listen took over */
    run(3.f, false);
    CHECK(fd_mood(&D) == CM_LISTENING, "never over a listen");
    fd_agent_mood(&D, CM_IDLE, -1.f);
    run(9.f, false);
    CHECK(fd_mood(&D) == CM_IDLE, "the window passed: no late flash");
    run(60.f, false);
    fd_set_idle_life(&D, FD_IDLE_OFF);
    fd_turn_spoken(&D); run(3.f, false);
    CHECK(fd_mood(&D) == CM_IDLE, "idle life off: no flash");
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
    fd_turn_spoken(&D); run(3.f, false);
    CHECK(fd_mood(&D) == CM_SLEEPY, "no happy flash while asleep");
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

/* a question covered the shown state and left: replay restarts its clock (it flows back
 * before it crystallises) without a transition or a change of state */
static void test_replay(void) {
    boot(true);
    run(5.f, false);
    CHECK(fd_solidity(&D, 1.f) > 0.99f, "settled creature solidity %f", fd_solidity(&D, 1.f));
    int melts = s_melts;
    fd_replay(&D);
    CHECK(fd_solidity(&D, 1.f) == 0.f && D.cur == FD_ST_CREATURE && !D.pending && s_melts == melts, "replay");
    run(2.f, false);
    CHECK(fd_solidity(&D, 1.f) > 0.99f, "replayed creature settles again: %f", fd_solidity(&D, 1.f));
}

int main(void) {
    printf("director host test\n");
    test_boot();
    test_guard();
    test_show_returns();
    test_idle_life();
    test_yawn();
    test_agent_acts();
    test_listen_reacts();
    test_convo();
    test_sleep();
    test_agent_moods();
    test_shake();
    test_status();
    test_reel();
    test_replay();
    printf("%d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
