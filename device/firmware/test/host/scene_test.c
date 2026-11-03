/*
 * SPDX-License-Identifier: MIT
 *
 * scene_test.c: host test for main/scene/scene_player.c with the director
 * (main/fluid/fluid_director.c), on the sample package (fixtures/shapes.json).
 *
 * Checks: binding ($param), repetition (each / @field) and polar placement give the
 * expected item geometry; every bad param is "bad_params" (unknown, missing, wrong type,
 * out of range, an asset or colour that is not one, too many elements / letters); an
 * unknown layout is "no_layout"; a scene ends at its ttl (or its dur) and the director
 * goes back to the creature; the queue plays in order, play replaces it, clear empties it;
 * the fill draws what is on (on / off) where it should, and lights a ball from its side.
 * Then a frame-cost estimate: a busy layout (1 ring, 12 glyphs, 10 balls, 40 letters)
 * against the creature's own update + raster + fill on the same host.
 *
 * Build + run: test/host/pkg_build.sh
 */
#define _POSIX_C_SOURCE 200809L
#include "scene_player.h"
#include "fluid_director.h"
#include "cute_spec.h"
#include "cute_rig.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int s_fail = 0, s_pass = 0;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)
#define DT (1.f / 30.f)

static pkg_t *P;

static scene_t *build(const char *layout, const char *params, float ttl, const char **err) {
    cJSON *j = params ? cJSON_Parse(params) : NULL;
    if (params && !j) { printf("bad test JSON: %s\n", params); s_fail++; }
    scene_t *sc = scene_build(P, layout, j, ttl, err);
    cJSON_Delete(j);
    return sc;
}

#define MARKS "[{\"a\":0,\"glyph\":\"plus\",\"color\":\"#ff0000\"},{\"a\":90,\"glyph\":\"halves\",\"color\":\"#00ff00\"},{\"a\":210,\"glyph\":\"plus\",\"color\":\"#0000ff\"}]"
static const char *DIAL = "{\"title\":\"hello\",\"lit\":0.4,\"marks\":" MARKS "}";

static const char *bad(const char *params) {
    const char *err;
    scene_t *sc = build("dial", params, 0, &err);
    scene_free(sc);
    return sc ? NULL : err;
}

