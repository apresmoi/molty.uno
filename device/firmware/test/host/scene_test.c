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
 * Painter's order: a black disc erases, a ball's dark side hides what is behind it. The
 * astrology team's package parses and every command its skill sends builds, fills and queues.
 * A subtitle is the reader's word at the bottom (its ORP accented on the centre line, inside the
 * glass); a hairline is one dot a step on the dot lattice, 8-connected, at every angle and round a ring.
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
#include "glyph_font.h"

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

static char *read_file(const char *p, size_t *len) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); *len = fread(b, 1, (size_t)n, f); b[*len] = 0; fclose(f);
    return b;
}

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

    /* a reading queues one scene per body: 1 playing + 7 waiting, all in order */
    char seven[16] = ""; no = 0; last = 0;
    const char *const names[] = { "a", "b", "c", "d", "e", "f", "g", "h" };
    for (int i = 0; i < 8; i++) CHECK(scene_queue(&SP, titled(names[i], 1.f)), "queued %s", names[i]);
    CHECK(SP.nq == 7, "7 wait behind the one playing (%d)", SP.nq);
    fd_request_state(&D, FD_ST_SCENE, 0, false);
    for (int n = 0; n < 300; n++) {
        if (shown() != last && shown() != '-' && no < 15) seven[no++] = shown();
        last = shown();
        frame();
    }
    CHECK(!strcmp(seven, "abcdefgh"), "the eight played in order: %s", seven);
    CHECK(D.cur == FD_ST_CREATURE, "then the creature");

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
    fluid_letter_t L[FLUID_MAX_LETTERS]; int nl = 0;
    scene_fill(s, sc, 0.5f, NULL, L, &nl, NULL, NULL);
    CHECK(ink_at(s, 0.9f, 0.f) > 0.5f, "the ring at r 0.9 (%.2f)", ink_at(s, 0.9f, 0.f));
    CHECK(ink_at(s, -0.35f, 0.35f) == 0.f, "nothing where no item is");
    CHECK(ink_at(s, 0.6f * 0.7071f, 0.6f * 0.7071f) < 0.05f, "the arc is not on before 1 s");
    CHECK(ink_at(s, -0.5f, -0.5f) > 0.5f, "the disc is on before 3 s");
    CHECK(ink_at(s, 0.f, 0.72f) > 0.2f, "the glyph at polar 90 (%.2f)", ink_at(s, 0.f, 0.72f));
    CHECK(ink_at(s, 0.15f, 0.1f) > ink_at(s, -0.15f, 0.1f) + 0.1f, "the ball lit from the right (%.2f vs %.2f)", ink_at(s, 0.15f, 0.1f), ink_at(s, -0.15f, 0.1f));
    int nm = 0;
    while (nm < nl && L[nm].mask) nm++;
    CHECK(nm > 0 && L[0].mw > 0 && nl == nm + 5, "the small glyphs are crisp symbols (%d), then the title (%d letters)", nm, nl - nm);
    const fluid_letter_t *T = L + nm;
    CHECK(nl == nm + 5 && T[0].ch == 'h' && T[4].ch == 'o' && T[0].rgb == (FLUID_RGB | 0xffffff) && T[0].y > FLUID_STAGE_REF / 2, "the title's letters, below the centre");
    scene_fill(s, sc, 3.5f, NULL, L, &nl, NULL, NULL);
    CHECK(ink_at(s, 0.6f * 0.7071f, 0.6f * 0.7071f) > 0.5f, "the arc is on after 1 s");
    CHECK(ink_at(s, -0.5f, -0.5f) == 0.f, "the disc is off after 3 s");
    CHECK(ink_at(s, 0.6f * 0.7071f, -0.6f * 0.7071f) == 0.f, "the arc spans 0..90 only");
    fluid_sim_destroy(s);
    scene_free(sc);
}

/* painter's order (packages/tools/play.mjs): a later item covers an earlier one, a black
 * disc erases, a ball's dark side hides what is behind it */
static const char *OCCLUDE =
    "{\"v\":\"molty-pkg/1\",\"id\":\"occlude\",\"name\":\"Occlude\",\"version\":\"1\",\"kind\":\"scenes\",\"layouts\":{"
    "\"erase\":{\"items\":[{\"k\":\"disc\",\"r\":0.4},{\"k\":\"disc\",\"r\":0.15,\"color\":\"#000000\"}]},"
    "\"under\":{\"items\":[{\"k\":\"disc\",\"r\":0.15,\"color\":\"#000000\"},{\"k\":\"disc\",\"r\":0.4}]},"
    "\"cover\":{\"items\":[{\"k\":\"disc\",\"r\":0.4,\"color\":\"#ff0000\"},{\"k\":\"disc\",\"r\":0.15,\"color\":\"#0000ff\"}]},"
    "\"ring\":{\"items\":[{\"k\":\"ring\",\"r\":0.2,\"w\":0.06}]},"
    "\"dark\":{\"items\":[{\"k\":\"ring\",\"r\":0.2,\"w\":0.06},{\"k\":\"ball\",\"r\":0.35,\"lit\":0}]},"
    "\"half\":{\"items\":[{\"k\":\"ring\",\"r\":0.2,\"w\":0.06},{\"k\":\"ball\",\"r\":0.35,\"lit\":0.5,\"side\":1}]}}}";

