/*
 * SPDX-License-Identifier: MIT
 *
 * beat_test.c: the rhythm package's clock on the firmware's own screen (the web module's native build: web_molty.c
 * included whole, its statics are this file's), and the wheel the finger turns.
 *   ring_input   clicks per angle, across 180 degrees, both ways, what is left over, near the centre, turning fast
 *   package      the rhythm package parses; a second beat, a bad `sets`, a literal `labels` are refused
 *   clock        its rings, marks and dots from the beat player's pattern; the hand on the step the player is at; the
 *                kept drawing (rings, marks, their dust) unchanged while the hand walks and the tempo changes; a new
 *                rhythm redraws it in place, the same look
 *   wheel        playing, a drag round the rim: 2 beats a minute a click, the rhythm keeps playing, the water untouched,
 *                no tap, a tick a click (the wheel's own; a fast turn one every 60 ms at most), one `ring` event when the
 *                finger leaves; the centre stops it at rest (the play mark, the hand at 12, silent), the rim then
 *                chooses (shown at once, silent, never back on its own), the centre plays it after a count-in; a
 *                dot's tap mutes its drum; the buttons play, stop and leave; no clock on screen stops the sound
 *   still        no change of picture (the dissolve: a new look) once the clock formed: every rhythm of the library
 *                four phrases untouched (every bar line, the count-in), and a whole session of turning, choosing,
 *                playing, stopping, muting and the agent's changes
 *   cost         the frame with a rhythm playing and the wheel turning, host time against the creature's
 *
 *   beat_test <repo root> <library.json>
 */
#include "../../web/web_molty.c"

#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int s_pass, s_fail;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define DT 16667u
static void frames(int n) { for (int i = 0; i < n; i++) wm_frame(DT); }

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "beat_test: cannot read %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    b[n] = 0; *len = (size_t)n;
    fclose(f);
    return b;
}

/* ── ring_input on its own ─────────────────────────────────────────────────────────────────────────────────────── */
static int turn(ring_drag_t *d, float from, float to, float r, float step, float secs, bool *fast) {
    int clicks = 0;
    const int n = 90;
    for (int q = 1; q <= n; q++) {
        const float a = (from + (to - from) * q / n) * 0.017453293f;
        clicks += ring_move(d, step, r * cosf(a), r * sinf(a), secs * q / n, fast);
    }
    return clicks;
}
static void test_ring_input(void) {
    ring_drag_t d;
    bool fast;
    CHECK(ring_grabs(0.85f, 0.f, 0.9f) && !ring_grabs(0.85f, 0.5f, 0.5f) && !ring_grabs(0.f, 0.f, 0.95f), "grab from r0 out");
    ring_down(&d, 0.f, 0.93f, 0.f);
    CHECK(turn(&d, 90.f, 54.f, 0.93f, 12.f, 1.f, &fast) == 3 && !fast, "36 degrees clockwise, slowly: 3 clicks");
    CHECK(turn(&d, 54.f, 90.f, 0.93f, 12.f, 2.f, &fast) == -3, "back again: -3");
    ring_down(&d, 0.93f * cosf(179.f * 0.017453293f), 0.93f * sinf(179.f * 0.017453293f), 0.f);   /* across 180 degrees */
    CHECK(turn(&d, 179.f, 244.f, 0.93f, 30.f, 1.f, &fast) == -2, "across 180 counter-clockwise, 65 degrees: -2 (30 a click)");
    ring_down(&d, 0.93f, 0.f, 0.f);
    CHECK(turn(&d, 0.f, -11.f, 0.93f, 12.f, 1.f, &fast) == 0 && turn(&d, -11.f, -13.f, 0.93f, 12.f, 1.f, &fast) == 1, "what is left over carries on");
    ring_down(&d, 0.f, 0.93f, 0.f);
    CHECK(turn(&d, 90.f, -90.f, 0.2f, 12.f, 1.f, &fast) == 0, "near the centre: nothing");
    ring_down(&d, 0.f, 0.93f, 0.f);
    const int c = turn(&d, 90.f, -30.f, 0.93f, 12.f, 0.25f, &fast);   /* 480 degrees a second */
    CHECK(c == 10 && fast, "turning fast: %d clicks, fast %d", c, fast);
}

/* ── the package ────────────────────────────────────────────────────────────────────────────────────────────────── */
static void test_package(const char *json, size_t len) {
    const char *err = NULL;
    pkg_t *p = pkg_parse(json, len, &err);
    CHECK(p && p->nlayouts == 1, "the rhythm package parses (%s)", err ? err : "");
    pkg_unref(p);
    static const char *const BAD[] = {
        "{\"v\":\"molty-pkg/1\",\"id\":\"x\",\"name\":\"X\",\"version\":\"1.0.0\",\"kind\":\"scenes\",\"layouts\":{\"a\":{\"dur\":0,\"items\":[{\"k\":\"beat\",\"r\":0.6},{\"k\":\"beat\",\"r\":0.5}]}}}",
        "{\"v\":\"molty-pkg/1\",\"id\":\"x\",\"name\":\"X\",\"version\":\"1.0.0\",\"kind\":\"scenes\",\"layouts\":{\"a\":{\"dur\":0,\"items\":[{\"k\":\"dial\",\"id\":\"t\",\"sets\":\"volume\"}]}}}",
        "{\"v\":\"molty-pkg/1\",\"id\":\"x\",\"name\":\"X\",\"version\":\"1.0.0\",\"kind\":\"scenes\",\"layouts\":{\"a\":{\"dur\":0,\"items\":[{\"k\":\"dial\",\"id\":\"t\",\"labels\":\"aries\"}]}}}",
        "{\"v\":\"molty-pkg/1\",\"id\":\"x\",\"name\":\"X\",\"version\":\"1.0.0\",\"kind\":\"scenes\",\"layouts\":{\"a\":{\"dur\":0,\"items\":[{\"k\":\"dial\",\"r\":0.9}]}}}",
        "{\"v\":\"molty-pkg/1\",\"id\":\"x\",\"name\":\"X\",\"version\":\"1.0.0\",\"kind\":\"scenes\",\"layouts\":{\"a\":{\"dur\":0,\"items\":[{\"k\":\"beat\",\"r\":0.6,\"group\":\"g\"}]}}}",
    };
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        pkg_t *b = pkg_parse(BAD[i], strlen(BAD[i]), &err);
        CHECK(!b, "bad package %zu refused", i);
        pkg_unref(b);
    }
    /* a generic list wheel: the astro signs, say: its labels from a list param */
    const char *LIST = "{\"v\":\"molty-pkg/1\",\"id\":\"x\",\"name\":\"X\",\"version\":\"1.0.0\",\"kind\":\"scenes\",\"layouts\":{\"a\":{\"dur\":0,"
                       "\"params\":{\"signs\":\"list\"},\"items\":[{\"k\":\"dial\",\"id\":\"sign\",\"step\":30,\"labels\":\"$signs\"}]}}}";
    pkg_t *lp = pkg_parse(LIST, strlen(LIST), &err);
    cJSON *pp = cJSON_Parse("{\"signs\":[{\"label\":\"ARIES\"},{\"label\":\"TAURUS\"},{\"label\":\"GEMINI\"}]}");
    scene_t *sc = lp ? scene_build(lp, "a", pp, 60, &err) : NULL;
    char labels[4][PKG_KEY_MAX + 1];
    CHECK(sc && scene_dial(sc, 0) == 0 && scene_dial_labels(sc, 0, labels, 4) == 3 && !strcmp(labels[1], "TAURUS"), "a list wheel takes its labels from a list param");
    scene_free(sc); cJSON_Delete(pp); pkg_unref(lp);
}