static void test_build(void) {
    const char *err;
    scene_t *sc = build("dial", DIAL, 0, &err);
    CHECK(sc, "the dial builds (%s)", err ? err : "");
    if (!sc) return;
    CHECK(sc->nitems == 11, "9 items, the glyph once per mark: %d", sc->nitems);
    const scene_item_t *g = &sc->items[3];
    CHECK(g[0].k == PK_GLYPH && g[1].k == PK_GLYPH && g[2].k == PK_GLYPH && sc->items[6].k == PK_BALL, "order kept, each expanded in place");
    CHECK(NEAR(g[0].x, 0.72f) && NEAR(g[0].y, 0.f), "polar 0 deg: right (%f %f)", g[0].x, g[0].y);
    CHECK(NEAR(g[1].x, 0.f) && NEAR(g[1].y, 0.72f), "polar 90 deg: up (%f %f)", g[1].x, g[1].y);
    CHECK(NEAR(g[2].x, 0.72f * cosf(210.f * 0.017453293f)) && NEAR(g[2].y, -0.36f), "polar 210 deg (%f %f)", g[2].x, g[2].y);
    CHECK(g[0].rgb == 0xff0000 && g[1].rgb == 0x00ff00 && g[2].rgb == 0x0000ff, "@color per element");
    CHECK(g[0].asset == 0 && g[1].asset == 1 && g[2].asset == 0, "@glyph names an asset per element");
    CHECK(NEAR(g[0].size, 0.16f), "a literal beside the bindings");
    const scene_item_t *b = &sc->items[6];
    CHECK(NEAR(b->lit, 0.4f) && b->asset == 1 && NEAR(b->r, 0.25f) && NEAR(b->y, 0.1f) && b->side == 1.f, "$lit on the ball");
    const scene_item_t *t = &sc->items[10];
    CHECK(t->k == PK_TEXT && t->nstr == 5 && !memcmp(sc->chars + t->str, "hello", 5) && NEAR(t->y, -0.55f), "$title");
    CHECK(sc->items[0].rgb == 0xffffff && sc->items[1].rgb == 0xffffff && sc->items[1].n == 12, "defaults and literals");
    CHECK(sc->items[8].npts == 2 && NEAR(sc->pts[sc->items[8].pts + 1][0], 0.6f), "line points");
    CHECK(NEAR(sc->ttl, 60.f) && NEAR(scene_end(sc), 6.f), "ttl defaults to 60, the layout's dur ends it first");
    CHECK(!strcmp(sc->pkg_id, "shapes") && !strcmp(sc->layout, "dial"), "names");
    scene_free(sc);

    sc = build("dial", "{\"title\":\"x\",\"lit\":1,\"marks\":[]}", 9999, &err);
    CHECK(sc && sc->nitems == 8 && NEAR(sc->ttl, SCENE_TTL_MAX), "an empty list draws none; ttl capped at 600");
    scene_free(sc);

    CHECK(!build("nope", DIAL, 0, &err) && !strcmp(err, "no_layout"), "an unknown layout");
    CHECK(!strcmp(bad("{\"title\":\"hello\",\"lit\":0.4,\"marks\":[],\"x\":1}"), "bad_params"), "an unknown param");
    CHECK(!strcmp(bad("{\"title\":\"hello\",\"marks\":[]}"), "bad_params"), "a missing param");
    CHECK(!strcmp(bad(NULL), "bad_params"), "no params at all");
    CHECK(!strcmp(bad("{\"title\":5,\"lit\":0.4,\"marks\":[]}"), "bad_params"), "a number for text");
    CHECK(!strcmp(bad("{\"title\":\"a\",\"lit\":\"0.4\",\"marks\":[]}"), "bad_params"), "text for a number");
    CHECK(!strcmp(bad("{\"title\":\"a\",\"lit\":1.5,\"marks\":[]}"), "bad_params"), "lit out of range");
    CHECK(!strcmp(bad("{\"title\":\"01234567890123456789012345678901234567890\",\"lit\":0,\"marks\":[]}"), "bad_params"), "41 letters");
    CHECK(!strcmp(bad("{\"title\":\"a\",\"lit\":0,\"marks\":{}}"), "bad_params"), "a list that is not one");
    CHECK(!strcmp(bad("{\"title\":\"a\",\"lit\":0,\"marks\":[1]}"), "bad_params"), "an element that is not an object");
    CHECK(!strcmp(bad("{\"title\":\"a\",\"lit\":0,\"marks\":[{\"a\":0,\"glyph\":\"nothing\",\"color\":\"#ffffff\"}]}"), "bad_params"), "@glyph naming no asset");
    CHECK(!strcmp(bad("{\"title\":\"a\",\"lit\":0,\"marks\":[{\"a\":0,\"glyph\":\"plus\",\"color\":\"red\"}]}"), "bad_params"), "@color not #rrggbb");
    CHECK(!strcmp(bad("{\"title\":\"a\",\"lit\":0,\"marks\":[{\"glyph\":\"plus\",\"color\":\"#ffffff\"}]}"), "bad_params"), "@a missing");
    CHECK(!strcmp(bad("{\"title\":\"a\",\"lit\":0,\"marks\":[{\"a\":\"0\",\"glyph\":\"plus\",\"color\":\"#ffffff\"}]}"), "bad_params"), "@a as text");
    CHECK(!strcmp(bad("{\"title\":\"a\",\"lit\":0,\"marks\":[{\"a\":0,\"glyph\":\"plus\",\"color\":\"#ffffff\",\"deep\":{}}]}"), "bad_params"), "a nested element");
    char many[4096] = "{\"title\":\"a\",\"lit\":0,\"marks\":[";
    for (int i = 0; i <= SCENE_LIST_MAX; i++) strcat(many, i ? ",{\"a\":0,\"glyph\":\"plus\",\"color\":\"#ffffff\"}" : "{\"a\":0,\"glyph\":\"plus\",\"color\":\"#ffffff\"}");
    strcat(many, "]}");
    CHECK(!strcmp(bad(many), "bad_params"), "33 elements");
}

/* ── ttl, dur, the queue, back to the creature ─────────────────────────────── */

static fluid_director_t D;
static scene_player_t SP;
static void frame(void) {
    scene_tick(&SP, DT);
    fd_set_scene(&D, scene_playing(&SP));
    if (!fd_scene_wanted(&D) && scene_playing(&SP)) scene_clear(&SP);
    fd_tick(&D, DT, false);
    int st, fx;
    fd_take(&D, &st, &fx);
}
static void run(float s) { for (int n = (int)lrintf(s / DT); n > 0; n--) frame(); }
static scene_t *titled(const char *title, float ttl) {
    char p[160]; const char *err;
    snprintf(p, sizeof(p), "{\"title\":\"%s\",\"lit\":1,\"marks\":[]}", title);
    return build("dial", p, ttl, &err);
}
static char shown(void) { return SP.cur ? SP.cur->chars[SP.cur->items[SP.cur->nitems - 1].str] : '-'; }