static fluid_sim_t *fill_layout(pkg_t *p, const char *layout) {
    const char *err;
    scene_t *sc = scene_build(p, layout, NULL, 0, &err);
    CHECK(sc, "%s builds (%s)", layout, err ? err : "");
    fluid_sim_t *s = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    fluid_letter_t L[FLUID_MAX_LETTERS]; int nl;
    scene_fill(s, sc, 0.5f, NULL, L, &nl, NULL, NULL);
    scene_free(sc);
    return s;
}

static void test_occlusion(void) {
    const char *err;
    pkg_t *p = pkg_parse(OCCLUDE, strlen(OCCLUDE), &err);
    CHECK(p, "the occlusion package parses (%s)", err ? err : "");
    if (!p) return;
    fluid_sim_t *s = fill_layout(p, "erase");
    CHECK(ink_at(s, 0.f, 0.f) < 0.02f && ink_at(s, 0.3f, 0.f) > 0.5f, "a black disc erases what is under it (%.2f), not around it", ink_at(s, 0.f, 0.f));
    fluid_sim_destroy(s);
    s = fill_layout(p, "under");
    CHECK(ink_at(s, 0.f, 0.f) > 0.5f, "a black disc under a later one is covered (%.2f)", ink_at(s, 0.f, 0.f));
    fluid_sim_destroy(s);
    s = fill_layout(p, "cover");
    const uint32_t c = s->tcol[16 * 52 + 26];
    CHECK((c >> 16 & 255) > 200 && (c & 255) < 50, "a later dimmer-or-equal item still takes the colour (blue over red: %06x)", c & 0xffffff);
    fluid_sim_destroy(s);
    s = fill_layout(p, "ring");
    const float bare_r = ink_at(s, 0.2f, 0.f), bare_l = ink_at(s, -0.2f, 0.f);
    fluid_sim_destroy(s);
    s = fill_layout(p, "dark");
    CHECK(bare_r > 0.5f && ink_at(s, 0.2f, 0.f) < 0.12f && ink_at(s, -0.2f, 0.f) < 0.12f,
          "an unlit ball hides the ring behind it (%.2f -> %.2f)", bare_r, ink_at(s, 0.2f, 0.f));
    fluid_sim_destroy(s);
    s = fill_layout(p, "half");
    CHECK(bare_l > 0.5f && ink_at(s, -0.2f, 0.f) < 0.12f && ink_at(s, 0.2f, 0.f) > 0.5f,
          "a half-lit ball: its dark left side hides the ring (%.2f), its lit right side shows itself (%.2f)", ink_at(s, -0.2f, 0.f), ink_at(s, 0.2f, 0.f));
    fluid_sim_destroy(s);
    pkg_unref(p);
}

/* ── spots: a place to tap and the words for it; hairlines ─────────────────── */

static const char *SPOTS =
    "{\"v\":\"molty-pkg/1\",\"id\":\"spots\",\"name\":\"Spots\",\"version\":\"1\",\"kind\":\"scenes\",\"layouts\":{"
    "\"map\":{\"params\":{\"list\":\"list\"},\"items\":["
    "{\"k\":\"spot\",\"each\":\"$list\",\"polar\":[\"@a\",0.5],\"r\":0.2,\"say\":\"@say\",\"id\":\"@id\"},"
    "{\"k\":\"spot\",\"x\":0,\"y\":0,\"r\":0.2,\"say\":\"the middle\",\"on\":1}]},"
    "\"hair\":{\"items\":[{\"k\":\"line\",\"pts\":[[-0.8,-0.5],[0.8,0.3]],\"w\":0.012}]}}}";

