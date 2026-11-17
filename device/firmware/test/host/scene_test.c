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
 * Fine dust (test_fine, packages/astrology/PORT-SPEC.md): the chart's specks gather toward its lines, never on them,
 * fainter round its symbols; the light flows through them; a tapped spot's drawing stays still and crisp while the wave
 * leaves its outline; the reading's word sits in the middle on a dark backdrop, forms from the dust and breaks into it;
 * and each state's panel time by the device's model (0.25 ms a window, 0.25 us a pixel).
 *
 * Build + run: test/host/pkg_build.sh
 */
#define _POSIX_C_SOURCE 200809L
#include "scene_player.h"
#include "fluid_director.h"
#include "cute_spec.h"
#include "cute_rig.h"
#include "glyph_font.h"
#include "fluid_fine.h"

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
 * rings are true circles of anti-aliased pixels (not the dot lattice), its symbols too, over the
 * fluid's own dust: dim, shimmering, kept off the drawing; a touch
 * turns only the cells near the finger into dust, and calm water gives back the very same
 * pixels; so does a transition. The panel's windows (fluid_draw_send) are the same frame. ── */

static int s_sends;
static int send_fb(void *ctx, int x, int y, int w, int h, const uint16_t *px) {
    uint16_t *fb = ctx;
    s_sends++;
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
    scene_shapes_t hd = { shp, 0, keep, NULL };
    scene_fill(s, sc, 1.f, NULL, L, &nl, dots, &hd);
    int rings = 0, segs = 0, masks = 0;
    for (int n = 0; n < hd.n; n++) { rings += shp[n].k == FLUID_SHAPE_RING; segs += shp[n].k == FLUID_SHAPE_SEG; masks += shp[n].k == FLUID_SHAPE_MASK; }
    CHECK(rings == 2 && segs > 12 + 36 && masks == nl && nl > 12, "its shapes: %d rings (no middle ring), %d strokes, %d symbols (%d letters)", rings, segs, masks, nl);
    fluid_sim_snapshot(s, fr, false);
    memset(fr->h, 0, sizeof(int16_t) * (size_t)N);
    for (int k = 0; k < N; k++) fr->view[k] = (int16_t)(0.2f * FQ_UNIT);   /* the fluid's haze */
    fluid_draw_opts_t o = { .palette = FLUID_PAL_PASTEL, .edge_mode = FLUID_EDGE_CONTOURS, .letters = L, .nletters = nl, .font = gc_font5x7,
                            .dots = dots, .scene = true, .shapes = shp, .nshapes = hd.n, .kdots = keep };
    int y0, y1;
    fluid_draw(d, fr, fb, &o, &y0, &y1);
    memcpy(calm, fb, (size_t)S * S * 2);

    /* the shapes alone: the frame with every dot taken off */
    uint16_t *only = calloc((size_t)S * S, 2);
    {
        uint16_t *keys = malloc(sizeof(uint16_t) * (size_t)N);
        memcpy(keys, d->key, sizeof(uint16_t) * (size_t)N);
        for (int k = 0; k < N; k++) if ((d->key[k] & 0xF000) == 0x8000) d->key[k] &= 0xFF00;
        fluid_draw_rect(d, &o, &(fluid_rect_t){ only, S, 0, 0, S, S }, true);
        memcpy(d->key, keys, sizeof(uint16_t) * (size_t)N);
        free(keys);
    }
    /* the outer ring (r 0.9, a hairline): lit at every degree, a pixel or two wide, its edge soft */
    const float R = 0.9f * S / 2, c0 = S / 2.f;
    int hit = 0, soft = 0, lattice = 0, wide = 0;
    for (int a = 0; a < 360; a++) {
        const float co = cosf(a * 0.0174533f), si = sinf(a * 0.0174533f);
        int best = 0, on = 0;
        for (float dr = -3.f; dr <= 3.f; dr += 0.5f) { const int v = lum(only[(int)(c0 + (R + dr) * si) * S + (int)(c0 + (R + dr) * co)]); if (v > best) best = v; on += v > 200; }
        hit += best > 200;
        wide += on > 6;                                              /* over 3 px of it (where a tick meets it) */
        for (float dr = -1.5f; dr <= 1.5f; dr += 0.5f) {
            const int v = lum(only[(int)(c0 + (R + dr) * si) * S + (int)(c0 + (R + dr) * co)]);
            soft += v > 20 && v < best * 3 / 4;
        }
    }
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++)
        if (fb[y * S + x] && d->yrow[y] < 0) lattice++;                 /* lit between the dot rows */
    CHECK(hit == 360 && wide <= 12 && soft > 200 && lattice > 2000, "calm: a true ring (%d of 360 degrees), fine (%d degrees wider than 3 px), soft edged (%d), lit off the dot rows (%d px)",
          hit, wide, soft, lattice);
    /* the dust takes the drawing's shape: denser the nearer it (in dots), none on it, dimmer than it */
    int tot[5] = { 0 }, on[5] = { 0 }, bright = 0;
    for (int j = 1; j < H - 1; j++) for (int i = 1; i < W - 1; i++) {
        const int k = j * W + i;
        if ((d->key[k] & 0xF000) != 0x8000 || keep[k]) continue;
        for (int q = 0; q < 8; q++) {
            const int dd = d->halo[(4 * j + (q & 3)) * 2 * W + 2 * i + (q >> 2)], b = dd > 4 ? 4 : dd;
            tot[b]++;
            on[b] += d->key[k] >> fluid_braille_bit(q >> 2, q & 3) & 1;
        }
        bright += (d->key[k] & 0xFF) && lum(d->col[k]) > 430;
    }
    float fr_[5];
    for (int q = 0; q < 5; q++) fr_[q] = tot[q] ? (float)on[q] / tot[q] : 0.f;
    CHECK(on[0] == 0 && fr_[1] > fr_[2] && fr_[2] > fr_[3] && fr_[3] > fr_[4] && fr_[4] > 0.02f && bright == 0,
          "calm: the dust by distance to the drawing %.2f %.2f %.2f %.2f, far %.2f; %d on it, %d cells bright", fr_[0], fr_[1], fr_[2], fr_[3], fr_[4], on[0], bright);
    int under = 0, line = 0;                                            /* the dust never on them */
    for (int q = 0; q < S * S; q++) if (only[q]) { line++; under += fb[q] != only[q]; }
    CHECK(line > 5000 && under == 0, "calm: no dust on the drawing's %d px (%d covered)", line, under);
    free(only);
    int hdc = 0, keyd = 0;
    for (int k = 0; k < N; k++) { hdc += (d->key[k] & 0xF000) == 0x8000; keyd += d->key[k] && d->key[k] < 0x100; }
    CHECK(hdc > 100 && keyd == 0, "calm: %d cells drawn smooth, %d as dots", hdc, keyd);
    fluid_draw(d, fr, fb, &o, &y0, &y1);
    CHECK(y1 == y0, "calm: nothing redrawn the next frame ([%d, %d))", y0, y1);
    /* the dust shimmers slowly: a second of calm frames reseeds every row once, each frame a row or
     * so, sent as windows of the cells that changed (the same frame as the framebuffer's), never
     * the drawing between: a calm frame sends a few percent of the panel */
    static uint16_t wbuf[466 * 14];
    memcpy(win, calm, (size_t)S * S * 2);
    long sentsum = 0, dirtysum = 0;
    int sentmax = 0, sends = 0, same = 1;
    double calm_us = 0.;
    for (int f = 1; f <= 30; f++) {
        o.time = f / 30.f;
        const double tc = now_us();
        fluid_draw_cells(d, fr, &o);
        calm_us += now_us() - tc;
        fluid_draw_rect(d, &o, &(fluid_rect_t){ fb, S, 0, 0, S, S }, false);
        s_sends = 0;
        const double ts = now_us();
        const int px = fluid_draw_send(d, &o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, false, wbuf, S * 14, send_fb, win);
        calm_us += now_us() - ts;
        for (int k = 0; k < N; k++) if (d->dirty[k]) dirtysum += (d->cx[k % W + 1] - d->cx[k % W]) * (d->cy[k / W + 1] - d->cy[k / W]);
        sentsum += px; sends += s_sends;
        if (px > sentmax) sentmax = px;
        same &= !memcmp(win, fb, (size_t)S * S * 2);
    }
    CHECK(same && memcmp(fb, calm, (size_t)S * S * 2) && sentsum / 30 < S * S / 20 && sentmax < S * S / 8 && sentsum < 2 * dirtysum,
          "calm: the dust shimmers, %ld px a frame sent (most %d, of %d), %.1f windows, the same frame", sentsum / 30, sentmax, S * S, sends / 30.f);
    double full_us = now_us();
    for (int f = 0; f < 10; f++) {
        d->force = true;
        fluid_draw_cells(d, fr, &o);
        fluid_draw_send(d, &o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, false, wbuf, S * 14, send_fb, win);
    }
    full_us = (now_us() - full_us) / 10;
    printf("  calm frame (host): %ld px sent on average (most %d), %.1f windows; drawn in %.0f us against %.0f us redrawing all\n",
           sentsum / 30, sentmax, sends / 30.f, calm_us / 30, full_us);
    o.time = 0.f;
    fluid_draw(d, fr, fb, &o, &y0, &y1);

    /* the panel's windows draw the same frame */
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
    o.dust = 0.2f; fluid_draw(d, fr, fb, &o, &y0, &y1);
    long ring_calm = 0, ring_dim = 0;                                   /* along the outer ring */
    for (int a = 0; a < 360; a++) {
        const int q = (int)(c0 + R * sinf(a * 0.0174533f)) * S + (int)(c0 + R * cosf(a * 0.0174533f));
        ring_calm += lum(calm[q]); ring_dim += lum(fb[q]);
    }
    CHECK(ring_dim < ring_calm / 2, "dust 0.2: the shapes give way to the dust at once (%ld of %ld)", ring_dim, ring_calm);
    o.dust = 1.f; fluid_draw(d, fr, fb, &o, &y0, &y1);
    int lit = 0;
    for (int q = 0; q < S * S; q++) lit += lum(fb[q]) > 260;
    CHECK(lit == 0, "dust 1: gone, but the fluid's dim dust (%d bright px)", lit);
    o.dust = 0.f; fluid_draw(d, fr, fb, &o, &y0, &y1);
    CHECK(!memcmp(fb, calm, (size_t)S * S * 2), "dust 0: the same pixels again");

    /* a tap on the Moon's spot: its symbol grows with the ripple, holds while its reading plays,
     * then eases back to exactly the pixels it had; the dust's shape follows it */
    int moon = -1;
    for (int n = 0; n < sc->nitems; n++)
        if (sc->items[n].k == PK_SPOT && sc->items[n].nsid == 4 && !memcmp(sc->say + sc->items[n].sid, "moon", 4)) moon = n;
    float w0 = 0.f;
    for (int n = 0; n < hd.n; n++) {
        const float gx = shp[n].x / (S / 2.f) - 1.f, gy = 1.f - shp[n].y / (S / 2.f), dx = gx - sc->items[moon].x, dy = gy - sc->items[moon].y;
        if (moon >= 0 && shp[n].k == FLUID_SHAPE_MASK && dx * dx + dy * dy < 1e-4f) w0 = shp[n].w;
    }
    CHECK(moon >= 0 && w0 > 0.f, "the Moon's spot and symbol (%.1f px)", w0);
    scene_grow_t g;
    scene_grow_tap(&g, moon);
    const uint32_t sig0 = d->halo_sig;
    float wmid = 0.f, whold = 0.f;
    int nt = 0;
    for (; nt < 60; nt++) {
        scene_grow_tick(&g, true, DT);
        scene_grow_apply(&g, sc);
        scene_fill(s, sc, 1.f, NULL, L, &nl, dots, &hd);
        fluid_draw(d, fr, fb, &o, &y0, &y1);
        float w = 0.f;
        for (int n = 0; n < hd.n; n++) if (shp[n].k == FLUID_SHAPE_MASK && fabsf(shp[n].x - (sc->items[moon].x + 1.f) * S / 2.f) < 3.f) w = fmaxf(w, shp[n].w);
        if (nt == 4) wmid = w;
        whold = w;
    }
    CHECK(wmid > w0 * 1.05f && wmid < w0 * 1.24f && fabsf(whold - w0 * SCENE_GROW) < 0.5f && d->halo_sig != sig0 && memcmp(fb, calm, (size_t)S * S * 2),
          "tap: the symbol grows (%.1f, %.1f, then %.1f px held), the dust follows", w0, wmid, whold);
    for (nt = 0; nt < 60 && g.spot >= 0; nt++) {
        scene_grow_tick(&g, false, DT);
        scene_grow_apply(&g, sc);
        scene_fill(s, sc, 1.f, NULL, L, &nl, dots, &hd);
        fluid_draw(d, fr, fb, &o, &y0, &y1);
    }
    CHECK(g.spot < 0 && nt * DT <= SCENE_GROW_OUT_S + 2 * DT && sc->focus < 0 && !memcmp(fb, calm, (size_t)S * S * 2),
          "its reading over: back in %.2f s, exactly the pixels it had", nt * DT);

    /* a tap on an aspect's line (it wins over the middle's spot under it): the line brightens, its
     * two bodies grow, it reaches out to them with a ring round each; all of it goes back exactly */
    {
        int asp = -1;
        for (int n = 0; n < sc->nitems && asp < 0; n++) if (sc->items[n].k == PK_SPOT && sc->items[n].lights[0]) asp = n;
        CHECK(asp >= 0 && sc->items[asp].npts == 2, "an aspect's spot, along its line");
        if (asp >= 0) {
            const scene_item_t *sp = &sc->items[asp];
            const float *e0 = sc->pts[sp->pts], *e1 = sc->pts[sp->pts + 1];
            const float mx = 0.5f * (e0[0] + e1[0]), my = 0.5f * (e0[1] + e1[1]);
            CHECK(scene_spot_at(sc, 1.f, mx + 0.03f * (e1[1] - e0[1]), my - 0.03f * (e1[0] - e0[0])) == asp && scene_spot_at(sc, 1.f, 0.f, 0.f) >= 0,
                  "a tap by the line lands on the aspect, not the middle");
            /* its two bodies: the glyphs in the groups it lights */
            int body[2] = { -1, -1 }, nb = 0;
            for (int n = 0; n < sc->nitems && nb < 2; n++)
                for (int q = 1; q < SCENE_LIGHTS && sc->items[n].k == PK_GLYPH && sc->items[n].grp; q++) if (sc->items[n].grp == sp->lights[q]) body[nb++] = n;
            int rings0 = 0, segs0 = 0;
            for (int n = 0; n < hd.n; n++) { rings0 += shp[n].k == FLUID_SHAPE_RING; segs0 += shp[n].k == FLUID_SHAPE_SEG; }
            float bw0[2] = { 0.f, 0.f };
            for (int q = 0; q < 2; q++) for (int n = 0; body[q] >= 0 && n < hd.n; n++)
                if (shp[n].k == FLUID_SHAPE_MASK && fabsf(shp[n].x - (sc->items[body[q]].x + 1.f) * S / 2.f) < 1.f && fabsf(shp[n].y - (1.f - sc->items[body[q]].y) * S / 2.f) < 1.f) bw0[q] = shp[n].w;
            scene_grow_t ga;
            scene_grow_tap(&ga, asp);
            for (int q = 0; q < 30; q++) { scene_grow_tick(&ga, true, DT); scene_grow_apply(&ga, sc); scene_fill(s, sc, 1.f, NULL, L, &nl, dots, &hd); fluid_draw(d, fr, fb, &o, &y0, &y1); }
            int rings = 0, segs = 0, ringed = 0;
            float bw[2] = { 0.f, 0.f };
            for (int n = 0; n < hd.n; n++) {
                rings += shp[n].k == FLUID_SHAPE_RING; segs += shp[n].k == FLUID_SHAPE_SEG;
                for (int q = 0; q < 2; q++) {
                    if (body[q] < 0) continue;
                    const float bx = (sc->items[body[q]].x + 1.f) * S / 2.f, by = (1.f - sc->items[body[q]].y) * S / 2.f;
                    if (shp[n].k == FLUID_SHAPE_MASK && fabsf(shp[n].x - bx) < 1.f && fabsf(shp[n].y - by) < 1.f) bw[q] = shp[n].w;
                    ringed += shp[n].k == FLUID_SHAPE_RING && fabsf(shp[n].x - bx) < 1.5f && fabsf(shp[n].y - by) < 1.5f;
                }
            }
            CHECK(nb == 2 && bw0[0] > 0.f && fabsf(bw[0] - bw0[0] * SCENE_GROW) < 0.5f && fabsf(bw[1] - bw0[1] * SCENE_GROW) < 0.5f,
                  "aspect: both its bodies grow (%.1f -> %.1f, %.1f -> %.1f px)", bw0[0], bw[0], bw0[1], bw[1]);
            CHECK(rings == rings0 + 2 && ringed == 2 && segs == segs0 && memcmp(fb, calm, (size_t)S * S * 2),
                  "aspect: a ring round each (%d more, %d on them), the line already reaching them (%d more strokes)", rings - rings0, ringed, segs - segs0);
            for (int q = 0; q < 60 && ga.spot >= 0; q++) { scene_grow_tick(&ga, false, DT); scene_grow_apply(&ga, sc); scene_fill(s, sc, 1.f, NULL, L, &nl, dots, &hd); fluid_draw(d, fr, fb, &o, &y0, &y1); }
            CHECK(ga.spot < 0 && !memcmp(fb, calm, (size_t)S * S * 2), "aspect: its reading over, exactly the pixels it had");
        }
    }

    /* each subtitle word's ripple: a wobble right around the word, never dust */
    {
        const scene_caption_t cap = { "Gemini", 6, 2 };
        scene_fill(s, sc, 1.f, &cap, L, &nl, dots, &hd);
        o.nshapes = hd.n;
        float wx0 = S, wx1 = 0, wy0 = S, wy1 = 0;
        for (int q = 0; q < nl; q++) {
            if (L[q].mask || L[q].rgb) continue;
            fluid_sim_impulse(s, (L[q].x + L[q].w * 0.5f) / S * 2.f - 1.f, (L[q].y + L[q].h * 0.5f) / S * 2.f - 1.f, SCENE_CAPTION_RIPPLE, SCENE_CAPTION_RIPPLE_R);
            wx0 = fminf(wx0, L[q].x); wx1 = fmaxf(wx1, L[q].x + L[q].w); wy0 = fminf(wy0, L[q].y); wy1 = fmaxf(wy1, L[q].y + L[q].h);
        }
        int dusty = 0, wob = 0, far = 0;
        for (int f = 0; f < 45; f++) {
            fluid_sim_snapshot(s, fr, false);
            for (int k = 0; k < N; k++) fr->view[k] = (int16_t)(0.2f * FQ_UNIT);
            fluid_draw_cells(d, fr, &o);
            for (int j = 1; j < H - 1; j++) for (int i = 1; i < W - 1; i++) {
                const int k = j * W + i;
                const float fx = (fr->h[k + 1] - fr->h[k - 1]) * 3.3f / FQ_H * 2.f, fy = (fr->h[k + W] - fr->h[k - W]) * 3.3f / FQ_H * 4.f * H / W;
                const bool moved = fx * fx + fy * fy >= 0.25f;                  /* half a dot */
                const float cx = (i + 0.5f) * S / W, cy = (j + 0.5f) * S / H;
                wob += moved;
                far += moved && f < 3 && (cx < wx0 - 24.f || cx > wx1 + 24.f || cy < wy0 - 24.f || cy > wy1 + 24.f);
                dusty += (d->key[k] & 0xF000) == 0x8000 && (d->key[k] >> 8 & 15) < 8;
            }
            fluid_sim_step(s, DT);
        }
        CHECK(wob > 0 && far == 0 && dusty == 0 && fabsf((wy0 + wy1) * 0.5f - S * 0.5f) < 4.f,
              "a word's ripple: a wobble (%d cell frames) starting round it (%d farther at first), never dust (%d); the word in the chart's middle (%.0f px)", wob, far, dusty, (wy0 + wy1) * 0.5f);
    }

    /* the drawer's cost while a finger moves on the water and in a transition (frames that change) */
    {
        double touch_us = 0., trans_us = 0.;
        int touch_px = 0, trans_px = 0;
        for (int f = 0; f < 20; f++) {
            for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
                const float di = (float)(i - ti - (f & 3)), r2 = di * di + (float)((j - tj) * (j - tj));
                fr->h[j * W + i] = (int16_t)(3.f * FQ_H * expf(-r2 / 18.f));
            }
            const double t0 = now_us();
            fluid_draw_cells(d, fr, &o);
            touch_px += fluid_draw_send(d, &o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, false, wbuf, S * 14, send_fb, win);
            touch_us += now_us() - t0;
        }
        memset(fr->h, 0, sizeof(int16_t) * (size_t)N);
        double trans_cells = 0.;
        for (int f = 0; f < 20; f++) {
            o.dust = 0.3f + 0.02f * f;
            const double t0 = now_us();
            fluid_draw_cells(d, fr, &o);
            trans_cells += now_us() - t0;
            if (!f) s_sends = 0;
            trans_px += fluid_draw_send(d, &o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, false, wbuf, S * 14, send_fb, win);
            trans_us += now_us() - t0;
        }
        o.dust = 0.f;
        /* the fluid's own look of the same grid, every frame changing (its shimmer): the drawer's
         * work the device keeps up with at 33 fps */
        double look_us = 0.;
        int look_px = 0;
        fluid_draw_opts_t lo = { .palette = FLUID_PAL_PASTEL, .edge_mode = FLUID_EDGE_CONTOURS, .shimmer = 0.12f, .ramp = " .:-=+*#", .font = gc_font5x7 };
        for (int k = 0; k < N; k++) fr->view[k] = (int16_t)(FQ_UNIT * (0.15f + 0.6f * (float)((k * 7919) % 97) / 97.f));
        for (int f = 0; f < 20; f++) {
            lo.time = f / 30.f;
            for (int k = 0; k < N; k++) fr->view[k] = (int16_t)(FQ_UNIT * (0.15f + 0.6f * (float)((k * 7919 + f * 13) % 97) / 97.f));
            const double t0 = now_us();
            fluid_draw_cells(d, fr, &lo);
            look_px += fluid_draw_send(d, &lo, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, false, wbuf, S * 14, send_fb, win);
            look_us += now_us() - t0;
        }
        for (int k = 0; k < N; k++) fr->view[k] = (int16_t)(0.2f * FQ_UNIT);
        d->force = true;
        fluid_draw(d, fr, fb, &o, &y0, &y1);
        printf("  fluid look frame (host): %.0f us, %d px\n", look_us / 20, look_px / 20);
        printf("  touch frame (host): %.0f us, %d px sent; transition frame: %.0f us (cells %.0f), %d px, %d windows\n", touch_us / 20, touch_px / 20, trans_us / 20, trans_cells / 20, trans_px / 20, s_sends / 20);
    }

    /* the fill's cost: the chart's items rasterised (a change), against a frame that keeps them and
     * lays a subtitle word over them */
    {
        const scene_caption_t cap = { "Gemini", 6, 2 };
        const int R3 = 60;
        double t0 = now_us();
        for (int q = 0; q < R3; q++) { sc->focus = q & 1 ? moon : -1; scene_fill(s, sc, 1.f, &cap, L, &nl, dots, &hd); }
        const double full_us = (now_us() - t0) / R3;
        sc->focus = -1;
        scene_fill(s, sc, 1.f, &cap, L, &nl, dots, &hd);
        t0 = now_us();
        for (int q = 0; q < R3; q++) scene_fill(s, sc, 1.f, &cap, L, &nl, dots, &hd);
        const double kept_us = (now_us() - t0) / R3;
        printf("  chart fill (host): %.0f us when its items change, %.0f us a frame otherwise (%.0fx); on the device (23 ms measured): %.1f ms\n",
               full_us, kept_us, full_us / kept_us, 23.0 * kept_us / full_us);
        CHECK(kept_us * 5.0 < full_us, "a calm frame's fill keeps the items' raster (%.0f vs %.0f us)", kept_us, full_us);
    }

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