/* ── on the screen ──────────────────────────────────────────────────────────────────────────────────────────────── */
#define S 466
static void touch_at(int down, float x, float y) { wm_touch(down, (int)lrintf((x + 1.f) * 0.5f * S), (int)lrintf((1.f - y) * 0.5f * S)); }
static int count_items(const scene_t *sc, bool live, int k) {
    int n = 0;
    for (int q = sc->nfixed; q < sc->nitems; q++) n += sc->items[q].live == live && sc->items[q].k == k;
    return n;
}
static float hand_angle(const scene_t *sc) {
    for (int q = sc->nfixed; q < sc->nitems; q++) if (sc->items[q].live && sc->items[q].k == PK_TICKS && sc->items[q].r < 0.5f) return sc->items[q].a0;
    return -999.f;
}
static uint32_t dim_of(uint32_t c) { const float k = SCENE_BEAT_MUTED; return (uint32_t)lrintf((c >> 16 & 255) * k) << 16 | (uint32_t)lrintf((c >> 8 & 255) * k) << 8 | (uint32_t)lrintf((c & 255) * k); }
static const char *str(const cJSON *j) { const char *s = cJSON_GetStringValue(j); return s ? s : ""; }
static int events_named(const char *name, cJSON **last) {
    char *e = wm_events();
    int n = 0;
    cJSON *a = e ? cJSON_Parse(e) : NULL, *it;
    cJSON_ArrayForEach(it, a) {
        if (!strcmp(str(cJSON_GetObjectItem(it, "name")), name)) { n++; if (last) { cJSON_Delete(*last); *last = cJSON_Duplicate(it, true); } }
    }
    cJSON_Delete(a); free(e);
    return n;
}
static char LIB_ARGS[65536];
/* the library's ids, in its order (rhythm.play's pick) */
static char IDS[RHY_LIB][RHY_ID_MAX + 1];
static int NIDS;
static int P(const char *id) { for (int i = 0; i < NIDS; i++) if (!strcmp(IDS[i], id)) return i; fprintf(stderr, "beat_test: no %s\n", id); exit(2); }
static const char *pick(const char *id, const char *more) { static char b[128]; snprintf(b, sizeof b, "\"pick\":%d%s%s", P(id), more[0] ? "," : "", more); return b; }
static void show(const char *pick_bpm) {
    CHECK(wm_scene_op("clock", "{\"tick_ms\":0,\"tick\":0}", 600, 0) == 0, "the clock shows");
    static char args[sizeof LIB_ARGS + 256];
    snprintf(args, sizeof args, "%s,%s}", LIB_ARGS, pick_bpm);
    CHECK(wm_rhythm("rhythm.play", args) == 0, "rhythm.play");
}

static void test_clock(void) {
    show(pick("son-clave", "\"bpm\":104"));             /* son clave: 16 steps, 1 4 7 11 13 */
    frames(30);
    scene_t *sc = s_sp.cur;
    CHECK(sc && sc->beat >= 0, "the scene has its beat");
    if (!sc) return;
    CHECK(count_items(sc, false, PK_RING) == 1, "a ring a drum (%d)", count_items(sc, false, PK_RING));
    int marks = 0, dots = 0;
    for (int q = sc->nfixed; q < sc->nitems; q++) { const scene_item_t *it = &sc->items[q]; if (!it->live && it->k == PK_TICKS) marks++; if (it->layer >= 0) dots++; }
    CHECK(marks == 16 && dots == 5, "sixteen marks, five hits (%d, %d)", marks, dots);
    /* after the count-in (one bar, 2.3 s at 104): the hand follows the player's step, frame after frame */
    frames(90);
    frames(60);
    int bad = 0, steps_seen = 0, last = -1;
    const unsigned serial = sc->serial, look = sc->look, gen = s_sp.gen;
    for (int f = 0; f < 200; f++) {
        frames(1);
        rhy_view_t v; rhy_pattern_t pt;
        rhythm_now(&v, &pt);
        const float want = (float)(90.0 - 360.0 * rhy_onset(&pt, v.step) / pt.start[pt.steps]);
        if (v.step < 0 || fabsf(hand_angle(sc) - want) > 0.01f) bad++;
        if (v.step != last) { steps_seen++; last = v.step; }
    }
    CHECK(!bad && steps_seen > 15, "the hand on the player's step every frame (%d off, %d steps)", bad, steps_seen);
    /* the swell: a sounding hit's live dot is bigger than its kept one */
    float big = 0.f;
    for (int f = 0; f < 30; f++) { frames(1); for (int q = sc->nfixed; q < sc->nitems; q++) if (sc->items[q].live && sc->items[q].k == PK_DISC && sc->items[q].r > big && sc->items[q].r < 0.2f) big = sc->items[q].r; }
    CHECK(big > sc->items[sc->beat].size * 1.5f, "a hit swells as it sounds (%.3f)", big);
    /* the tempo changes at once and nothing is redrawn but the hand */
    CHECK(wm_rhythm("rhythm.set", "{\"bpm\":160}") == 0, "rhythm.set bpm");
    frames(60);
    CHECK(sc->serial == serial && s_sp.gen == gen && s_sp.cur == sc, "playing and a new tempo: the kept drawing stays (no dots, no dust redrawn)");
    rhy_view_t v; rhythm_now(&v, NULL);
    CHECK(v.bpm == 160.f && !v.count_in, "the new tempo, no restart");
    /* a new rhythm: the kept drawing changes, the scene stays (no forming from dust) */
    char set[64]; snprintf(set, sizeof set, "{%s}", pick("bell-12", ""));
    CHECK(wm_rhythm("rhythm.set", set) == 0, "rhythm.set pick");
    frames(10);
    CHECK(s_sp.gen == gen && sc->serial != serial && sc->look == look && count_items(sc, false, PK_RING) == 2, "a new rhythm: redrawn in place, the same scene, the same look");
    printf("  clock   son clave: 1 ring, 16 marks, 5 dots; the hand on the player's step %d frames running; tempo and rhythm change in place\n", 200);
}