static void test_spots(void) {
    const char *err;
    pkg_t *p = pkg_parse(SPOTS, strlen(SPOTS), &err);
    CHECK(p, "the spots package parses (%s)", err ? err : "");
    if (!p) return;
    char say[PKG_SAY_MAX + 2];
    memset(say, 'a', PKG_SAY_MAX); say[PKG_SAY_MAX] = 0;
    char params[1024];
    snprintf(params, sizeof params, "{\"list\":[{\"a\":0,\"say\":\"%s\",\"id\":\"east\"},{\"a\":20,\"say\":\"north of east\",\"id\":\"ne\"}]}", say);
    cJSON *j = cJSON_Parse(params);
    scene_t *sc = scene_build(p, "map", j, 0, &err);
    cJSON_Delete(j);
    CHECK(sc && sc->nitems == 3, "spots build, a %d-letter reading too (%s)", PKG_SAY_MAX, err ? err : "");
    if (sc) {
        const scene_item_t *e = &sc->items[0];
        CHECK(e->k == PK_SPOT && e->nstr == PKG_SAY_MAX && e->nsid == 4 && !memcmp(sc->say + e->sid, "east", 4), "its reading and id");
        CHECK(sc->items[2].nsid == 0 && sc->items[2].nstr == 10, "a literal reading, no id");
        CHECK(scene_spot_at(sc, 0.f, 0.55f, 0.f) == 0, "a tap on it");
        const float nx = 0.5f * cosf(20.f * 0.017453293f), ny = 0.5f * sinf(20.f * 0.017453293f);
        CHECK(scene_spot_at(sc, 0.f, nx, ny - 0.02f) == 1, "two overlap: the nearer centre wins");
        CHECK(scene_spot_at(sc, 0.f, -0.5f, 0.f) == -1, "a tap on nothing");
        CHECK(scene_spot_at(sc, 0.5f, 0.f, 0.f) == -1 && scene_spot_at(sc, 1.5f, 0.f, 0.f) == 2, "a spot is there from its on");
        fluid_sim_t *s = fluid_sim_create(52, 32, FLUID_STAGE_REF);
        fluid_letter_t L[FLUID_MAX_LETTERS]; int nl;
        scene_fill(s, sc, 2.f, NULL, L, &nl, NULL, NULL);
        int ink = 0;
        for (int k = 0; k < s->N; k++) ink += s->T[k] != 0;
        CHECK(ink == 0 && nl == 0, "a spot draws nothing (%d cells)", ink);
        fluid_sim_destroy(s);
    }
    scene_free(sc);
    say[PKG_SAY_MAX] = 'a'; say[PKG_SAY_MAX + 1] = 0;
    snprintf(params, sizeof params, "{\"list\":[{\"a\":0,\"say\":\"%s\",\"id\":\"east\"}]}", say);
    j = cJSON_Parse(params);
    sc = scene_build(p, "map", j, 0, &err);
    cJSON_Delete(j);
    CHECK(!sc && !strcmp(err, "bad_params"), "a reading over %d letters", PKG_SAY_MAX);
    scene_free(sc);
    static const char *NO_SAY = "{\"v\":\"molty-pkg/1\",\"id\":\"x\",\"name\":\"X\",\"version\":\"1\",\"kind\":\"scenes\",\"layouts\":{\"a\":{\"items\":[{\"k\":\"spot\",\"r\":0.2}]}}}";
    pkg_t *q = pkg_parse(NO_SAY, strlen(NO_SAY), &err);
    CHECK(!q, "a spot needs its reading");
    pkg_unref(q);

    pkg_unref(p);
}

/* ── hairlines: a dot a step on the dot lattice ───────────────────────────── */

#define LW (2 * 52)
#define LH (4 * 32)
static uint8_t s_lit[LH][LW];

/* fill the one-item layout and read its dots back onto the lattice (dot (x, y): column x, row y) */
static int lit_dots(const char *item) {
    char js[512]; const char *err;
    snprintf(js, sizeof js, "{\"v\":\"molty-pkg/1\",\"id\":\"h\",\"name\":\"H\",\"version\":\"1\",\"kind\":\"scenes\","
             "\"layouts\":{\"a\":{\"items\":[%s]}}}", item);
    pkg_t *p = pkg_parse(js, strlen(js), &err);
    scene_t *sc = p ? scene_build(p, "a", NULL, 0, &err) : NULL;
    CHECK(sc, "%s builds (%s)", item, err ? err : "");
    memset(s_lit, 0, sizeof s_lit);
    int n = 0;
    if (sc) {
        fluid_sim_t *s = fluid_sim_create(52, 32, FLUID_STAGE_REF);
        uint8_t *dots = calloc((size_t)s->N, 1);
        fluid_letter_t L[FLUID_MAX_LETTERS]; int nl;
        scene_fill(s, sc, 0.f, NULL, L, &nl, dots, NULL);
        for (int y = 0; y < LH; y++) for (int x = 0; x < LW; x++)
            n += s_lit[y][x] = (dots[(y >> 2) * s->W + (x >> 1)] >> fluid_braille_bit(x & 1, y & 3)) & 1;
        free(dots);
        fluid_sim_destroy(s);
    }
    scene_free(sc);
    pkg_unref(p);
    return n;
}
static inline float lat_x(float x) { return (x + 1.f) * 0.5f * LW - 0.5f; }   /* glass -> lattice */
static inline float lat_y(float y) { return (1.f - y) * 0.5f * LH - 0.5f; }
static int neighbours(int x, int y) {
    int n = 0;
    for (int v = -1; v <= 1; v++) for (int u = -1; u <= 1; u++)
        n += (u || v) && x + u >= 0 && y + v >= 0 && x + u < LW && y + v < LH && s_lit[y + v][x + u];
    return n;
}