static void test_player(void) {
    fd_init(&D, NULL, 7); fd_set_creature(&D, true); frame();
    memset(&SP, 0, sizeof(SP));
    scene_play(&SP, titled("a", 2.f));
    CHECK(scene_take_start(&SP) && !scene_take_start(&SP), "a start is reported once");
    fd_request_state(&D, FD_ST_SCENE, 0, false);
    frame();
    CHECK(D.cur == FD_ST_SCENE, "the scene is on screen");
    run(1.8f);
    CHECK(D.cur == FD_ST_SCENE && scene_playing(&SP), "still on at 1.8 s of a 2 s ttl");
    run(0.3f);
    CHECK(!scene_playing(&SP) && D.cur == FD_ST_CREATURE, "back to the creature when the ttl ends (cur %d)", D.cur);
    run(30.f);
    CHECK(D.cur == FD_ST_CREATURE, "and it stays there");

    /* the layout's dur ends it before a longer ttl */
    scene_play(&SP, titled("d", 60.f)); fd_request_state(&D, FD_ST_SCENE, 0, false);
    run(5.8f);
    CHECK(D.cur == FD_ST_SCENE, "on before its 6 s dur");
    run(0.4f);
    CHECK(D.cur == FD_ST_CREATURE, "dur 6 s ends it");

    /* queue order: a, b, c, each 1 s; then the creature */
    char order[8] = ""; int no = 0;
    CHECK(scene_queue(&SP, titled("a", 1.f)) && scene_queue(&SP, titled("b", 1.f)) && scene_queue(&SP, titled("c", 1.f)), "queued");
    fd_request_state(&D, FD_ST_SCENE, 0, false);
    char last = 0;
    for (int n = 0; n < 150; n++) {
        if (shown() != last && shown() != '-' && no < 7) order[no++] = shown();
        last = shown();
        frame();
    }
    CHECK(!strcmp(order, "abc"), "played in order: %s", order);
    CHECK(D.cur == FD_ST_CREATURE && !scene_playing(&SP), "then the creature");

    /* play replaces the queue; a full queue refuses; clear empties */
    scene_play(&SP, titled("x", 5.f));
    for (int i = 0; i < SCENE_QUEUE_MAX; i++) scene_queue(&SP, titled("q", 5.f));
    scene_t *extra = titled("z", 5.f);
    CHECK(SP.nq == SCENE_QUEUE_MAX && !scene_queue(&SP, extra), "a full queue refuses");
    scene_free(extra);
    scene_play(&SP, titled("y", 5.f));
    CHECK(SP.nq == 0 && shown() == 'y', "play replaces what plays and the queue");
    scene_clear(&SP);
    CHECK(!scene_playing(&SP) && SP.nq == 0, "clear");

    /* another state takes the screen (a say's words): the scene is dropped, not resumed later */
    scene_play(&SP, titled("s", 30.f)); fd_request_state(&D, FD_ST_SCENE, 0, false); frame();
    fd_request_state(&D, FD_ST_TEXT, 0, false); frame();
    CHECK(D.cur == FD_ST_TEXT && !scene_playing(&SP), "replaced by the words");
}

/* ── the fill ──────────────────────────────────────────────────────────────── */

static float ink_at(const fluid_sim_t *s, float x, float y) {
    int i = (int)((x + 1.f) * 0.5f * s->W), j = (int)((1.f - y) * 0.5f * s->H);
    return s->T[j * s->W + i] / FQ_UNIT;
}

static void test_fill(void) {
    const char *err;
    scene_t *sc = build("dial", DIAL, 0, &err);
    fluid_sim_t *s = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    fluid_letter_t L[SCENE_LETTERS]; int nl = 0;
    scene_fill(s, sc, 0.5f, L, &nl);
    CHECK(ink_at(s, 0.9f, 0.f) > 0.5f, "the ring at r 0.9 (%.2f)", ink_at(s, 0.9f, 0.f));
    CHECK(ink_at(s, -0.35f, 0.35f) == 0.f, "nothing where no item is");
    CHECK(ink_at(s, 0.6f * 0.7071f, 0.6f * 0.7071f) < 0.05f, "the arc is not on before 1 s");
    CHECK(ink_at(s, -0.5f, -0.5f) > 0.5f, "the disc is on before 3 s");
    CHECK(ink_at(s, 0.f, 0.72f) > 0.2f, "the glyph at polar 90 (%.2f)", ink_at(s, 0.f, 0.72f));
    CHECK(ink_at(s, 0.15f, 0.1f) > ink_at(s, -0.15f, 0.1f) + 0.1f, "the ball lit from the right (%.2f vs %.2f)", ink_at(s, 0.15f, 0.1f), ink_at(s, -0.15f, 0.1f));
    CHECK(nl == 5 && L[0].ch == 'h' && L[4].ch == 'o' && L[0].rgb == (FLUID_RGB | 0xffffff) && L[0].y > FLUID_STAGE_REF / 2, "the title's letters, below the centre");
    scene_fill(s, sc, 3.5f, L, &nl);
    CHECK(ink_at(s, 0.6f * 0.7071f, 0.6f * 0.7071f) > 0.5f, "the arc is on after 1 s");
    CHECK(ink_at(s, -0.5f, -0.5f) == 0.f, "the disc is off after 3 s");
    CHECK(ink_at(s, 0.6f * 0.7071f, -0.6f * 0.7071f) == 0.f, "the arc spans 0..90 only");
    fluid_sim_destroy(s);
    scene_free(sc);
}