/* the water's height after a drag round the rim, or the same frames with no finger */
static void water(void *out, bool drag) {
    for (int q = 0; q < 40; q++) {
        if (drag) { const float a = (90.f - q * 1.5f) * 0.017453293f; touch_at(1, 0.93f * cosf(a), 0.93f * sinf(a)); }
        frames(1);
    }
    if (drag) touch_at(0, 0.f, 0.f);
    frames(10);
    memcpy(out, s_sim->h, sizeof(int16_t) * (size_t)s_sim->N);
}
static void water_drag(void *out) { water(out, true); }
static void water_still(void *out) { water(out, false); }
static void forked(void (*fn)(void *out), void *out, size_t n) {
    int fd[2];
    if (pipe(fd)) exit(2);
    fflush(stdout);
    const pid_t pid = fork();
    if (pid < 0) exit(2);
    if (!pid) { close(fd[0]); fn(out); if (write(fd[1], out, n) != (ssize_t)n) _exit(3); _exit(0); }
    close(fd[1]);
    size_t got = 0;
    for (ssize_t r; got < n && (r = read(fd[0], (char *)out + got, n - got)) > 0; got += (size_t)r) {}
    close(fd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (got != n || !WIFEXITED(st) || WEXITSTATUS(st)) { fprintf(stderr, "beat_test: a case failed to run\n"); exit(2); }
}

/* a drag round the rim from `from` degrees, `deg` clockwise, `per` frames a degree (the finger lifts after) */
static void drag(float from, int deg, int per) {
    touch_at(1, 0.93f * cosf(from * 0.017453293f), 0.93f * sinf(from * 0.017453293f)); frames(1);
    for (int q = 1; q <= deg; q++) { const float an = (from - q) * 0.017453293f; touch_at(1, 0.93f * cosf(an), 0.93f * sinf(an)); frames(per); }
}
static void lift(void) { touch_at(0, 0.f, 0.f); frames(2); }
static void tap(float x, float y) { touch_at(1, x, y); frames(2); lift(); }
/* the play mark: a triangle of ten dots in the accent in the middle */
static int play_dots(const scene_t *sc) {
    int n = 0;
    for (int q = sc->nfixed; q < sc->nitems; q++) { const scene_item_t *it = &sc->items[q]; n += it->live && it->k == PK_DISC && it->rgb == SCENE_NOTCH_RGB && fabsf(it->x) < 0.1f && fabsf(it->y) < 0.1f; }
    return n;
}

static void test_wheel(void) {
    wm_rhythm("rhythm.set", "{\"pick\":0,\"bpm\":100}");
    frames(120);
    events_named("x", NULL);                            /* (what was heard so far) */
    wm_wheel_ticks(NULL, NULL);
    /* the water under a drag round the rim: exactly as with no finger */
    const size_t hn = sizeof(int16_t) * (size_t)s_sim->N;
    int16_t *a = malloc(hn), *b = malloc(hn);
    forked(water_drag, a, hn); forked(water_still, b, hn);
    int diff = 0;
    for (int k = 0; k < s_sim->N; k++) diff += a[k] != b[k];
    CHECK(!diff, "a drag round the rim leaves the water as it was (%d cells differ)", diff);
    free(a); free(b);
    /* playing: the rim is the tempo. 40 degrees clockwise, slowly: three clicks of 2, a tick each, the wheel's own */
    drag(90.f, 40, 2);
    rhy_view_t v; rhythm_now(&v, NULL);
    CHECK(v.bpm == 106.f && !v.count_in && v.on && s_wh.w.face == 0, "playing, 40 degrees: 100 -> 106 while it plays on (%.0f)", v.bpm);
    CHECK(scene_wheel_words(s_sp.cur, &s_wh.w), "the tempo in the middle while it turns");
    int ms = 0, st = 0;
    CHECK(wm_wheel_ticks(&ms, &st) == 3 && ms == HAPTIC_WHEEL_MS && st == HAPTIC_WHEEL_STRENGTH, "a slow turn of 3 clicks: 3 ticks, %d ms at %d percent", ms, st);
    cJSON *last = NULL;
    CHECK(events_named("tap", NULL) == 0, "no tap from a drag");
    lift();
    CHECK(events_named("ring", &last) == 1, "one ring event when it settles");
    const cJSON *d = cJSON_GetObjectItem(last, "data");
    CHECK(d && !strcmp(str(cJSON_GetObjectItem(d, "spot")), "tempo") && cJSON_GetObjectItem(d, "value")->valuedouble == 106., "ring {spot: tempo, value: 106}");
    frames(60);
    CHECK(!scene_wheel_words(s_sp.cur, &s_wh.w), "the number goes 0.9 s after the finger");
    /* a slow turn of twelve clicks, both ways: twelve ticks; a fast one: fewer ticks than clicks (60 ms apart at most) */
    drag(90.f, 75, 4); lift();
    touch_at(1, 0.93f * cosf(15.f * 0.017453293f), 0.93f * sinf(15.f * 0.017453293f)); frames(1);
    for (int q = 1; q <= 75; q++) { const float an = (15.f + q) * 0.017453293f; touch_at(1, 0.93f * cosf(an), 0.93f * sinf(an)); frames(4); }
    lift();
    const int slow = wm_wheel_ticks(NULL, NULL);
    CHECK(slow == 12, "a slow turn of 6 clicks each way: 12 ticks (%d)", slow);
    rhythm_now(&v, NULL);
    const float b0 = v.bpm;
    touch_at(1, 0.f, 0.93f); frames(1);
    for (int q = 1; q <= 15; q++) { const float an = (90.f - q * 15.f) * 0.017453293f; touch_at(1, 0.93f * cosf(an), 0.93f * sinf(an)); frames(1); }   /* 900 degrees a second */
    lift();
    const int fast = wm_wheel_ticks(NULL, NULL);
    rhythm_now(&v, NULL);
    CHECK(v.bpm > b0 + 12 && fast >= 2 && fast < 18, "a fast turn of 18 clicks: %d ticks, 60 ms apart at most (%.0f -> %.0f)", fast, b0, v.bpm);
    /* the clamp: 220 at most */
    wm_rhythm("rhythm.set", "{\"bpm\":218}"); frames(2);
    drag(90.f, 60, 2); lift();
    rhythm_now(&v, NULL);
    CHECK(v.bpm == 220.f, "220 at most (%.0f)", v.bpm);
    /* the centre: it stops, at rest: the rhythms on the rim, the play mark, the hand at 12, silent */
    wm_rhythm("rhythm.set", "{\"bpm\":100}"); frames(30);
    events_named("x", NULL); wm_wheel_ticks(NULL, NULL);
    tap(0.05f, 0.05f); frames(10);
    rhythm_now(&v, NULL);
    const scene_t *sc = s_sp.cur;
    CHECK(v.rest && !rhythm_sounding() && s_wh.w.face == 1 && s_wh.w.level == 0 && s_wh.w.nlabels == s_wh.count[s_wh.region] && s_wh.w.index == 0 &&
          s_wh.w.above && !strcmp(s_wh.w.above, "ARGENTINA"), "a centre tap: stopped at rest, Argentina's rhythms on the rim, its name above (face %d, %d labels)", s_wh.w.face, s_wh.w.nlabels);
    CHECK(play_dots(sc) == 10 && fabsf(hand_angle(sc) - 90.f) < 0.01f, "at rest: the play mark (%d dots), the hand at 12 o'clock (%.1f)", play_dots(sc), hand_angle(sc));
    CHECK(events_named("rhythm", &last) == 1 && !strcmp(str(cJSON_GetObjectItem(cJSON_GetObjectItem(last, "data"), "state")), "paused"), "rhythm {state: paused}");
    /* choosing: 60 degrees, two beads on: the clock shows carnavalito at once, still silent; one ring when it settles */
    drag(90.f, 60, 2);
    rhythm_now(&v, NULL);
    const int third = s_wh.map[2];
    CHECK(s_wh.w.index == 2 && v.cur == third && v.rest && !rhythm_sounding(), "60 degrees at rest: the region's third rhythm on the clock, silent");
    CHECK(wm_wheel_ticks(NULL, NULL) == 2, "a tick a bead");
    lift();
    CHECK(events_named("ring", &last) == 1 && !strcmp(str(cJSON_GetObjectItem(cJSON_GetObjectItem(last, "data"), "id")), IDS[third]),
          "ring {spot: rhythm, index: 2, id: %s}", IDS[third]);
    frames(300);                                        /* 5 s untouched: still choosing, never back on its own */
    rhythm_now(&v, NULL);
    CHECK(v.rest && s_wh.w.face == 1 && !rhythm_sounding() && scene_wheel_words(s_sp.cur, &s_wh.w), "5 s untouched: still at rest, its name up");
    /* the centre again: it plays the chosen one, after a count-in; the rim is the tempo again */
    tap(0.f, -0.05f); frames(4);
    rhythm_now(&v, NULL);
    CHECK(!v.rest && v.cur == third && v.count_in && rhythm_sounding() && s_wh.w.face == 0 && !play_dots(s_sp.cur), "a centre tap: the chosen one plays after a count-in, the tempo on the rim");
    CHECK(events_named("rhythm", &last) == 1 && !strcmp(str(cJSON_GetObjectItem(cJSON_GetObjectItem(last, "data"), "state")), "playing"), "rhythm {state: playing}");
    CHECK(wm_wheel_ticks(NULL, NULL) == 0, "the centre buzzes as a tap, never a wheel tick");
    /* a tap on a drum's dot mutes it; again, back */
    wm_rhythm("rhythm.set", "{\"pick\":0}"); frames(120);
    sc = s_sp.cur;
    float dx = 0.f, dy = 0.f;
    for (int q = sc->nfixed; q < sc->nitems; q++) if (sc->items[q].layer == 1) { dx = sc->items[q].x; dy = sc->items[q].y; break; }
    events_named("x", NULL);
    tap(dx, dy);
    rhythm_now(&v, NULL);
    CHECK(v.mute == 2u && events_named("tap", &last) == 1 && cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(last, "data"), "muted")), "a dot's tap: parche muted, a tap {spot: parche, muted: true}");
    int dim = 0;
    for (int q = sc->nfixed; q < sc->nitems; q++) if (sc->items[q].layer == 1 && sc->items[q].rgb == dim_of(0xffa36bu)) dim++;
    CHECK(dim == 2, "its dots at 30 percent (%d)", dim);
    tap(dx, dy);
    rhythm_now(&v, NULL);
    CHECK(v.mute == 0u, "tapped again: back");
    /* the buttons: accept stops and plays as the centre does, reject stops and the clock goes */
    events_named("x", NULL);
    wm_button(0, 1); frames(2); wm_button(0, 0); frames(20);
    rhythm_now(&v, NULL);
    CHECK(v.rest && s_wh.w.face == 1 && events_named("rhythm", &last) == 1, "accept: at rest, choosing");
    wm_button(0, 1); frames(2); wm_button(0, 0); frames(20);
    rhythm_now(&v, NULL);
    CHECK(!v.rest && v.count_in, "accept again: on, after a count-in");
    wm_button(1, 1); frames(2); wm_button(1, 0); frames(30);
    CHECK(!rhythm_active() && !s_sp.cur && events_named("button", NULL) == 0, "reject: stopped, the clock gone, no button event");
    /* the agent's `play --still`: at rest, choosing, the play mark up */
    show(pick("milonga", "\"bpm\":90,\"paused\":true"));
    frames(30);
    rhythm_now(&v, NULL);
    CHECK(v.rest && !rhythm_sounding() && s_wh.w.face == 1 && play_dots(s_sp.cur) == 10, "paused: true: at rest, choosing, the play mark");
    wm_rhythm("rhythm.set", "{\"paused\":false}"); frames(4);
    rhythm_now(&v, NULL);
    CHECK(!v.rest && v.count_in && s_wh.w.face == 0, "the agent's paused: false plays it, after a count-in");
    wm_rhythm("rhythm.stop", NULL); frames(30);
    /* no clock on screen: the sound stops by itself */
    show("\"pick\":0,\"bpm\":100");
    frames(30);
    wm_scene_clear(); frames(60);
    CHECK(rhythm_active(), "the clock gone a second: still playing");
    frames(90);
    CHECK(!rhythm_active(), "after 2 s with no clock: stopped");
    cJSON_Delete(last);
    printf("  wheel   playing: 40 degrees = +6 bpm (3 ticks of %d ms at %d percent), clamped at 220; centre: at rest, 60 degrees = carnavalito, centre: it plays; a dot mutes; buttons\n", HAPTIC_WHEEL_MS, HAPTIC_WHEEL_STRENGTH);
}