static void test_hairlines(void) {
    /* segments at every 5 degrees and at the shallow and steep diagonals of the lattice */
    float angles[90]; int na = 0;
    for (int d = 0; d < 180; d += 5) angles[na++] = (float)d;
    const float lat[] = { 1.f / 3, 1.f / 2, 2.f / 3, 1.f, 3.f / 2, 2.f, 3.f };   /* rise over run in dots */
    for (unsigned q = 0; q < sizeof lat / sizeof *lat; q++) {
        const float a = atanf(lat[q] * LW / LH) / 0.017453293f;
        angles[na++] = a; angles[na++] = 180.f - a;
    }
    int bad = 0;
    for (int k = 0; k < na; k++) {
        const float t = angles[k] * 0.017453293f, ax = 0.04f - 0.7f * cosf(t), ay = -0.03f - 0.7f * sinf(t), bx = 0.04f + 0.7f * cosf(t), by = -0.03f + 0.7f * sinf(t);
        char item[160];
        snprintf(item, sizeof item, "{\"k\":\"line\",\"pts\":[[%.4f,%.4f],[%.4f,%.4f]],\"w\":0.012}", ax, ay, bx, by);
        const int total = lit_dots(item);
        const float X0 = lat_x(ax), Y0 = lat_y(ay), X1 = lat_x(bx), Y1 = lat_y(by), DX = X1 - X0, DY = Y1 - Y0;
        const bool xmaj = fabsf(DX) >= fabsf(DY);
        const int m0 = (int)lroundf(xmaj ? fminf(X0, X1) : fminf(Y0, Y1)), m1 = (int)lroundf(xmaj ? fmaxf(X0, X1) : fmaxf(Y0, Y1));
        int steps = 0, wrong = 0, jumps = 0, far = 0, prev = -1, seen = 0;
        for (int mj = m0; mj <= m1; mj++, steps++) {
            int cnt = 0, mi = -1;
            for (int mn = 0; mn < (xmaj ? LH : LW); mn++) if (xmaj ? s_lit[mn][mj] : s_lit[mj][mn]) { cnt++; mi = mn; }
            seen += cnt;
            wrong += cnt != 1;
            if (cnt != 1) continue;
            jumps += prev >= 0 && abs(mi - prev) > 1;
            prev = mi;
            const float px = xmaj ? mj : mi, py = xmaj ? mi : mj;
            far += fabsf((px - X0) * DY - (py - Y0) * DX) / sqrtf(DX * DX + DY * DY) > 1.f;
        }
        if (wrong || jumps || far || seen != total) {
            if (!bad) printf("  hairline at %.1f deg: %d of %d steps not one dot, %d gaps, %d dots over a dot off, %d outside its ends\n",
                             angles[k], wrong, steps, jumps, far, total - seen);
            bad++;
        }
    }
    CHECK(bad == 0, "a hairline is one dot a step, 8-connected, within a dot of the line, at every angle (%d of %d not)", bad, na);

    /* rings and an arc: a simple 8-connected curve (each dot two neighbours, an arc's ends one; no
     * three dots in any 2 x 2), within a dot of the circle */
    const char *rings[] = { "{\"k\":\"ring\",\"r\":0.6,\"w\":0.01}", "{\"k\":\"ring\",\"r\":0.33,\"w\":0.01}",
                            "{\"k\":\"ring\",\"r\":0.85,\"w\":0.01}", "{\"k\":\"arc\",\"r\":0.5,\"a0\":30,\"a1\":200,\"w\":0.01}" };
    const float rr[] = { 0.6f, 0.33f, 0.85f, 0.5f };
    for (int k = 0; k < 4; k++) {
        const int total = lit_dots(rings[k]);
        int ends = 0, thick = 0, clump = 0, far = 0;
        for (int y = 0; y < LH; y++) for (int x = 0; x < LW; x++) {
            if (y + 1 < LH && x + 1 < LW && s_lit[y][x] + s_lit[y][x + 1] + s_lit[y + 1][x] + s_lit[y + 1][x + 1] >= 3) clump++;
            if (!s_lit[y][x]) continue;
            const int nb = neighbours(x, y);
            ends += nb == 1; thick += nb != 2 && nb != 1;
            const float gx = ((x + 0.5f) / LW) * 2.f - 1.f, gy = 1.f - ((y + 0.5f) / LH) * 2.f, d = sqrtf(gx * gx + gy * gy);
            const float ex = (gx - gx * rr[k] / d) * 0.5f * LW, ey = (gy - gy * rr[k] / d) * 0.5f * LH;
            far += sqrtf(ex * ex + ey * ey) > 1.f;
        }
        CHECK(total > 50 && ends == (k == 3 ? 2 : 0) && thick == 0 && clump == 0 && far == 0,
              "%s: %d dots, %d ends, %d without two neighbours, %d clumps, %d over a dot off", rings[k], total, ends, thick, clump, far);
    }
}

/* ── the subtitle: the reader's word at the bottom ───────────────────────── */

/* the word's letters, n of them after the scene's own, the ORP (orp) accented on x = stage / 2
 * (orp -1: none, the word centred), every box inside the glass at the caption's height */