/* ── fine dust: the chart as the page draws it (packages/astrology/PORT-SPEC.md) ── */
typedef struct { int wins; long px; } cost_t;
static int send_cost(void *ctx, int x, int y, int w, int h, const uint16_t *px) {
    cost_t *c = ctx; (void)x; (void)y; (void)px;
    c->wins++; c->px += (long)w * h;
    return 0;
}
static void outline_bump(void *ctx, float x, float y) { fluid_sim_impulse(ctx, x, -y, SCENE_OUTLINE_WAVE, SCENE_OUTLINE_R); }
static int spot_named(const scene_t *sc, const char *id) {
    for (int n = 0; n < sc->nitems; n++)
        if (sc->items[n].k == PK_SPOT && sc->items[n].nsid == strlen(id) && !memcmp(sc->say + sc->items[n].sid, id, strlen(id))) return n;
    return -1;
}

static void test_fine(const char *dir) {
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
    CHECK(sc && fabsf(sc->caption_y) < 1e-6f, "the chart builds, its caption in the middle");
    if (!sc) { pkg_unref(p); return; }
    const int S = 466;
    int W, H;
    fluid_grid_for_stage(S, &W, &H);
    fluid_sim_t *s = fluid_sim_create(W, H, S);
    s->p.still = true;
    fluid_frame_t *fr = fluid_frame_create(W, H);
    fluid_draw_t *d = fluid_draw_create(W, H, S, S);
    fluid_fine_t *fine = fluid_fine_create(W, H, S, S);
    const int N = W * H;
    uint8_t *dots = calloc((size_t)N, 1), *keep = calloc((size_t)N, 1), *still = calloc((size_t)N, 1);
    static fluid_shape_t shp[FLUID_MAX_SHAPES], full[FLUID_MAX_SHAPES];
    uint16_t *fb = calloc((size_t)S * S, 2), *only = calloc((size_t)S * S, 2);
    static uint16_t wbuf[466 * 18];
    fluid_letter_t L[FLUID_MAX_LETTERS]; int nl;
    scene_shapes_t hd = { shp, 0, keep, still };

    /* the build: the calm drawing's specks */
    uint32_t key;
    const int nf = scene_full_shapes(s, sc, 1.f, full, &key);
    double t0 = now_us();
    fluid_fine_want(fine, key, true, full, nf, FLUID_PAL_PASTEL);
    const bool built = fluid_fine_work(fine, 0);
    const double build_us = now_us() - t0;
    fluid_fine_swap(fine);
    const int nsp = fluid_fine_specks(fine);
    { long ns = 0; const fine_set_t *st = &fine->set[fine->live]; for (int k = 0; k < N; k++) ns += st->nstat[k];
      printf("  fine dust: %d specks, %ld (%.0f%%) never hidden by the light\n", nsp, ns, 100.0 * ns / nsp); }
    CHECK(built && nsp > 10000 && nsp < 80000, "the calm chart's dust: %d specks, built in %.0f us (host)", nsp, build_us);
    /* every speck off the drawing, denser and brighter toward a line, fainter round a symbol */
    static int16_t fbox[FLUID_MAX_SHAPES][4];
    for (int q = 0; q < nf; q++) fluid_shape_box(&full[q], 0, 0, fbox[q]);
    for (int y = 0; y < S; y++) fluid_shapes_row(full, nf, (const int16_t (*)[4])fbox, 0, 0, FLUID_PAL_PASTEL, NULL, 0.f, only + (size_t)y * S, 0, y, 0, S);
    {
        const fine_set_t *st = &fine->set[fine->live];
        int on = 0, band[3] = { 0 }, area[3] = { 0 };
        long lum_near = 0, lum_far = 0; int n_near = 0, n_far = 0;
        for (int n = 0; n < st->n; n++) {
            const int x = st->pos[n] & 511, y = st->pos[n] >> 9 & 511;
            on += lum(only[y * S + x]) > 3 * 40 * 2;
        }
        /* density by distance from the outer ring (r 0.9): its band 1.5..3 px, 6..10 px, 14..19 px outside (nothing else there) */
        const float R = 0.9f * S / 2;
        for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
            const float r = hypotf(x + 0.5f - S / 2.f, y + 0.5f - S / 2.f), dr = R - r;
            const int b = dr <= -1.5f && dr > -3.f ? 0 : dr <= -6.f && dr > -10.f ? 1 : dr <= -14.f && dr > -19.f ? 2 : -1;
            if (b >= 0 && r < S / 2.f - 2.f) area[b]++;
        }
        for (int n = 0; n < st->n; n++) {
            const int x = st->pos[n] & 511, y = st->pos[n] >> 9 & 511;
            const float dr = R - hypotf(x + 0.5f - S / 2.f, y + 0.5f - S / 2.f);
            const int b = dr <= -1.5f && dr > -3.f ? 0 : dr <= -6.f && dr > -10.f ? 1 : dr <= -14.f && dr > -19.f ? 2 : -1;
            if (b < 0) continue;
            band[b]++;
            const uint16_t c = st->col[n];
            const int l = (c >> 11) * 8 + (c >> 5 & 63) * 4 + (c & 31) * 8;
            if (b == 0) { lum_near += l; n_near++; } else if (b == 2) { lum_far += l; n_far++; }
        }
        const float d0 = (float)band[0] / area[0], d1 = (float)band[1] / area[1], d2 = (float)band[2] / area[2];
        CHECK(on == 0 && d0 > 0.35f && d0 > d1 && d1 > d2 && d2 > 0.02f && d2 < 0.1f && lum_near / n_near > lum_far / n_far,
              "the dust gathers into the ring: density %.2f, %.2f, %.2f by its distance, brighter near; %d on the drawing", d0, d1, d2, on);
        /* round a symbol (the Moon's glyph, its box 2..6 px out) much sparser than round a line */
        int moon = spot_named(sc, "moon"), ns = 0, as = 0;
        const float mx = (sc->items[moon].x + 1.f) * S / 2, my = (1.f - sc->items[moon].y) * S / 2, h = 0.12f * S / 4 + 3.f;
        for (int n = 0; n < st->n; n++) {
            const int x = st->pos[n] & 511, y = st->pos[n] >> 9 & 511;
            ns += fabsf(x - mx) < h + 3.f && fabsf(y - my) < h + 3.f && !(fabsf(x - mx) < h - 1.f && fabsf(y - my) < h - 1.f);
        }
        for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) as += fabsf(x - mx) < h + 3.f && fabsf(y - my) < h + 3.f && !(fabsf(x - mx) < h - 1.f && fabsf(y - my) < h - 1.f);
        CHECK((float)ns / as < 0.6f * d0, "round a symbol the dust is fainter (%.2f, a line's %.2f)", (float)ns / as, d0);
    }

    /* calm frames: the light flows (each frame a thin front of specks changes), cheap on the panel */
    scene_fill(s, sc, 1.f, NULL, L, &nl, dots, &hd);
    fluid_sim_snapshot(s, fr, false);
    memset(fr->h, 0, sizeof(int16_t) * (size_t)N);
    fluid_draw_opts_t o = { .palette = FLUID_PAL_PASTEL, .edge_mode = FLUID_EDGE_CONTOURS, .letters = L, .nletters = nl, .font = gc_font5x7,
                            .dots = dots, .scene = true, .shapes = shp, .nshapes = hd.n, .kdots = keep, .fine = fine, .still = still };
    int y0, y1;
    o.time = 1.f;
    fluid_draw(d, fr, fb, &o, &y0, &y1);
    int under = 0, line = 0;
    for (int q = 0; q < S * S; q++) if (only[q] && lum(only[q]) > 200) { line++; under += lum(fb[q]) < lum(only[q]) - 30; }
    CHECK(line > 3000 && under == 0, "the lines show through the dust (%d px, %d dimmed)", line, under);
    double worst = 0., sum = 0., cells_us = 0., calm_px = 0., calm_w = 0.;
    int changed = 0;
    for (int f = 1; f <= 60; f++) {
        o.time = 1.f + f / 30.f;
        cost_t c = { 0, 0 };
        const double tc = now_us();
        fluid_draw_cells(d, fr, &o);
        cells_us += now_us() - tc;
        fluid_draw_send(d, &o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, false, wbuf, 4096, send_cost, &c);
        fluid_draw_rect(d, &o, &(fluid_rect_t){ fb, S, 0, 0, S, S }, false);
        const double ms = c.wins * 0.25 + c.px * 0.25e-3;
        calm_px += c.px; calm_w += c.wins;
        sum += ms; worst = ms > worst ? ms : worst; changed += c.px > 0;
    }
    {   /* the light, as the page's (chart-engine.js light()) */
        float err_l = 0.f;
        for (int gy = 0; gy < fine->lg; gy += 3) for (int gx = 0; gx < fine->lg; gx += 5) {
            const double u = gx * (double)FINE_LIGHT_GRID / S, v = gy * (double)FINE_LIGHT_GRID / S;
            float best = 1.f;
            for (int back = 0; back < FINE_PHASES; back++) {     /* a quarter of its rows move each frame: now or up to 3 frames ago */
            const double T = (o.time - back / 30.0) * FINE_LIGHT_SPEED;
            const double a = sin(6.2832 * (1.4 * u + 0.5 * v) + 0.31 * T + 1.6 * sin(6.2832 * (0.8 * v - 0.55 * u) + 0.21 * T));
            const double bb = sin(6.2832 * (-0.7 * u + 1.25 * v) - 0.27 * T + 1.6 * sin(6.2832 * (0.95 * u + 0.35 * v) - 0.17 * T));
            double r = pow(1 - fabs(a), 3) + pow(1 - fabs(bb), 3);
            r = r > 1 ? 1 : r;
            best = fminf(best, fabsf((float)r - fine->lgrid[gy * fine->lg + gx] / 255.f));
            }
            err_l = fmaxf(err_l, best);
        }
        CHECK(err_l < 0.03f, "the light on the water is the page's (off by %.3f at most)", err_l);
    }
    printf("  fine dust, calm (panel model): %.1f ms a frame, worst %.1f; %.0f kpx and %.1f windows a frame; the dust's frame (cells) %.0f us on the host\n",
           sum / 60, worst, calm_px / 60e3, calm_w / 60.0, cells_us / 60);
    if (getenv("FINE_DUMP")) {   /* the calm frame as a PPM, to look at against the page */
        FILE *pf = fopen(getenv("FINE_DUMP"), "wb");
        if (pf) {
            fprintf(pf, "P6 %d %d 255\n", S, S);
            for (int q = 0; q < S * S; q++) { int r, g, b; const uint16_t le = (uint16_t)(fb[q] >> 8 | fb[q] << 8); r = (le >> 11) * 255 / 31; g = (le >> 5 & 63) * 255 / 63; b = (le & 31) * 255 / 31; fputc(r, pf); fputc(g, pf); fputc(b, pf); }
            fclose(pf);
        }
    }
    CHECK(changed >= 55 && sum / 60 < 14.0 && worst < 20.0, "calm: the light flows every frame (%d of 60), %.1f ms a frame on the panel, worst %.1f", changed, sum / 60, worst);

    /* a tap on an aspect: the wave from its outline, the dust of its grown drawing, its items still and crisp */
    const int asp = spot_named(sc, "mars-saturn");
    CHECK(asp >= 0 && fabsf(scene_spot_grow(sc, asp) - 1.25f) < 1e-6f && fabsf(scene_spot_grow(sc, spot_named(sc, "leo")) - 1.5f) < 1e-6f,
          "a planet's or an aspect's drawing grows 1.25x, a sign's 1.5x");
    int npts = 0; float worst_off = 0.f;
    {
        const float *a = sc->pts[sc->items[asp].pts], *b = sc->pts[sc->items[asp].pts + 1];
        struct { const float *a, *b; int n; float off; } acc = { a, b, 0, 0.f };
        (void)acc;
        npts = scene_outline(sc, asp, outline_bump, s);
        (void)worst_off;
    }
    CHECK(npts > 40, "the wave leaves the aspect's outline: %d bumps", npts);
    scene_grow_t g;
    scene_grow_tap(&g, asp);
    double tap_sum = 0., tap_worst = 0.;
    uint16_t *held = calloc((size_t)S * S, 2);
    int moved_items = 0, still_cells = 0, broke_still = 0;
    for (int f = 0; f < 60; f++) {
        scene_grow_tick(&g, true, DT);
        scene_grow_apply(&g, sc);
        scene_fill(s, sc, 1.f, NULL, L, &nl, dots, &hd);
        o.nshapes = hd.n;
        const int nn = scene_full_shapes(s, sc, 1.f, full, &key);
        fluid_fine_want(fine, key, false, full, nn, FLUID_PAL_PASTEL);
        if (fluid_fine_work(fine, 0)) fluid_fine_swap(fine);
        fluid_sim_step(s, DT);
        fluid_sim_snapshot(s, fr, false);
        o.time = 3.f + f / 30.f;
        cost_t c = { 0, 0 };
        fluid_draw_cells(d, fr, &o);
        fluid_draw_rect(d, &o, &(fluid_rect_t){ fb, S, 0, 0, S, S }, false);
        fluid_draw_send(d, &o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, false, wbuf, 4096, send_cost, &c);
        const double ms = c.wins * 0.25 + c.px * 0.25e-3;
        tap_sum += ms; tap_worst = ms > tap_worst ? ms : tap_worst;
        if (getenv("FINE_TRACE")) printf("    tap f%d: %.1f ms (%d windows, %ld px)\n", f, ms, c.wins, c.px);
        if (f == 20) {                                  /* the drawing grown: its own lit pixels in the tapped items' cells */
            memcpy(held, fb, (size_t)S * S * 2);
            static int16_t gb[FLUID_MAX_SHAPES][4];
            for (int q = 0; q < nn; q++) fluid_shape_box(&full[q], 0, 0, gb[q]);
            memset(only, 0, (size_t)S * S * 2);
            for (int y = 0; y < S; y++) fluid_shapes_row(full, nn, (const int16_t (*)[4])gb, 0, 0, FLUID_PAL_PASTEL, NULL, 0.f, only + (size_t)y * S, 0, y, 0, S);
        }
        if (f > 20) for (int k = 0; k < N; k++) if (still[k] == 2) {
            const int i = k % W, j = k / W;
            for (int y = d->cy[j]; y < d->cy[j + 1]; y++) for (int x = d->cx[i]; x < d->cx[i + 1]; x++)
                moved_items += lum(only[y * S + x]) > 200 && held[y * S + x] != fb[y * S + x];
        }
        for (int k = 0; k < N; k++) { still_cells += still[k] != 0; broke_still += still[k] && (d->key[k] & 0xF000) == 0x8000 && (d->key[k] >> 8 & 15) < 8; }
    }
    printf("  fine dust, tapping an aspect (panel model): %.1f ms a frame, worst %.1f\n", tap_sum / 60, tap_worst);
    CHECK(still_cells > 0 && broke_still == 0 && moved_items == 0, "tap: the aspect's drawing stays still and crisp (%d cell frames held, %d broken, %d lit px moved)", still_cells, broke_still, moved_items);
    CHECK(tap_worst < 30.0, "tap: under a frame on the panel (%.1f ms mean, worst %.1f)", tap_sum / 60, tap_worst);

    /* the reading in the middle: each word forms from the dust on a dark backdrop, the last breaks up into the water */
    const char *words[] = { "Mars", "opposes", "Saturn:", "drive", "meets", "limits." };
    double rd_sum = 0., rd_worst = 0.;
    int frames = 0, dark_bad = 0, half_ok = 0, broke = 0;
    float tw = 0.f;
    for (int w = 0; w <= 6; w++) {
        const scene_caption_t cap = w < 6 ? (scene_caption_t){ words[w], (int)strlen(words[w]), 1 } : (scene_caption_t){ NULL, 0, -1 };
        for (int f = 0; f < 9; f++, frames++) {
            scene_fill(s, sc, 1.f, &cap, L, &nl, dots, &hd);
            o.nshapes = hd.n; o.letters = L; o.nletters = nl;
            if (!f && cap.n) for (int q = 0; q < nl; q++) if (!L[q].mask && !L[q].rgb && L[q].ch != ' ')
                fluid_sim_impulse(s, (L[q].x + L[q].w * 0.5f) / S * 2.f - 1.f, (L[q].y + L[q].h * 0.5f) / S * 2.f - 1.f, SCENE_CAPTION_RIPPLE, SCENE_CAPTION_RIPPLE_R);
            o.cap_word = cap.n ? (unsigned)(w + 1) : 0;
            o.cap_unformed = fmaxf(0.f, 1.f - (f * DT) / FINE_REVEAL_S);
            fluid_sim_step(s, DT);
            fluid_sim_snapshot(s, fr, false);
            o.time = 5.f + (tw += DT);
            cost_t c = { 0, 0 };
            fluid_draw_cells(d, fr, &o);
            fluid_draw_rect(d, &o, &(fluid_rect_t){ fb, S, 0, 0, S, S }, false);
            fluid_draw_send(d, &o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, false, wbuf, 4096, send_cost, &c);
            const double ms = c.wins * 0.25 + c.px * 0.25e-3;
            rd_sum += ms; rd_worst = ms > rd_worst ? ms : rd_worst;
            if (getenv("FINE_TRACE")) printf("    read w%d f%d: %.1f ms (%d windows, %ld px)\n", w, f, ms, c.wins, c.px);
            if (cap.n && f == 1 && fine->cbox[1] > fine->cbox[0]) {   /* half formed */
                int shown = 0, all = 0;
                for (int y = (int)fine->cbox[2]; y < (int)fine->cbox[3]; y++) for (int x = (int)fine->cbox[0]; x < (int)fine->cbox[1]; x++) all += lum(fb[y * S + x]) > 600;
                (void)shown;
                half_ok += all > 0;
            }
            if (cap.n && f == 8) {                        /* whole: no dust on its backdrop */
                const fine_set_t *st = fine->drawn;
                for (int n = 0; st && n < st->n; n++) {
                    const int x = st->pos[n] & 511, y = st->pos[n] >> 9 & 511;
                    if (x > fine->cbox[0] && x < fine->cbox[1] && y > fine->cbox[2] && y < fine->cbox[3] && fine->lv[n] > 0.17f * 255) { if (getenv("FINE_TRACE") && dark_bad < 5) printf("    dark: %d,%d lv %d box %.1f %.1f %.1f %.1f\n", x, y, fine->lv[n], fine->cbox[0], fine->cbox[1], fine->cbox[2], fine->cbox[3]); dark_bad++; }
                }
            }
            if (w == 6 && f == 4) broke = fine->npd;
        }
    }
    printf("  fine dust, reading to its end (panel model): %.1f ms a frame, worst %.1f\n", rd_sum / frames, rd_worst);
    CHECK(dark_bad == 0 && half_ok > 0 && broke > 50, "reading: each word on dark (%d specks on it), forming (%d), the last breaking into %d specks", dark_bad, half_ok, broke);
    CHECK(rd_worst < 30.0, "reading: under a frame on the panel (%.1f ms mean, worst %.1f)", rd_sum / frames, rd_worst);

    /* back to calm: the calm set comes back at once (kept) */
    scene_grow_tick(&g, false, 1.f);
    scene_grow_apply(&g, sc);
    const int nn = scene_full_shapes(s, sc, 1.f, full, &key);
    fluid_fine_want(fine, key, true, full, nn, FLUID_PAL_PASTEL);
    CHECK(fine->ready >= 0 && fine->build < 0 && fine->set[fine->ready].n == nsp, "untapped: the calm dust is back without a build");
    free(held);
    free(dots); free(keep); free(still); free(fb); free(only);
    fluid_fine_destroy(fine); fluid_draw_destroy(d); fluid_frame_destroy(fr); fluid_sim_destroy(s);
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
    test_fine(argv[1]);
    bench(argc > 2 ? argv[2] : "");
    CHECK(P->refs == 1, "every scene gave its package reference back (%d)", P->refs);
    pkg_unref(P);
    printf("scene_test: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