/* ── regions: a level above the rhythms while stopped ─────────────────────────────────────────────────────────────── */
static void test_regions(void) {
    wm_rhythm("rhythm.stop", NULL); wm_scene_clear(); frames(60);
    show(pick("zamba", "\"bpm\":80,\"paused\":true"));
    frames(60);
    cJSON *last = NULL;
    CHECK(s_wh.w.face == 1 && s_wh.w.level == 0 && s_wh.w.above && !strcmp(s_wh.w.above, "ARGENTINA") && s_wh.nregions >= 10,
          "at rest: the region's rhythms, its name above (%d regions)", s_wh.nregions);
    events_named("x", NULL);
    tap(0.f, SCENE_WHEEL_ABOVE_Y);                      /* the region's name: up a level */
    frames(4);
    CHECK(s_wh.w.level == 1 && s_wh.w.nlabels == s_wh.nregions && s_wh.w.index == 0 && s_wh.w.below && strstr(s_wh.w.below, "RHYTHMS") &&
          wm_closed_buzz() == 0, "a tap on it: the regions on the rim, Argentina under the notch, its size under its name (%s)", s_wh.w.below ? s_wh.w.below : "");
    int enter = 0;
    for (int q = s_sp.cur->nfixed; q < s_sp.cur->nitems; q++) enter += s_sp.cur->items[q].live && s_sp.cur->items[q].k == PK_DISC && s_sp.cur->items[q].rgb == SCENE_NOTCH_RGB;
    CHECK(enter == 9 && play_dots(s_sp.cur) != 10, "the centre shows the enter mark (a ring of dots), not the play mark (%d)", enter);
    rhy_view_t v; rhythm_now(&v, NULL);
    const int was = v.cur;
    drag(90.f, 60, 2);                                  /* two regions on */
    rhythm_now(&v, NULL);
    CHECK(s_wh.w.index == 2 && v.cur == was && !rhythm_sounding(), "turning the regions: the highlight moves, the clock keeps its rhythm, silent");
    lift();
    CHECK(events_named("ring", &last) == 1 && !strcmp(str(cJSON_GetObjectItem(cJSON_GetObjectItem(last, "data"), "spot")), "region") &&
          !strcmp(str(cJSON_GetObjectItem(cJSON_GetObjectItem(last, "data"), "label")), s_wh.regions[2]), "ring {spot: region, label: %s}", s_wh.regions[2]);
    tap(0.02f, -0.02f); frames(4);                      /* the centre: into it */
    rhythm_now(&v, NULL);
    CHECK(s_wh.w.level == 0 && s_wh.region == 2 && rhythm_region_of(v.cur) == 2 && v.rest && !rhythm_sounding() && !strcmp(s_wh.w.above, s_wh.regions[2]),
          "the centre: into %s, its first rhythm on the clock, still silent", s_wh.regions[2]);
    const int first = v.cur;
    drag(90.f, 33, 2); lift();                          /* one on, then up and back: the region remembers it */
    rhythm_now(&v, NULL);
    const int second = v.cur;
    tap(0.f, SCENE_WHEEL_ABOVE_Y); frames(4);
    tap(0.02f, -0.02f); frames(4);
    rhythm_now(&v, NULL);
    CHECK(second != first && v.cur == second && s_wh.region == 2, "up and back in: the region's last rhythm again");
    /* the agent's pick of another region's rhythm: the wheel follows to its region */
    char c[64]; snprintf(c, sizeof c, "{%s}", pick("teental", ""));
    wm_rhythm("rhythm.set", c); frames(4);
    CHECK(s_wh.w.level == 0 && s_wh.region == rhythm_region_of(P("teental")) && !strcmp(s_wh.w.above, "INDIA"), "the agent picks teental: the wheel on India");
    tap(0.02f, -0.02f); frames(10);
    rhythm_now(&v, NULL);
    CHECK(!v.rest && v.cur == P("teental") && s_wh.w.face == 0 && !s_wh.w.above, "the centre plays it; playing, no region label");
    cJSON_Delete(last);
    printf("  regions %d regions: the label above goes up, the rim steps through regions, the centre enters (its last rhythm), the agent's pick follows\n", s_wh.nregions);
}