static void check_caption(const scene_t *sc, const char *w, int orp) {
    fluid_sim_t *s = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    fluid_letter_t L[FLUID_MAX_LETTERS]; int nl, base;
    scene_fill(s, sc, 0.f, NULL, L, &base, NULL, NULL);
    const int n = (int)strlen(w);
    const scene_caption_t cap = { w, n, orp };
    scene_fill(s, sc, 0.f, &cap, L, &nl, NULL, NULL);
    const float S = FLUID_STAGE_REF, mid = S / 2, cy = (1.f - SCENE_CAPTION_Y) * 0.5f * S;
    int same = nl == base + n, acc = 0, inside = 1, row = 1;
    for (int q = 0; same && q < n; q++) {
        const fluid_letter_t *c = &L[base + q];
        same &= c->ch == w[q] && !c->mask && c->rgb == 0;
        acc += c->acc != (q == orp);
        for (int k = 0; k < 4; k++) {
            const float x = c->x + (k & 1) * c->w - mid, y = c->y + (k >> 1) * c->h - mid;
            inside &= x * x + y * y <= mid * mid;
        }
        row &= fabsf(c->y + c->h * 0.5f - cy) < 1.f;
    }
    const fluid_letter_t *o = &L[base + (orp < 0 ? 0 : orp)];
    const float centre = orp < 0 ? o->x + n * o->w * 0.5f : o->x + o->w * 0.5f;
    CHECK(same && acc == 0 && fabsf(centre - mid) < 0.5f && inside && row,
          "\"%s\" orp %d: its %d letters (%d), %d wrongly accented, centred at %.1f of %.1f, inside the glass %d, on the caption's row %d",
          w, orp, n, nl - base, acc, centre, mid, inside, row);
    fluid_sim_destroy(s);
}

static void test_caption(void) {
    const char *err;
    pkg_t *p = pkg_parse(SPOTS, strlen(SPOTS), &err);
    scene_t *sc = p ? scene_build(p, "hair", NULL, 0, &err) : NULL;
    CHECK(sc, "the caption's scene builds");
    if (sc) {
        check_caption(sc, "orbiting", 2);
        check_caption(sc, "a", 0);
        check_caption(sc, "the", -1);
        check_caption(sc, "incomprehensibilities", 4);
        check_caption(sc, "incomprehensibilities", 19);
    }
    scene_free(sc);
    pkg_unref(p);
}

/* ── high definition: a symbol's dots are its shape (its dust); each scene forms from dust,
 * and breaks into dust before the next one or the creature ── */
static void test_hd(void) {
    const char *err;
    scene_t *sc = build("dial", DIAL, 0, &err);
    fluid_sim_t *s = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    uint8_t *dots = calloc((size_t)s->N, 1);
    fluid_letter_t L[FLUID_MAX_LETTERS]; int nl = 0;
    scene_fill(s, sc, 0.5f, NULL, L, &nl, dots, NULL);
    const fluid_letter_t *p = &L[0];                         /* the "plus" at 0 degrees */
    const float px = (float)s->stage / (2 * s->W), py = (float)s->stage / (4 * s->H);
    int corner = 0, bar = 0;
    for (int y = 0; y < 4 * s->H; y++) for (int x = 0; x < 2 * s->W; x++) {
        const float u = ((x + 0.5f) * px - p->x) / p->w, v = ((y + 0.5f) * py - p->y) / p->h;
        if (u < 0.f || u >= 1.f || v < 0.f || v >= 1.f) continue;
        const bool on = dots[(y >> 2) * s->W + (x >> 1)] >> fluid_braille_bit(x & 1, y & 3) & 1;
        if ((u < 0.3f || u >= 0.7f) && (v < 0.3f || v >= 0.7f)) corner += on;
        else bar += on;
    }
    CHECK(p->mask && corner == 0 && bar > 6, "the symbol's dots are a plus: %d in its corners, %d on its bars", corner, bar);
    free(dots); fluid_sim_destroy(s); scene_free(sc);

    scene_player_t sp = { 0 };
    scene_hd_t h = { 0, false, 1.f };
    int n;
    scene_play(&sp, titled("a", 30.f));
    for (n = 0; n < 100 && h.dust > 0.f; n++) scene_hd_tick(&h, sp.gen, false, DT);
    CHECK(h.gen == sp.gen && !h.out && fabsf(n * DT - SCENE_DUST_S) <= DT, "a forms from dust in %.2f s", n * DT);
    const unsigned a = sp.gen;
    scene_play(&sp, titled("b", 30.f));
    scene_hd_tick(&h, sp.gen, false, DT);
    CHECK(h.out && h.gen == a && h.dust > 0.f && h.dust < 0.2f, "b: a breaks into dust first (%.2f)", h.dust);
    float peak = 0.f;
    for (n = 0; n < 100 && h.out; n++) { scene_hd_tick(&h, sp.gen, false, DT); if (h.out && h.dust > peak) peak = h.dust; }
    CHECK(peak > 0.85f && h.gen == sp.gen && h.dust > 0.85f, "a went all to dust (%.2f), b forms (%.2f)", peak, h.dust);
    for (n = 0; n < 100 && h.dust > 0.f; n++) scene_hd_tick(&h, sp.gen, false, DT);
    CHECK(h.gen == sp.gen && h.dust == 0.f, "b whole");
    scene_hd_tick(&h, sp.gen, true, DT);
    CHECK(h.gen == 0 && h.dust == 1.f, "a listen: the fluid's own look");
    scene_hd_tick(&h, sp.gen, false, DT);
    CHECK(h.gen == sp.gen && h.dust < 1.f && h.dust > 0.8f, "after it, b forms from dust again");
    for (n = 0; n < 100 && h.dust > 0.f; n++) scene_hd_tick(&h, sp.gen, false, DT);
    scene_clear(&sp);
    for (n = 0; n < 100 && h.gen; n++) scene_hd_tick(&h, 0, false, DT);
    CHECK(h.gen == 0 && !h.out && h.dust == 1.f && n * DT >= SCENE_DUST_S - 2 * DT, "cleared: b breaks into dust over %.2f s, then nothing", n * DT);
}

