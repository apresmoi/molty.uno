/*
 * SPDX-License-Identifier: MIT
 *
 * beat_test.c: the rhythm package's clock on the firmware's own screen (the web module's native build: web_molty.c
 * included whole, its statics are this file's), and the wheel the finger turns.
 *   ring_input   clicks per angle, across 180 degrees, both ways, what is left over, near the centre, turning fast
 *   package      the rhythm package parses; a second beat, a bad `sets`, a literal `labels` are refused
 *   clock        its rings, marks and dots from the beat player's pattern; the hand on the step the player is at; the
 *                kept drawing (its dots, its dust) unchanged while the hand walks and the tempo changes: the hand and
 *                the swells are live shapes; a new rhythm redraws the kept part but never forms the scene from dust
 *   wheel        a drag round the rim: 2 beats a minute a click, the rhythm keeps playing, the water untouched, no tap,
 *                one `ring` event when the finger leaves; the centre's tap and the 4 s wait choose a rhythm; a dot's
 *                tap mutes its drum; the buttons pause and stop; no clock on screen stops the sound
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
    for (int q = sc->nfixed; q < sc->nitems; q++) if (sc->items[q].live && sc->items[q].k == PK_TICKS && sc->items[q].w > 0.02f && sc->items[q].r < 0.2f) return sc->items[q].a0;
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
static char LIB_ARGS[8192];
static void show(const char *pick_bpm) {
    CHECK(wm_scene_op("clock", "{}", 600, 0) == 0, "the clock shows");
    char args[9000];
    snprintf(args, sizeof args, "%s,%s}", LIB_ARGS, pick_bpm);
    CHECK(wm_rhythm("rhythm.play", args) == 0, "rhythm.play");
}

static void test_clock(void) {
    show("\"pick\":6,\"bpm\":104");                     /* son clave: 16 steps, 1 4 7 11 13 */
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
    const unsigned serial = sc->serial, gen = s_sp.gen;
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
    CHECK(wm_rhythm("rhythm.set", "{\"pick\":9}") == 0, "rhythm.set pick");
    frames(10);
    CHECK(s_sp.gen == gen && sc->serial != serial && count_items(sc, false, PK_RING) == 2, "a new rhythm: redrawn in place, the same scene");
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