/* ── no change of picture while the clock is up ─────────────────────────────────────────────────────────────────────
 * What the device logs as `look ... change`, `change f=` and `anim wave` is the drawer's change of picture (the dissolve,
 * fluid_morph_t): counted here frame by frame, from the drawer itself, with every new look it is told. */
typedef struct { int changes, looks; uint32_t look; } trans_t;
static trans_t s_tr;
static void watch(int n) {
    for (int i = 0; i < n; i++) {
        const bool was = s_draw->m.on;
        wm_frame(DT);
        if (s_draw->m.on && !was) s_tr.changes++;
        const uint32_t lk = s_draw->m.look & ~FLUID_LOOK_DUST;
        if (lk != s_tr.look) { s_tr.looks++; s_tr.look = lk; }
    }
}
/* the clock shown and formed: its first change of picture over, the counts from zero */
static void formed(void) {
    for (int i = 0; i < 900 && (fluid_draw_morphing(s_draw) || i < 30); i++) watch(1);
    watch(30);
    s_tr = (trans_t){ 0, 0, s_draw->m.look & ~FLUID_LOOK_DUST };
}
static int lib_index(const char *id) {
    char got[RHY_ID_MAX + 1];
    for (int i = 0; i < RHY_LIB; i++) if (!strcmp(rhythm_id(i, got), id)) return i;
    return -1;
}
static void test_still_picture(void) {
    wm_device(1);
    wm_rhythm("rhythm.stop", NULL); wm_scene_clear(); frames(120);
    /* every rhythm of the library, untouched for four whole phrases at 220 (the count-in, every bar line, the phrase) */
    char args[64], ids[RHY_LIB][RHY_ID_MAX + 1];
    int n = 0;
    show("\"pick\":0,\"bpm\":220");
    for (; n < RHY_LIB && rhythm_id(n, ids[n])[0]; n++) {}
    int bad = 0;
    for (int i = 0; i < n; i++) {
        snprintf(args, sizeof args, "\"pick\":%d,\"bpm\":220", i);
        wm_rhythm("rhythm.stop", NULL); wm_scene_clear(); frames(60);
        show(args);
        formed();
        wm_rhythm("rhythm.set", "{\"paused\":true}"); watch(10);   /* (stopped and played again: its count-in in view too) */
        wm_rhythm("rhythm.set", "{\"paused\":false}"); watch(1);
        rhy_view_t v; rhy_pattern_t pt;
        rhythm_now(&v, &pt);
        const int bars = 4 * (pt.norder > 1 ? pt.norder : 1) + 1;
        int lines = 0, prev = -1, f = 0;
        const bool counted = v.count_in;
        for (; f < 20000 && lines < bars; f++) {
            watch(1);
            rhythm_now(&v, NULL);
            if (v.step >= 0 && v.step < prev) lines++;   /* a bar line: the hand back at the top */
            if (v.step >= 0) prev = v.step;
        }
        CHECK(counted && lines >= bars, "%s: the count-in and %d bar lines seen (%d)", ids[i], bars, lines);
        if (s_tr.changes || s_tr.looks) { bad++; printf("    %s: %d changes of picture, %d new looks in %d frames\n", ids[i], s_tr.changes, s_tr.looks, f); }
    }
    CHECK(!bad, "every rhythm playing untouched four phrases: no change of picture (%d of %d had one)", bad, n);
    /* the chacarera: the tempo turned on the rim, the number and its fade */
    wm_rhythm("rhythm.stop", NULL); wm_scene_clear(); frames(60);
    snprintf(args, sizeof args, "\"pick\":%d,\"bpm\":100", lib_index("chacarera"));
    show(args);
    formed();
    const struct { const char *what; int k; float a, b; const char *cmd; } STEPS[] = {
        { "the tempo turned and settled", 1, 90.f, 60.f, NULL },
        { "the centre: stopped, choosing", 2, 0.05f, 0.05f, NULL },
        { "the region's name: up to the regions", 2, 0.f, SCENE_WHEEL_ABOVE_Y, NULL },
        { "a region turned", 1, 90.f, 33.f, NULL },
        { "the centre: into it", 2, 0.02f, -0.02f, NULL },
        { "up again", 2, 0.f, SCENE_WHEEL_ABOVE_Y, NULL },
        { "back, into the first", 1, 90.f, -33.f, NULL },
        { "the centre: into Argentina", 2, 0.02f, -0.02f, NULL },
        { "a rhythm chosen on the rim, both ways", 1, 90.f, 90.f, NULL },
        { "... and back", 1, 0.f, -60.f, NULL },
        { "the centre: it plays", 2, 0.f, -0.05f, NULL },
        { "a drum muted", 3, 1.f, 0.f, NULL },
        { "... and back", 3, 1.f, 0.f, NULL },
        { "the agent's tempo", 0, 0.f, 0.f, "{\"bpm\":150}" },
        { "the agent's rhythm", 0, 0.f, 0.f, NULL },
        { "the agent's rhythm, a phrase", 0, 0.f, 0.f, "{\"pick\":0}" },
        { "the accept button: stopped", 4, 0.f, 0.f, NULL },
        { "the accept button: it plays", 4, 0.f, 0.f, NULL },
    };
    int bad_steps = 0;
    for (size_t q = 0; q < sizeof(STEPS) / sizeof(STEPS[0]); q++) {
        const int k = STEPS[q].k;
        if (k == 1) {                                   /* a drag: from a, b degrees (negative: the other way) */
            touch_at(1, 0.93f * cosf(STEPS[q].a * 0.017453293f), 0.93f * sinf(STEPS[q].a * 0.017453293f)); watch(1);
            const int n = (int)fabsf(STEPS[q].b), sg = STEPS[q].b < 0 ? -1 : 1;
            for (int d = 1; d <= n; d++) { const float an = (STEPS[q].a - sg * d) * 0.017453293f; touch_at(1, 0.93f * cosf(an), 0.93f * sinf(an)); watch(2); }
            touch_at(0, 0.f, 0.f);
        } else if (k == 2) { touch_at(1, STEPS[q].a, STEPS[q].b); watch(2); touch_at(0, 0.f, 0.f); }
        else if (k == 3) {                              /* the first drum's first dot */
            const scene_t *sc = s_sp.cur;
            for (int i = sc->nfixed; i < sc->nitems; i++) if (sc->items[i].layer == 0) { touch_at(1, sc->items[i].x, sc->items[i].y); break; }
            watch(2); touch_at(0, 0.f, 0.f);
        } else if (k == 4) { wm_button(0, 1); watch(2); wm_button(0, 0); }
        else if (STEPS[q].cmd) wm_rhythm("rhythm.set", STEPS[q].cmd);
        else { char c[64]; snprintf(c, sizeof c, "{%s}", pick("son-clave", "")); wm_rhythm("rhythm.set", c); }
        watch(150);                                     /* (2.5 s: the words fade, a bar or two goes by) */
        if (s_tr.changes || s_tr.looks) { bad_steps++; printf("    %s: %d changes of picture, %d new looks\n", STEPS[q].what, s_tr.changes, s_tr.looks); }
        s_tr.changes = s_tr.looks = 0;
    }
    rhy_view_t v; rhythm_now(&v, NULL);
    CHECK(!bad_steps && !v.rest && v.cur == 0 && s_sp.cur, "turning, choosing, playing, stopping, muting, the agent's changes: no change of picture (%d steps had one)", bad_steps);
    wm_device(0);
    printf("  still   %d rhythms, four phrases each untouched; a whole session on the chacarera: the picture never changed whole\n", n);
}