/* ── frame cost: a busy layout against the creature on the same host ─────── */

static double now_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e6 + ts.tv_nsec * 1e-3; }

static void bench(const char *molty) {
    char p[4096] = "{\"caption\":\"0123456789012345678901234567890123456789\",\"glyphs\":[";
    for (int i = 0; i < 12; i++) snprintf(p + strlen(p), sizeof(p) - strlen(p), "%s{\"a\":%d}", i ? "," : "", i * 30);
    strcat(p, "],\"balls\":[");
    for (int i = 0; i < 10; i++) snprintf(p + strlen(p), sizeof(p) - strlen(p), "%s{\"a\":%d,\"r\":%.2f,\"lit\":%.1f}", i ? "," : "", i * 36 + 10, 0.35 + 0.03 * i, i / 10.0);
    strcat(p, "]}");
    const char *err;
    scene_t *sc = build("busy", p, 0, &err);
    CHECK(sc && sc->nitems == 1 + 12 + 10 + 1 && sc->nchars == 40, "the busy layout: 1 ring, 12 glyphs, 10 balls, 40 letters (%s)", err ? err : "");
    if (!sc) return;
    fluid_sim_t *s = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    fluid_letter_t L[SCENE_LETTERS]; int nl;
    const int N = 2000;
    double t0 = now_us();
    for (int k = 0; k < N; k++) scene_fill(s, sc, k * DT, L, &nl);
    const double scene_us = (now_us() - t0) / N;
    CHECK(nl == 40, "40 letters on the overlay");

    double creature_us = 0.;
    static uint8_t buf[CUTE_MAX_FILE + 16];
    FILE *f = fopen(molty, "rb");
    size_t n = f ? fread(buf, 1, sizeof(buf), f) : 0;
    if (f) fclose(f);
    static cute_spec_t spec;
    if (n && cute_spec_parse(buf, n, &spec, &err) == 0) {
        cute_rt_t *rt = cute_rt_create(&spec, 99);
        for (int k = 0; k < 30; k++) cute_rt_update(rt, DT);
        t0 = now_us();
        for (int k = 0; k < N; k++) { cute_rt_update(rt, DT); cute_rt_raster(rt, s->W, s->H); cute_rt_fill(rt, s, 1.f); }
        creature_us = (now_us() - t0) / N;
    }
    CHECK(creature_us > 0., "the creature to compare with (%s)", molty);
    /* docs/AGENT-LINK.md: the creature's update + raster + fill is ~11 ms of the device's
     * ~30 ms frame (33 fps). The scene replaces that work, so it scales the same way. */
    const double ratio = scene_us / (creature_us > 0. ? creature_us : 1.), dev_ms = 11.0 * ratio, frame_ms = 30.0 - 11.0 + dev_ms;
    printf("  bench (host): busy scene fill %.1f us, creature update+raster+fill %.1f us, ratio %.2f\n", scene_us, creature_us, ratio);
    printf("  estimate (device, scaled from the creature's ~11 ms at 33 fps): scene fill %.1f ms, frame %.1f ms = %.0f fps\n",
           dev_ms, frame_ms, 1000.0 / (frame_ms > 30.0 ? frame_ms : 30.0));
    CHECK(ratio < 1.0, "the busy scene costs less than the creature it replaces (ratio %.2f)", ratio);
    fluid_sim_destroy(s);
    scene_free(sc);
}

static char *read_file(const char *p, size_t *len) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); *len = fread(b, 1, (size_t)n, f); b[*len] = 0; fclose(f);
    return b;
}

int main(int argc, char **argv) {
    size_t len; const char *err;
    char *fix = argc > 1 ? read_file(argv[1], &len) : NULL;
    if (!fix || !(P = pkg_parse(fix, len, &err))) { printf("usage: scene_test fixtures/shapes.json creature.molty\n"); return 2; }
    free(fix);
    test_build();
    test_player();
    test_fill();
    bench(argc > 2 ? argv[2] : "");
    CHECK(P->refs == 1, "every scene gave its package reference back (%d)", P->refs);
    pkg_unref(P);
    printf("scene_test: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
