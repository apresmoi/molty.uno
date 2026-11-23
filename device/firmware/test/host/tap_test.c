/*
 * SPDX-License-Identifier: MIT
 *
 * tap_test.c: what a touch does to the water and to the creature, through mole_fluid.c itself (the web module's
 * native build, web_molty.c, included whole: its statics are this file's). A touch on a control (a scene's spot, a
 * question's answer) is not a touch on the water: its ring is TAP_SPOT of an open-water touch's at the same place and
 * moment, and it does not call the creature over, so a gesture or a mood asked right after an answer shows. Each case
 * runs in a child process forked from the same moment, so the runs it compares start alike.
 *
 *   tap_test <repo root>
 */
#include "../../web/web_molty.c"

#include <sys/wait.h>
#include <unistd.h>

static int s_pass, s_fail;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define DT 16667u
static void frames(int n) { for (int i = 0; i < n; i++) wm_frame(DT); }

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "tap_test: cannot read %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    b[n] = 0; *len = (size_t)n;
    fclose(f);
    return b;
}

/* a case in a child forked from now: it writes n bytes of results to out */
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
    if (got != n || !WIFEXITED(st) || WEXITSTATUS(st)) { fprintf(stderr, "tap_test: a case failed to run\n"); exit(2); }
}

/* ── the water: the ring a tap raises, against the same frames with no tap ── */

static int16_t *s_h0;                  /* the water with no tap, at the frame measured */
enum { RING_FRAMES = 8 };
typedef struct { float peak; double energy; int spot, opened, grow; } ring_t;
static int s_px_x, s_px_y, s_item;     /* the tap: panel px, and the spot item there */

static void calm(void *out) { frames(2 + RING_FRAMES); memcpy(out, s_sim->h, sizeof(int16_t) * (size_t)s_sim->N); }
static void ring_measure(ring_t *r) {
    r->peak = 0.f; r->energy = 0.;
    for (int k = 0; k < s_sim->N; k++) {
        const float d = (float)(s_sim->h[k] - s_h0[k]);
        r->peak = fmaxf(r->peak, fabsf(d)); r->energy += (double)d * d;
    }
}
static void on_spot(void *out) {
    ring_t *r = out;
    const unsigned g0 = s_sp.gen;
    r->spot = wm_touch(1, s_px_x, s_px_y); frames(2); wm_touch(0, 0, 0); frames(RING_FRAMES);
    ring_measure(r);
    r->opened = s_sp.gen != g0 && s_sp.back != NULL;
    r->grow = s_grow.spot;
}
/* the same touch with the scene's spots off the glass (none ever on): open water at the same place and moment */
static void on_water(void *out) {
    ring_t *r = out;
    for (int n = 0; n < s_sp.cur->nitems; n++) if (s_sp.cur->items[n].k == PK_SPOT) s_sp.cur->items[n].on = 1e9f;
    for (int n = 0; n < s_spot_sc->nitems; n++) if (s_spot_sc->items[n].k == PK_SPOT) s_spot_sc->items[n].on = 1e9f;
    r->spot = wm_touch(1, s_px_x, s_px_y); frames(2); wm_touch(0, 0, 0); frames(RING_FRAMES);
    ring_measure(r);
}
/* the first spot on the glass that opens a layout (open) or not, and a panel px on it that only it holds */
static bool find_spot(bool open) {
    const scene_t *sc = s_sp.cur;
    for (int n = 0; sc && n < sc->nitems; n++) {
        const scene_item_t *it = &sc->items[n];
        if (it->k != PK_SPOT || (it->nopen > 0) != open || s_sp.t < it->on || (it->off > 0.f && s_sp.t >= it->off - 2.f)) continue;
        const int px = s_ox + (int)lrintf((it->x + 1.f) * 0.5f * s_stage), py = s_oy + (int)lrintf((1.f - it->y) * 0.5f * s_stage);
        const float x = (float)(px - s_ox) / (float)s_stage * 2.f - 1.f, y = 1.f - (float)(py - s_oy) / (float)s_stage * 2.f;
        if (scene_spot_at(sc, s_sp.t, x, y) != n) continue;
        s_px_x = px; s_px_y = py; s_item = n;
        return true;
    }
    return false;
}
static void ring_case(const char *what, bool open, int kind) {
    fluid_fine_dust_kind(kind);
    if (!find_spot(open)) { CHECK(false, "%s: a spot on the glass to tap", what); return; }
    forked(calm, s_h0, sizeof(int16_t) * (size_t)s_sim->N);
    ring_t a, b;
    forked(on_spot, &a, sizeof(a));
    forked(on_water, &b, sizeof(b));
    const float k = b.peak > 0.f ? a.peak / b.peak : 0.f;
    printf("  %s: ring %.0f against %.0f on open water (%.3f), energy %.3f of it\n", what, a.peak, b.peak, k, b.energy > 0. ? a.energy / b.energy : 0.);
    CHECK(b.peak > 50.f && fabsf(k - TAP_SPOT) < 0.03f, "%s: the ring is %.2f of an open-water touch's (want %.2f)", what, k, TAP_SPOT);
    CHECK(a.spot == 1 && b.spot == 0, "%s: the touch tells its caller it hit a control (%d), open water not (%d)", what, a.spot, b.spot);
    if (open) CHECK(a.opened, "%s: the tap still opens its layout", what);
    else CHECK(a.grow == s_item, "%s: the tap still grows its drawing (spot %d, want %d)", what, a.grow, s_item);
}