/* ── a real package: every command the molty-astro skill sends (fixtures/astrology*, copied from
 * the astrology team's packages/astrology/astrology.json and `astro.mjs <scene> --dry-run`) ── */

static void test_real_package(const char *dir) {
    char path[512]; size_t len; const char *err;
    snprintf(path, sizeof(path), "%s/astrology.json", dir);
    char *buf = read_file(path, &len);
    const size_t plen = len;
    pkg_t *p = buf ? pkg_parse(buf, len, &err) : NULL;
    free(buf);
    CHECK(p, "the real package parses (%s)", buf ? err : "missing");
    if (!p) return;
    snprintf(path, sizeof(path), "%s/astrology-commands.jsonl", dir);
    char *cmds = read_file(path, &len);
    scene_player_t sp = { 0 };
    bool seen[PKG_LAYOUTS_MAX] = { 0 };
    int ncmd = 0, deepest = 0;
    fluid_sim_t *s = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    for (char *line = cmds ? strtok(cmds, "\n") : NULL; line; line = strtok(NULL, "\n"), ncmd++) {
        cJSON *c = cJSON_Parse(line), *a = cJSON_GetObjectItem(c, "args");
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(c, "name")), *layout = cJSON_GetStringValue(cJSON_GetObjectItem(a, "layout"));
        scene_t *sc = scene_build(p, layout, cJSON_GetObjectItem(a, "params"), (float)cJSON_GetNumberValue(cJSON_GetObjectItem(a, "ttl_s")), &err);
        CHECK(sc, "%s %s builds (%s)", name, layout, err ? err : "");
        if (!sc) { cJSON_Delete(c); continue; }
        for (int i = 0; i < p->nlayouts; i++) if (!strcmp(p->layouts[i].name, layout)) seen[i] = true;
        fluid_letter_t L[FLUID_MAX_LETTERS]; int nl, lit = 0;
        scene_fill(s, sc, sc->dur > 0.f ? sc->dur / 2 : 1.f, NULL, L, &nl, NULL, NULL);
        for (int k = 0; k < s->N; k++) lit += s->T[k] > 0.2f * FQ_UNIT;
        CHECK(lit > 10, "%s draws something (%d cells; a thin crescent is ~20)", layout, lit);
        if (!strcmp(name, "scene.play")) scene_play(&sp, sc);
        else CHECK(scene_queue(&sp, sc), "%s queued", layout);
        if (sp.nq > deepest) deepest = sp.nq;
        cJSON_Delete(c);
    }
    int all = 0;
    for (int i = 0; i < p->nlayouts; i++) all += seen[i];
    CHECK(ncmd >= 15 && all == p->nlayouts, "%d commands played every layout (%d of %d)", ncmd, all, p->nlayouts);
    CHECK(deepest >= 6, "the skill queues %d at once, within the queue", deepest);
    printf("  real package: %d layouts, %d commands, %zu B parsed into %zu B, deepest queue %d\n", p->nlayouts, ncmd, plen, p->used, deepest);
    scene_clear(&sp);
    fluid_sim_destroy(s);
    free(cmds);
    CHECK(p->refs == 1, "its scenes gave their references back");
    pkg_unref(p);
}

/* ── frame cost: a busy layout against the creature on the same host ─────── */

static double now_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e6 + ts.tv_nsec * 1e-3; }

/* ── high definition at the panel's pixels: the chart on the 1.75C's 466 px stage. Calm, its
 * rings are true circles of anti-aliased pixels (not the dot lattice), its symbols too; a touch
 * turns only the cells near the finger into dust, and calm water gives back the very same
 * pixels; so does a transition. The panel's windows (fluid_draw_send) are the same frame. ── */

static int send_fb(void *ctx, int x, int y, int w, int h, const uint16_t *px) {
    uint16_t *fb = ctx;
    for (int r = 0; r < h; r++) memcpy(fb + (size_t)(y + r) * 466 + x, px + (size_t)r * w, (size_t)w * 2);
    return 0;
}
static int lum(uint16_t be) { const uint16_t le = (uint16_t)(be >> 8 | be << 8); return (le >> 11) * 8 + ((le >> 5) & 63) * 4 + (le & 31) * 8; }