static double now_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e6 + ts.tv_nsec * 1e-3; }
/* the device's time for a frame on its measured costs (web_device.c device mode: the 1.75C panel's windows and pixels,
 * the cells pass, the sim), averaged over 210 frames; turn: the wheel turning all the while */
static double device_ms(bool turn, double *worst) {
    wm_device(1);
    double sum = 0.;
    int n = 0;
    uint32_t dt = DT;
    *worst = 0.;
    for (int q = 0; q < 240; q++) {
        const float an = (90.f - q * 2.f) * 0.017453293f;
        if (turn) touch_at(1, 0.93f * cosf(an), 0.93f * sinf(an));
        wm_frame(dt);
        const float ms = wm_frame_info()[FI_FRAME_MS];
        if (q >= 30) { sum += ms; n++; if (ms > *worst) *worst = ms; }
    }
    if (turn) touch_at(0, 0.f, 0.f);
    wm_device(0);
    return sum / n;
}
/* the wheel turning ten seconds with the bell at 220 (device mode: the panel keeps what is not sent): every frame the
 * hand is drawn for that frame's own step, and the drawer owes no cell to the next frame (an owed cell shows the frame
 * before: the middle lagging, the hand a frame late) */
static void test_turn_fresh(void) {
    wm_rhythm("rhythm.stop", NULL); wm_scene_clear(); frames(60);
    show(pick("bell-12", "\"bpm\":220"));
    formed();
    wm_device(1);
    int late[2] = { 0, 0 }, owed[2] = { 0, 0 };
    double px[2] = { 0, 0 }; int pxmax[2] = { 0, 0 };   /* the pixels drawn into the glass a frame: the work, untimed */
    for (int turning = 0; turning < 2; turning++) {     /* ten seconds still, then ten seconds turning */
        float a = 90.f;
        if (turning) { touch_at(1, 0.f, 0.93f); wm_frame(DT); }
        for (int f = 0; f < 600; f++) {
            if (turning) {
                a += f % 200 >= 100 ? 2.f : -2.f;          /* 120 degrees a second, both ways in turn */
                touch_at(1, 0.93f * cosf(a * 0.017453293f), 0.93f * sinf(a * 0.017453293f));
            }
            wm_frame(DT);
            rhy_view_t v; rhy_pattern_t pt;
            rhythm_now(&v, &pt);
            const float want = (float)(90.0 - 360.0 * rhy_onset(&pt, v.step) / pt.start[pt.steps]);
            if (v.step >= 0 && fabsf(hand_angle(s_sp.cur) - want) > 0.01f) late[turning]++;
            owed[turning] += s_draw->nowed > 0;
            px[turning] += s_draw->ndrawn + s_draw->nowed * 64; if (s_draw->ndrawn > pxmax[turning]) pxmax[turning] = s_draw->ndrawn;
        }
        if (turning) { touch_at(0, 0.f, 0.f); frames(2); }
    }
    wm_device(0);
    /* the work a frame of the turn adds, in pixels drawn (the drawer's budget is timed, on the device and in the model,
     * so the frames that owe a cell vary with the computer's load: reported, not checked) */
    px[0] /= 600; px[1] /= 600;
    CHECK(!late[0] && !late[1], "ten seconds of turning: the hand on its own frame's step every frame (%d not)", late[1]);
    CHECK(px[1] <= px[0] * 1.3 && pxmax[1] <= 20000, "a frame of the turn draws about what a still frame does: %.0f px against %.0f, at most %d (the drawer takes 40000)", px[1], px[0], pxmax[1]);
    printf("  turn    600 frames turning with the bell at 220: the hand on each frame's own step; %.0f px drawn a frame (%.0f still, at most %d), %d frames owed a cell (%d still)\n", px[1], px[0], pxmax[1], owed[1], owed[0]);
}