static void test_wheel(void) {
    wm_rhythm("rhythm.set", "{\"pick\":0,\"bpm\":100}");
    frames(120);
    events_named("x", NULL);                            /* (what was heard so far) */
    wm_wheel_ticks();
    /* the water under a drag round the rim: exactly as with no finger */
    const size_t hn = sizeof(int16_t) * (size_t)s_sim->N;
    int16_t *a = malloc(hn), *b = malloc(hn);
    forked(water_drag, a, hn); forked(water_still, b, hn);
    int diff = 0;
    for (int k = 0; k < s_sim->N; k++) diff += a[k] != b[k];
    CHECK(!diff, "a drag round the rim leaves the water as it was (%d cells differ)", diff);
    free(a); free(b);
    /* 40 degrees clockwise, slowly: three clicks of 2 */
    float start = 90.f;
    touch_at(1, 0.93f * cosf(start * 0.017453293f), 0.93f * sinf(start * 0.017453293f));
    frames(1);
    for (int q = 1; q <= 40; q++) { const float an = (start - q) * 0.017453293f; touch_at(1, 0.93f * cosf(an), 0.93f * sinf(an)); frames(2); }
    rhy_view_t v; rhythm_now(&v, NULL);
    CHECK(v.bpm == 106.f && !v.count_in && v.on, "40 degrees: 100 -> 106 while it plays on (%.0f)", v.bpm);
    CHECK(scene_wheel_words(s_sp.cur, &s_wh.w), "the tempo in the middle while it turns");
    CHECK(wm_wheel_ticks() == 3, "a tick a click");
    cJSON *last = NULL;
    CHECK(events_named("tap", NULL) == 0, "no tap from a drag");
    touch_at(0, 0.f, 0.f); frames(2);
    CHECK(events_named("ring", &last) == 1, "one ring event when it settles");
    const cJSON *d = cJSON_GetObjectItem(last, "data");
    CHECK(d && !strcmp(str(cJSON_GetObjectItem(d, "spot")), "tempo") && cJSON_GetObjectItem(d, "value")->valuedouble == 106., "ring {spot: tempo, value: 106}");
    frames(60);
    CHECK(!scene_wheel_words(s_sp.cur, &s_wh.w), "the number goes 0.9 s after the finger");
    /* the clamp: 220 at most */
    wm_rhythm("rhythm.set", "{\"bpm\":218}"); frames(2);
    touch_at(1, 0.f, 0.93f); frames(1);
    for (int q = 1; q <= 60; q++) { const float an = (90.f - q) * 0.017453293f; touch_at(1, 0.93f * cosf(an), 0.93f * sinf(an)); frames(2); }
    touch_at(0, 0.f, 0.f); frames(2);
    rhythm_now(&v, NULL);
    CHECK(v.bpm == 220.f, "220 at most (%.0f)", v.bpm);
    /* the centre: the library on the rim; two clicks on, then 4 s untouched plays it */
    events_named("x", NULL);
    touch_at(1, 0.05f, 0.05f); frames(2); touch_at(0, 0.f, 0.f); frames(2);
    CHECK(s_wh.w.face == 1 && s_wh.w.nlabels == 12 && s_wh.w.index == 0, "a centre tap: the rhythms on the rim (face %d, %d labels)", s_wh.w.face, s_wh.w.nlabels);
    touch_at(1, 0.f, 0.93f); frames(1);
    for (int q = 1; q <= 60; q++) { const float an = (90.f - q) * 0.017453293f; touch_at(1, 0.93f * cosf(an), 0.93f * sinf(an)); frames(2); }
    touch_at(0, 0.f, 0.f); frames(2);
    CHECK(s_wh.w.index == 2 && events_named("ring", NULL) == 0, "60 degrees: the third rhythm, nothing sent yet");
    rhythm_now(&v, NULL);
    CHECK(v.cur == 0, "it still plays the first while you choose");
    frames(250);                                       /* 4 s untouched */
    rhythm_now(&v, NULL);
    CHECK(s_wh.w.face == 0 && v.cur == 2 && v.count_in, "4 s later: back, playing the chosen one after a count-in");
    CHECK(events_named("ring", &last) == 1 && !strcmp(str(cJSON_GetObjectItem(cJSON_GetObjectItem(last, "data"), "id")), "carnavalito"),
          "ring {spot: rhythm, index: 2, id: carnavalito}");
    /* a tap on a drum's dot mutes it; again, back */
    wm_rhythm("rhythm.set", "{\"pick\":0}"); frames(120);
    const scene_t *sc = s_sp.cur;
    float dx = 0.f, dy = 0.f;
    for (int q = sc->nfixed; q < sc->nitems; q++) if (sc->items[q].layer == 1) { dx = sc->items[q].x; dy = sc->items[q].y; break; }
    events_named("x", NULL);
    touch_at(1, dx, dy); frames(2); touch_at(0, 0.f, 0.f); frames(2);
    rhythm_now(&v, NULL);
    CHECK(v.mute == 2u && events_named("tap", &last) == 1 && cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(last, "data"), "muted")), "a dot's tap: parche muted, a tap {spot: parche, muted: true}");
    int dim = 0;
    for (int q = sc->nfixed; q < sc->nitems; q++) if (sc->items[q].layer == 1 && sc->items[q].rgb == dim_of(0xff9a7au)) dim++;
    CHECK(dim == 2, "its dots at 30 percent (%d)", dim);
    touch_at(1, dx, dy); frames(2); touch_at(0, 0.f, 0.f); frames(2);
    rhythm_now(&v, NULL);
    CHECK(v.mute == 0u, "tapped again: back");
    /* the buttons: accept pauses and resumes, reject stops and the clock goes */
    events_named("x", NULL);
    wm_button(0, 1); frames(2); wm_button(0, 0); frames(20);
    rhythm_now(&v, NULL);
    CHECK(v.paused && events_named("rhythm", &last) == 1, "accept: paused");
    wm_button(0, 1); frames(2); wm_button(0, 0); frames(20);
    rhythm_now(&v, NULL);
    CHECK(!v.paused && v.count_in, "accept again: on, after a count-in");
    wm_button(1, 1); frames(2); wm_button(1, 0); frames(30);
    CHECK(!rhythm_active() && !s_sp.cur && events_named("button", NULL) == 0, "reject: stopped, the clock gone, no button event");
    /* no clock on screen: the sound stops by itself */
    show("\"pick\":0,\"bpm\":100");
    frames(30);
    wm_scene_clear(); frames(60);
    CHECK(rhythm_active(), "the clock gone a second: still playing");
    frames(90);
    CHECK(!rhythm_active(), "after 2 s with no clock: stopped");
    cJSON_Delete(last);
    printf("  wheel   40 degrees = +6 bpm (3 ticks), clamped at 220; centre + 60 degrees + 4 s = carnavalito; a dot mutes; buttons pause and stop\n");
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
        dt = (uint32_t)(ms * 1000.f);
        if (q >= 30) { sum += ms; n++; if (ms > *worst) *worst = ms; }
    }
    if (turn) touch_at(0, 0.f, 0.f);
    wm_device(0);
    return sum / n;
}
static void test_cost(void) {
    wm_scene_clear(); frames(120);
    double w0, w1, w2;
    const double dev_creature = device_ms(false, &w0);
    double t0 = now_us();
    frames(120);
    const double creature = (now_us() - t0) / 120;
    show("\"pick\":9,\"bpm\":220");                      /* the bell 12/8, the busiest */
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
    snprintf(LIB_ARGS, sizeof LIB_ARGS, "{\"rhythms\":%s", lib);
    free(lib);
    if (wm_start(S, 7, 0) != 0 || wm_package(pkg, len) != 0) { fprintf(stderr, "beat_test: no start\n"); return 2; }
    wm_sense(1);
    frames(60);
    test_clock();
    test_wheel();
    test_cost();
    free(pkg);
    printf("beat_test: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