/* ── the creature: a touch on a question's answer does not call it over ── */

typedef struct { int control, coming, eat, surprised, came; } cat_t;
static int s_ans_x, s_ans_y;
static bool clip_is(const char *name) {   /* the creature plays it now or next */
    const cc_t *c = cpk_clip(s_cpk);
    int q[8];
    const int n = cc_queue(c, q, 8), cur = cc_clip(c, NULL);
    if (cur >= 0 && strstr(cc_clip_name(c, cur), name)) return true;
    for (int i = 0; i < n; i++) if (q[i] >= 0 && strstr(cc_clip_name(c, q[i]), name)) return true;
    return false;
}
static void answer_then(void *out, const char *state, const char *clip) {
    cat_t *r = out;
    r->control = wm_touch(1, s_ans_x, s_ans_y); frames(2); wm_touch(0, 0, 0);
    frames(28);                                        /* 0.5 s after the touch */
    r->coming = cc_coming(cpk_clip(s_cpk));
    wm_state(state);
    int seen = 0;
    for (int f = 0; f < 12 && !seen; f++) { frames(1); seen = clip_is(clip); }
    *(clip[0] == 'e' ? &r->eat : &r->surprised) = seen;
}
static void answer_eat(void *out) { answer_then(out, "{\"act\":\"eat\"}", "eat"); }
static void answer_surprised(void *out) { answer_then(out, "{\"mood\":\"surprised\"}", "surprised"); }
static void open_glass(void *out) {
    cat_t *r = out;
    wm_ask_end(); frames(90);                          /* (the question gone: the glass is open water) */
    r->control = wm_touch(1, s_ans_x, s_ans_y); frames(2); wm_touch(0, 0, 0); frames(10);
    r->came = cc_coming(cpk_clip(s_cpk));
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: tap_test <repo root>\n"); return 2; }
    char path[1024]; size_t len;
    snprintf(path, sizeof(path), "%s/packages/cat/cat.json", argv[1]);
    char *cat = slurp(path, &len);
    CHECK(wm_creature("cat", cat, len) == 0, "the cat loads");
    snprintf(path, sizeof(path), "%s/packages/astrology/astrology.json", argv[1]);
    char *pkg = slurp(path, &len);
    CHECK(wm_package(pkg, len) == 0, "the astrology package loads");
    CHECK(wm_start(466, 20260516, 0) == 0, "the module starts");
    frames(120);
    s_h0 = malloc(sizeof(int16_t) * (size_t)s_sim->N);

    /* the creature: a question, its first answer touched */
    CHECK(wm_ask("{\"title\":\"feed the cat?\",\"options\":[\"yes\",\"no\"]}") == 0, "a question is asked");
    frames(150);
    bool found = false;
    for (int y = 0; y < s_px && !found; y += 4) for (int x = 0; x < s_px && !found; x += 4) {
        const float r = s_px * 0.5f;
        if (fluid_ask_pick(&s_q.hit, ((float)x + 0.5f - r) / r, ((float)y + 0.5f - r) / r) == 0) { s_ans_x = x; s_ans_y = y; found = true; }
    }
    CHECK(found && s_cpk && cpk_crisp(s_cpk), "the question's first answer is on the glass, over the cat");
    cat_t e = { 0 }, m = { 0 }, o = { 0 };
    forked(answer_eat, &e, sizeof(e));
    forked(answer_surprised, &m, sizeof(m));
    forked(open_glass, &o, sizeof(o));
    CHECK(e.control == 1, "a touch on an answer tells its caller it hit a control");
    CHECK(!e.coming && e.eat, "eat asked 0.5 s after an answer plays (the cat %s, eat %s)", e.coming ? "coming" : "not called", e.eat ? "plays" : "does not play");
    CHECK(!m.coming && m.surprised, "surprised set 0.5 s after an answer shows (%s)", m.surprised ? "it does" : "it does not");
    CHECK(o.control == 0 && o.came, "a touch on open glass still calls the cat over");
    wm_ask_end();
    frames(90);

    /* the water: a star chart's spots against open water */
    snprintf(path, sizeof(path), "%s/device/firmware/web/fidelity.script", argv[1]);
    char *script = slurp(path, &len), *chart = strstr(script, "scene chart 40 ");
    char *eol = chart ? strchr(chart, '\n') : NULL;
    if (eol) *eol = 0;
    CHECK(chart && wm_scene("chart", chart + strlen("scene chart 40 "), 40.f) == 0, "the chart is up");
    frames(180);
    ring_case("a planet, fine specks", false, FINE_DUST_SPECKS);
    ring_case("a planet, plain dots (no outline wave)", false, FINE_DUST_DOTS);
    ring_case("a sign (its close-up opens)", true, FINE_DUST_SPECKS);
    fluid_fine_dust_kind(FINE_DUST_SPECKS);

    free(script); free(pkg); free(cat); free(s_h0);
    printf("%d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