static void test_hd_pixels(const char *dir) {
    char path[512]; size_t len; const char *err;
    snprintf(path, sizeof(path), "%s/astrology.json", dir);
    char *buf = read_file(path, &len);
    pkg_t *p = buf ? pkg_parse(buf, len, &err) : NULL;
    free(buf);
    snprintf(path, sizeof(path), "%s/astrology-commands.jsonl", dir);
    char *cmds = p ? read_file(path, &len) : NULL;
    scene_t *sc = NULL;
    for (char *line = cmds ? strtok(cmds, "\n") : NULL; line && !sc; line = strtok(NULL, "\n")) {
        cJSON *c = cJSON_Parse(line), *a = cJSON_GetObjectItem(c, "args");
        const char *layout = cJSON_GetStringValue(cJSON_GetObjectItem(a, "layout"));
        if (layout && !strcmp(layout, "chart")) sc = scene_build(p, layout, cJSON_GetObjectItem(a, "params"), 0.f, &err);
        cJSON_Delete(c);
    }
    free(cmds);
    CHECK(sc, "the chart builds");
    if (!sc) { pkg_unref(p); return; }
    const int S = 466;
    int W, H;
    fluid_grid_for_stage(S, &W, &H);
    fluid_sim_t *s = fluid_sim_create(W, H, S);
    fluid_frame_t *fr = fluid_frame_create(W, H);
    fluid_draw_t *d = fluid_draw_create(W, H, S, S);
    const int N = W * H;
    uint8_t *dots = calloc((size_t)N, 1), *keep = calloc((size_t)N, 1);
    static fluid_shape_t shp[FLUID_MAX_SHAPES];
    uint16_t *fb = calloc((size_t)S * S, 2), *calm = calloc((size_t)S * S, 2), *win = calloc((size_t)S * S, 2);
    fluid_letter_t L[FLUID_MAX_LETTERS]; int nl;
    scene_shapes_t hd = { shp, 0, keep };
    scene_fill(s, sc, 1.f, NULL, L, &nl, dots, &hd);
    int rings = 0, segs = 0, masks = 0;
    for (int n = 0; n < hd.n; n++) { rings += shp[n].k == FLUID_SHAPE_RING; segs += shp[n].k == FLUID_SHAPE_SEG; masks += shp[n].k == FLUID_SHAPE_MASK; }
    CHECK(rings == 3 && segs > 12 + 36 && masks == nl && nl > 12, "its shapes: %d rings, %d strokes, %d symbols (%d letters)", rings, segs, masks, nl);
    fluid_sim_snapshot(s, fr, false);
    memset(fr->h, 0, sizeof(int16_t) * (size_t)N);
    fluid_draw_opts_t o = { .palette = FLUID_PAL_PASTEL, .edge_mode = FLUID_EDGE_CONTOURS, .letters = L, .nletters = nl, .font = gc_font5x7,
                            .dots = dots, .scene = true, .shapes = shp, .nshapes = hd.n, .kdots = keep };
    int y0, y1;
    fluid_draw(d, fr, fb, &o, &y0, &y1);
    memcpy(calm, fb, (size_t)S * S * 2);

    /* the outer ring (r 0.9): lit at every degree, its edge soft (partly lit pixels both sides) */
    const float R = 0.9f * S / 2, c0 = S / 2.f;
    int hit = 0, soft = 0, lattice = 0;
    for (int a = 0; a < 360; a++) {
        const float co = cosf(a * 0.0174533f), si = sinf(a * 0.0174533f);
        int best = 0;
        for (float dr = -1.f; dr <= 1.f; dr += 0.5f) { const int v = lum(fb[(int)(c0 + (R + dr) * si) * S + (int)(c0 + (R + dr) * co)]); if (v > best) best = v; }
        hit += best > 300;
        for (float dr = 1.5f; dr <= 2.5f; dr += 1.f) {
            const int v = lum(fb[(int)(c0 + (R + dr) * si) * S + (int)(c0 + (R + dr) * co)]);
            soft += v > 20 && v < best * 3 / 4;
        }
    }
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++)
        if (fb[y * S + x] && d->yrow[y] < 0) lattice++;                 /* lit between the dot rows */
    CHECK(hit == 360 && soft > 60 && lattice > 2000, "calm: a true ring (%d of 360 degrees), soft edged (%d), lit off the dot rows (%d px)", hit, soft, lattice);
    int hdc = 0, keyd = 0;
    for (int k = 0; k < N; k++) { hdc += (d->key[k] & 0xF000) == 0x8000; keyd += d->key[k] && d->key[k] < 0x100; }
    CHECK(hdc > 100 && keyd == 0, "calm: %d cells drawn smooth, %d as dots", hdc, keyd);
    fluid_draw(d, fr, fb, &o, &y0, &y1);
    CHECK(y1 == y0, "calm: nothing redrawn the next frame ([%d, %d))", y0, y1);

    /* the panel's windows draw the same frame */
    static uint16_t wbuf[466 * 14];
    d->force = true;
    fluid_draw_cells(d, fr, &o);
    fluid_draw_send(d, &o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, true, wbuf, S * 14, send_fb, win);
    CHECK(!memcmp(win, calm, (size_t)S * S * 2), "the windows are the same frame");

    /* a touch on the ring's right: dust there, the left untouched; the windows the panel is
     * sent (only the rows that changed) make the same frame */
    const int ti = (int)((0.9f + 1.f) * 0.5f * W), tj = H / 2;
    for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
        const float r2 = (float)((i - ti) * (i - ti) + (j - tj) * (j - tj));
        fr->h[j * W + i] = (int16_t)(3.f * FQ_H * expf(-r2 / 18.f));
    }
    memcpy(fb, calm, (size_t)S * S * 2);
    fluid_draw_cells(d, fr, &o);
    fluid_draw_rect(d, &o, &(fluid_rect_t){ fb, S, 0, 0, S, S }, false);
    fluid_draw_send(d, &o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, false, wbuf, S * 14, send_fb, win);
    CHECK(!memcmp(win, fb, (size_t)S * S * 2) && memcmp(fb, calm, (size_t)S * S * 2), "touch: the windows sent are the same frame");
    int dust = 0, near = 0, left = 0;
    for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
        const int k = j * W + i;
        if ((i - ti) * (i - ti) + (j - tj) * (j - tj) <= 9) { near++; dust += (d->key[k] & 0xF000) != 0x8000 || (d->key[k] >> 8 & 15) < 8; }
    }
    for (int y = 0; y < S; y++) for (int x = 0; x < S / 3; x++) left += fb[y * S + x] != calm[y * S + x];
    CHECK(dust > near / 2 && left == 0, "touch: %d of %d cells by the finger in dust, %d px changed on the far side", dust, near, left);
    memset(fr->h, 0, sizeof(int16_t) * (size_t)N);
    fluid_draw(d, fr, fb, &o, &y0, &y1);
    CHECK(!memcmp(fb, calm, (size_t)S * S * 2), "calm again: the very same pixels");

    /* a transition: the shapes fade into the dust, all dust, then whole again */
    o.dust = 0.15f; fluid_draw(d, fr, fb, &o, &y0, &y1);
    long ring_calm = 0, ring_dim = 0;                                   /* along the outer ring */
    for (int a = 0; a < 360; a++) {
        const int q = (int)(c0 + R * sinf(a * 0.0174533f)) * S + (int)(c0 + R * cosf(a * 0.0174533f));
        ring_calm += lum(calm[q]); ring_dim += lum(fb[q]);
    }
    CHECK(ring_dim > ring_calm / 4 && ring_dim < ring_calm * 3 / 4, "dust 0.15: the ring dimmed (%ld of %ld)", ring_dim, ring_calm);
    o.dust = 1.f; fluid_draw(d, fr, fb, &o, &y0, &y1);
    int lit = 0;
    for (int q = 0; q < S * S; q++) lit += fb[q] != 0;
    CHECK(lit == 0, "dust 1: gone (%d px)", lit);
    o.dust = 0.f; fluid_draw(d, fr, fb, &o, &y0, &y1);
    CHECK(!memcmp(fb, calm, (size_t)S * S * 2), "dust 0: the same pixels again");

    /* the cost: every cell redrawn (a full window pass) against the dots alone */
    const int R2 = 40;
    double t0 = now_us();
    for (int q = 0; q < R2; q++) { d->force = true; fluid_draw(d, fr, fb, &o, &y0, &y1); }
    const double hd_us = (now_us() - t0) / R2;
    o.shapes = NULL;
    t0 = now_us();
    for (int q = 0; q < R2; q++) { d->force = true; fluid_draw(d, fr, fb, &o, &y0, &y1); }
    const double dot_us = (now_us() - t0) / R2;
    printf("  high definition (host): a full redraw of the chart %.0f us, on the dots %.0f us (%d shapes)\n", hd_us, dot_us, hd.n);

    free(dots); free(keep); free(fb); free(calm); free(win);
    fluid_draw_destroy(d); fluid_frame_destroy(fr); fluid_sim_destroy(s);
    scene_free(sc);
    pkg_unref(p);
}

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
    fluid_letter_t L[FLUID_MAX_LETTERS]; int nl;
    const int N = 2000;
    double t0 = now_us();
    for (int k = 0; k < N; k++) scene_fill(s, sc, k * DT, NULL, L, &nl, NULL, NULL);
    const double scene_us = (now_us() - t0) / N;
    CHECK(nl == 12 + 40, "12 crisp symbols and 40 letters on the overlay (%d)", nl);

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

int main(int argc, char **argv) {
    size_t len; const char *err;
    char path[512];
    snprintf(path, sizeof(path), "%s/shapes.json", argc > 1 ? argv[1] : ".");
    char *fix = read_file(path, &len);
    if (!fix || !(P = pkg_parse(fix, len, &err))) { printf("usage: scene_test fixtures-dir creature.molty\n"); return 2; }
    free(fix);
    test_build();
    test_player();
    test_fill();
    test_occlusion();
    test_spots();
    test_caption();
    test_hairlines();
    test_hd();
    test_real_package(argv[1]);
    test_hd_pixels(argv[1]);
    bench(argc > 2 ? argv[2] : "");
    CHECK(P->refs == 1, "every scene gave its package reference back (%d)", P->refs);
    pkg_unref(P);
    printf("scene_test: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