/* ── closing any scene by hand: the reject button closes it whole, a finger held still a second closes one level ── */
static const char *scene_why(cJSON **last) {
    if (events_named("scene", last) < 1) return "";
    return str(cJSON_GetObjectItem(cJSON_GetObjectItem(*last, "data"), "why"));
}
static void hold_at(float x, float y, int n) { touch_at(1, x, y); frames(n); }
static void test_close(const char *rhythm_pkg, size_t rlen) {
    /* a plain package: a chart whose spot opens a close-up (the astrology skill's shape), no beat, no wheel */
    static const char *PKG = "{\"v\":\"molty-pkg/1\",\"id\":\"plain\",\"name\":\"Plain\",\"version\":\"1.0.0\",\"kind\":\"scenes\",\"layouts\":{"
        "\"chart\":{\"dur\":0,\"items\":[{\"k\":\"ring\",\"r\":0.6},{\"k\":\"spot\",\"x\":0.4,\"y\":0,\"r\":0.12,\"say\":\"a planet\",\"id\":\"mars\",\"open\":\"close\"}]},"
        "\"close\":{\"dur\":0,\"items\":[{\"k\":\"ring\",\"r\":0.3},{\"k\":\"disc\",\"r\":0.1}]}}}";
    wm_rhythm("rhythm.stop", NULL); wm_scene_clear(); frames(90);
    CHECK(wm_package(PKG, strlen(PKG)) == 0, "a plain package");
    cJSON *last = NULL;
    /* the button: no scene up, it does as before (bound to the screen or an event); a scene up, it closes it */
    const int bound = gesture_binds_get(mole_fluid_gestures(), GESTURE_IN_BUTTON_REJECT);
    events_named("x", NULL);
    wm_button(1, 1); frames(2); wm_button(1, 0); frames(40);
    const int before = events_named("button", NULL);
    CHECK(!s_sp.cur && (bound != GESTURE_DO_EVENT || before == 1) && (bound != GESTURE_DO_SCREEN || !mole_fluid_screen_on()),
          "no scene: the reject button does what it is bound to (%d: %d events, screen %d)", bound, before, mole_fluid_screen_on());
    frames(60);
    if (bound == GESTURE_DO_SCREEN) { wm_button(1, 1); frames(2); wm_button(1, 0); frames(60); }   /* (the screen back on) */
    CHECK(wm_scene_op("chart", "{}", 600, 0) == 0, "the chart shows"); frames(90);
    events_named("x", NULL);
    wm_button(1, 1); frames(2); wm_button(1, 0); frames(10);
    CHECK(!s_sp.cur && !strcmp(scene_why(&last), "close") && events_named("button", NULL) == 0 && wm_closed_buzz() == 0,
          "a scene up: the reject button closes it, a scene {why: close}, no button event, no buzz");
    frames(90);
    /* the hold: a tap and an early release never close; a second still closes one level, then the next */
    wm_scene_op("chart", "{}", 600, 0); frames(90);
    events_named("x", NULL);
    hold_at(-0.3f, 0.3f, 3); touch_at(0, 0.f, 0.f); frames(30);
    CHECK(s_sp.cur && events_named("scene", NULL) == 0, "a tap: nothing closes");
    hold_at(-0.3f, 0.3f, 45); touch_at(0, 0.f, 0.f); frames(30);
    CHECK(s_sp.cur && events_named("scene", NULL) == 0, "held 0.75 s and let go: nothing closes");
    touch_at(1, -0.3f, 0.3f); frames(20);
    for (int q = 0; q < 40; q++) { touch_at(1, -0.3f + q * 0.004f, 0.3f); frames(1); }   /* drifts 0.16: a drag */
    touch_at(0, 0.f, 0.f); frames(30);
    CHECK(s_sp.cur && events_named("scene", NULL) == 0, "a finger that moves: nothing closes");
    touch_at(1, 0.4f, 0.f); frames(2); touch_at(0, 0.f, 0.f); frames(60);   /* the spot */
    CHECK(s_sp.cur && !strcmp(s_sp.cur->layout, "close"), "a tap on the spot opens the close-up");
    /* held on the close-up's own drawing: back to the chart (a press beside it goes back at once, as a tap does) */
    events_named("x", NULL);
    hold_at(0.f, 0.f, 70);
    touch_at(0, 0.f, 0.f); frames(10);
    CHECK(s_sp.cur && !strcmp(s_sp.cur->layout, "chart") && !strcmp(scene_why(&last), "close") &&
          !strcmp(str(cJSON_GetObjectItem(cJSON_GetObjectItem(last, "data"), "layout")), "chart") && wm_closed_buzz() == 1,
          "held a second in the close-up: back to the chart, a scene {why: close, layout: chart}, one touch buzz");
    touch_at(1, 0.4f, 0.f); frames(2); touch_at(0, 0.f, 0.f); frames(60);
    events_named("x", NULL);
    hold_at(-0.5f, -0.5f, 70); touch_at(0, 0.f, 0.f); frames(10);
    CHECK(s_sp.cur && !strcmp(s_sp.cur->layout, "chart") && !strcmp(scene_why(&last), "back") && wm_closed_buzz() == 0,
          "held beside the close-up: back one level only (its press went back), never two");
    hold_at(0.f, -0.5f, 70); touch_at(0, 0.f, 0.f); frames(10);
    CHECK(!s_sp.cur && !strcmp(scene_why(&last), "close") && !cJSON_GetObjectItem(cJSON_GetObjectItem(last, "data"), "layout"),
          "held again at the top: the creature back, a scene {why: close} with no layout");
    /* the ring of dots while it fills: none at first, some halfway */
    wm_scene_op("chart", "{}", 600, 0); frames(90);
    touch_at(1, 0.2f, -0.4f); frames(8);
    const int n0 = s_nshp; frames(30);
    const int n1 = s_nshp;
    touch_at(0, 0.f, 0.f); frames(10);
    CHECK(n1 >= n0 + SCENE_HOLD_DOTS, "while it is held a ring of dots shows (%d shapes, then %d)", n0, n1);
    wm_scene_clear(); frames(90);
    /* the rhythm: a slow drag round the rim and a centre tap never close it; a hold does, and the rhythm stops */
    CHECK(wm_package(rhythm_pkg, rlen) == 0, "the rhythm package again");
    show("\"pick\":0,\"bpm\":100");
    frames(120);
    events_named("x", NULL);
    touch_at(1, 0.f, 0.93f); frames(1);
    for (int q = 1; q <= 30; q++) { const float an = (90.f - q * 0.4f) * 0.017453293f; touch_at(1, 0.93f * cosf(an), 0.93f * sinf(an)); frames(4); }   /* 12 degrees in 2 s */
    touch_at(0, 0.f, 0.f); frames(4);
    CHECK(s_sp.cur && events_named("scene", NULL) == 0, "a slow drag round the rim never counts as a hold");
    tap(0.03f, 0.03f); frames(20); tap(0.03f, 0.03f); frames(20);
    CHECK(s_sp.cur && events_named("scene", NULL) == 0 && rhythm_active(), "the centre's taps never close it");
    hold_at(-0.4f, 0.1f, 70); touch_at(0, 0.f, 0.f); frames(40);
    CHECK(!s_sp.cur && !rhythm_active() && !strcmp(scene_why(&last), "close"), "a hold closes the clock, the rhythm stops with it");
    cJSON_Delete(last);
    printf("  close   the reject button closes any scene (as bound with none); a still finger for %.1f s closes a level; taps, a drag, the rim and an early lift never do\n", SCENE_HOLD_S);
}

