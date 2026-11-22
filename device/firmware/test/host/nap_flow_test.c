/*
 * SPDX-License-Identifier: MIT
 *
 * nap_flow_test.c: the screen's nap through mole_fluid.c itself (the web module's native build, web_molty.c, included
 * whole: its statics are this file's). The cat asleep on battery for the set time: the screen goes off; not before the
 * power is known, not while a listen or a question is up; the agent's mood or a pick-up ends a nap with the cat
 * awake; an off by hand stays off for them and a touch still wakes it; an on by hand starts the timer again.
 *
 *   nap_flow_test <repo root>
 */
#include "../../web/web_molty.c"

static int s_pass, s_fail;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define DT 33333u
#define FPS 30
#define NAP_S 10
static void frames(int n) { for (int i = 0; i < n; i++) wm_frame(DT); }
static bool on(void) { return mole_fluid_screen_on(); }
/* frames until the screen goes off (at most max s): the s it took, -1 if it did not */
static float until_off(float max) {
    for (int i = 0; i < (int)(max * FPS); i++) { if (!on()) return (float)i / FPS; wm_frame(DT); }
    return on() ? -1.f : max;
}
static bool until_asleep(void) {
    for (int i = 0; i < 30 * FPS && !fd_asleep(&s_dir); i++) wm_frame(DT);
    return fd_asleep(&s_dir);
}

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "nap_flow_test: cannot read %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    b[n] = 0; *len = (size_t)n;
    fclose(f);
    return b;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: nap_flow_test <repo root>\n"); return 2; }
    char path[1024]; size_t len;
    snprintf(path, sizeof(path), "%s/packages/cat/cat.json", argv[1]);
    char *cat = slurp(path, &len);
    CHECK(wm_creature("cat", cat, len) == 0, "the cat loads");
    CHECK(wm_start(466, 20261122, 0) == 0, "the module starts");
    CHECK(wm_state("{\"screen_off_battery_s\":10,\"screen_off_usb_s\":0}") == 0, "the nap's settings are taken");
    CHECK(wm_state("{\"screen_off_battery_s\":5}") != 0 && wm_state("{\"screen_off_usb_s\":4000}") != 0, "out of range refused");
    frames(30);
    s_dir.cfg.sleep_after_s = 3.f; s_dir.cfg.yawn_lead_s = 1.f;   /* (dozes off in seconds, not minutes) */

    /* the power not known yet (the web module never says): never */
    CHECK(until_asleep() && until_off(3 * NAP_S) < 0.f, "power unknown: the screen stays on");
    /* on USB set to never: never */
    mole_fluid_power(NAP_POWER_USB);
    CHECK(until_off(3 * NAP_S) < 0.f, "on USB, never: the screen stays on");
    /* on battery: off the set time after it fell asleep (here it slept already: counted from the power's change) */
    mole_fluid_power(NAP_POWER_BATTERY);
    float t = until_off(2 * NAP_S);
    CHECK(t > NAP_S - 0.5f && t < NAP_S + 0.5f, "on battery: off %.1f s on (%d s set)", t, NAP_S);

    /* the agent's mood ends the nap, the cat awake */
    CHECK(wm_state("{\"mood\":\"happy\"}") == 0, "a mood");
    frames(3);
    CHECK(on() && !fd_asleep(&s_dir), "a mood wakes the screen (%d) and the cat (%s)", on(), fd_asleep(&s_dir) ? "asleep" : "awake");

    /* a pick-up ends the next nap, the cat awake */
    CHECK(until_asleep() && until_off(2 * NAP_S) > 0.f, "the next nap");
    mole_fluid_lifted();
    frames(3);
    CHECK(on() && !fd_asleep(&s_dir), "a pick-up wakes the screen (%d) and the cat (%s)", on(), fd_asleep(&s_dir) ? "asleep" : "awake");

    /* a listen up: never; it ends: the set time from then */
    CHECK(until_asleep(), "asleep again");
    mole_fluid_listen(true);
    CHECK(until_off(3 * NAP_S) < 0.f, "a listen keeps the screen on");
    mole_fluid_listen(false);
    s_dir.asleep = true;                                   /* (a listen does not wake it: still asleep) */
    t = until_off(2 * NAP_S);
    CHECK(t > NAP_S - 0.5f && t < NAP_S + 0.5f, "after the listen: off %.1f s on", t);

    /* a question up over the sleeping cat: never; answered: the set time from then */
    wm_touch(1, 233, 233); frames(2); wm_touch(0, 0, 0); frames(2);   /* (a touch wakes it as before) */
    CHECK(on(), "a touch wakes the screen");
    CHECK(wm_ask("{\"title\":\"feed the cat?\",\"options\":[\"yes\",\"no\"]}") == 0, "a question");
    frames(3);
    s_dir.asleep = true;
    CHECK(until_off(3 * NAP_S) < 0.f, "a question keeps the screen on");
    wm_ask_end();
    frames(60);
    s_dir.asleep = true;
    t = until_off(2 * NAP_S);
    CHECK(t > NAP_S - 0.5f - 2.f && t < NAP_S + 0.5f, "after the question: off %.1f s on", t);

    /* the button on while it naps, the cat asleep: the timer starts again from then */
    mole_fluid_screen(true);
    frames(2);
    CHECK(on() && fd_asleep(&s_dir), "on by hand, the cat still asleep");
    t = until_off(2 * NAP_S);
    CHECK(t > NAP_S - 0.5f && t < NAP_S + 0.5f, "on by hand: off again %.1f s on", t);

    /* off by hand: the agent and a pick-up leave it off; a touch wakes it */
    mole_fluid_screen(true); frames(2);
    mole_fluid_screen(false); frames(2);
    CHECK(!on(), "off by hand");
    wm_state("{\"mood\":\"happy\"}"); mole_fluid_lifted(); frames(10);
    CHECK(!on(), "off by hand: a mood and a pick-up leave it off");
    wm_touch(1, 233, 233); frames(2); wm_touch(0, 0, 0); frames(2);
    CHECK(on(), "off by hand: a touch wakes it");
    /* a nap the human then turned off by hand (POST /api/power) stays off for the agent */
    CHECK(until_asleep() && until_off(2 * NAP_S) > 0.f, "a nap");
    mole_fluid_screen(false); frames(2);
    wm_state("{\"mood\":\"happy\"}"); frames(10);
    CHECK(!on(), "a nap turned off by hand: a mood leaves it off");

    printf("%d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