static void test_cost(void) {
    wm_scene_clear(); frames(120);
    double w0, w1, w2;
    const double dev_creature = device_ms(false, &w0);
    double t0 = now_us();
    frames(120);
    const double creature = (now_us() - t0) / 120;
    show(pick("bell-12", "\"bpm\":220"));                      /* the bell 12/8, the busiest */
    frames(120);
    t0 = now_us();
    touch_at(1, 0.f, 0.93f);
    for (int q = 0; q < 120; q++) { const float an = (90.f - q * 3.f) * 0.017453293f; touch_at(1, 0.93f * cosf(an), 0.93f * sinf(an)); frames(1); }
    touch_at(0, 0.f, 0.f);
    const double beat = (now_us() - t0) / 120;
    printf("  cost    a frame (sim + drawer, this computer): the creature %.0f us, the bell 12/8 at 220 with the wheel turning %.0f us\n", creature, beat);
    CHECK(beat < creature * 3.f + 2000.f, "the clock costs about what a scene does");
    const double dev_play = device_ms(false, &w1), dev_turn = device_ms(true, &w2);
    printf("  device  the model of the 1.75C (device mode): the creature %.1f ms a frame (worst %.1f), the bell playing %.1f (%.1f), playing with the wheel turning %.1f (%.1f)\n",
           dev_creature, w0, dev_play, w1, dev_turn, w2);
    CHECK(dev_play <= dev_creature && dev_turn <= dev_creature, "the clock playing, the wheel turning: no dearer a frame than the creature, which holds 30 fps on the device");
    wm_rhythm("rhythm.stop", NULL); frames(30);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: beat_test <repo root> <library.json>\n"); return 2; }
    test_ring_input();
    char path[1024];
    size_t len;
    snprintf(path, sizeof path, "%s/packages/rhythm/rhythm.json", argv[1]);
    char *pkg = slurp(path, &len);
    test_package(pkg, len);
    size_t ll;
    char *lib = slurp(argv[2], &ll);
    while (ll && (lib[ll - 1] == '\n' || lib[ll - 1] == ' ')) lib[--ll] = 0;
    snprintf(LIB_ARGS, sizeof LIB_ARGS, "%.*s", (int)ll - 1, lib);      /* ({regions, rhythms} without its last brace) */
    {
        cJSON *L = cJSON_Parse(lib), *r;
        cJSON_ArrayForEach(r, cJSON_GetObjectItem(L, "rhythms")) snprintf(IDS[NIDS++], RHY_ID_MAX + 1, "%s", str(cJSON_GetObjectItem(r, "id")));
        cJSON_Delete(L);
    }
    free(lib);
    if (wm_start(S, 7, 0) != 0 || wm_package(pkg, len) != 0) { fprintf(stderr, "beat_test: no start\n"); return 2; }
    wm_sense(1);
    frames(60);
    test_clock();
    test_wheel();
    test_regions();
    test_still_picture();
    test_turn_fresh();
    test_close(pkg, len);
    test_cost();
    free(pkg);
    printf("beat_test: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
