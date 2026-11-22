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
 * Fine dust (test_fine, the chart design): the chart's specks gather toward its lines, never on them,
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
#include "glyph_rsvp.h"
#include "fluid_fine.h"
#include "fluid_tx.h"
#include "fluid_askview.h"
#include "creature_pkg.h"

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
    /* a sector: a spot is also tappable over a ring sector about the glass centre (arc degrees round its own direction,
     * r0..r1); a spot that holds the tap on its own disc or segment wins over one that holds it by its sector only */
    static const char *WEDGE =
        "{\"v\":\"molty-pkg/1\",\"id\":\"w\",\"name\":\"W\",\"version\":\"1\",\"kind\":\"scenes\",\"layouts\":{\"a\":{\"items\":["
        "{\"k\":\"spot\",\"polar\":[90,0.8],\"r\":0.05,\"say\":\"north\",\"arc\":30,\"r0\":0.7,\"r1\":0.9},"
        "{\"k\":\"spot\",\"polar\":[120,0.8],\"r\":0.05,\"say\":\"next\",\"arc\":30,\"r0\":0.7,\"r1\":0.9},"
        "{\"k\":\"spot\",\"polar\":[100,0.75],\"r\":0.06,\"say\":\"a planet in the wedge\"}]}}}";
    pkg_t *w = pkg_parse(WEDGE, strlen(WEDGE), &err);
    scene_t *ws = w ? scene_build(w, "a", NULL, 0, &err) : NULL;
    CHECK(ws, "a sector spot parses and builds (%s)", err ? err : "");
    if (ws) {
        const float D = 0.017453293f;
#define AT(deg, r) scene_spot_at(ws, 0.f, (r) * cosf((deg) * D), (r) * sinf((deg) * D))
        CHECK(AT(90, 0.8f) == 0 && AT(76, 0.71f) == 0 && AT(104, 0.89f) == 0 && AT(76, 0.89f) == 0 && AT(104.5f, 0.71f) == 0, "its centre and its four corners hit it");
        CHECK(AT(90, 0.69f) == -1 && AT(90, 0.91f) == -1 && AT(74, 0.8f) == -1, "just inside, outside and beside it: nothing");
        CHECK(AT(106, 0.8f) == 1 && AT(134, 0.8f) == 1, "the next wedge takes over at the boundary");
        CHECK(AT(100, 0.75f) == 2 && AT(98, 0.72f) == 2, "a spot's own disc inside a wedge keeps the tap");
#undef AT
    }
    scene_free(ws);
    pkg_unref(w);
    static const char *OPENS = "{\"v\":\"molty-pkg/1\",\"id\":\"x\",\"name\":\"X\",\"version\":\"1\",\"kind\":\"scenes\",\"layouts\":{"
        "\"a\":{\"items\":[{\"k\":\"spot\",\"r\":0.2,\"say\":\"s\",\"open\":\"%s\"%s}]},\"b\":{\"dur\":3,\"items\":[{\"k\":\"disc\",\"r\":0.1}]}}}";
    char ob[512];
    snprintf(ob, sizeof ob, OPENS, "b", ",\"ttl\":5");
    pkg_t *po = pkg_parse(ob, strlen(ob), &err);
    scene_t *oa = po ? scene_build(po, "a", NULL, 0, &err) : NULL, *obb = oa ? scene_open_build(oa, 0, &err) : NULL;
    CHECK(po && oa && obb && !strcmp(obb->layout, "b") && fabsf(scene_end(obb) - 3.f) < 1e-6f, "a spot opens another layout of its package, for that one's own length");
    scene_free(obb); scene_free(oa); pkg_unref(po);
    snprintf(ob, sizeof ob, OPENS, "nowhere", "");
    po = pkg_parse(ob, strlen(ob), &err);
    CHECK(!po && !strcmp(err, "item: open names no layout"), "it must name one (%s)", err ? err : "");
    pkg_unref(po);
    static const char *TTL_ONLY = "{\"v\":\"molty-pkg/1\",\"id\":\"x\",\"name\":\"X\",\"version\":\"1\",\"kind\":\"scenes\",\"layouts\":{\"a\":{\"items\":[{\"k\":\"spot\",\"r\":0.2,\"say\":\"s\",\"ttl\":5}]}}}";
    po = pkg_parse(TTL_ONLY, strlen(TTL_ONLY), &err);
    CHECK(!po, "ttl is for a spot that opens");
    pkg_unref(po);
    static const char *HALF = "{\"v\":\"molty-pkg/1\",\"id\":\"x\",\"name\":\"X\",\"version\":\"1\",\"kind\":\"scenes\",\"layouts\":{\"a\":{\"items\":[{\"k\":\"spot\",\"r\":0.2,\"say\":\"s\",\"arc\":30}]}}}";
    pkg_t *h = pkg_parse(HALF, strlen(HALF), &err);
    CHECK(!h, "a sector needs its outer radius");
    pkg_unref(h);
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

static int spot_named(const scene_t *sc, const char *id) {
    for (int n = 0; n < sc->nitems; n++)
        if (sc->items[n].k == PK_SPOT && sc->items[n].nsid == strlen(id) && !memcmp(sc->say + sc->items[n].sid, id, strlen(id))) return n;
    return -1;
}

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
    int nh = -1;                                        /* (frames it stays grown: the backdrop lifting) */
    for (nt = 0; nt < 60 && g.spot >= 0; nt++) {
        scene_grow_tick(&g, false, DT);
        scene_grow_apply(&g, sc);
        if (nh < 0 && sc->grow < scene_spot_grow(sc, moon)) nh = nt;
        scene_fill(s, sc, 1.f, NULL, L, &nl, dots, &hd);
        fluid_draw(d, fr, fb, &o, &y0, &y1);
    }
    CHECK(g.spot < 0 && fabsf(nh * DT - SCENE_GROW_HOLD_S) <= DT && (nt - nh) * DT <= SCENE_GROW_OUT_S + 2 * DT && sc->focus < 0 && !memcmp(fb, calm, (size_t)S * S * 2),
          "its reading over: held %.2f s, back in %.2f s, exactly the pixels it had", nh * DT, (nt - nh) * DT);

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

    /* the signs' wedges: a tap anywhere in a sign's 30 degree block of the zodiac ring lands on it; a planet keeps its own */
    {
        const float D = 0.017453293f;
        const int virgo = spot_named(sc, "virgo"), libra = spot_named(sc, "libra"), sun = spot_named(sc, "moon");
        const float va = 345.f;                                     /* Virgo: sign 5, centred at 195 + 30 x 5 */
#define AT(deg, r) scene_spot_at(sc, 1.f, (r) * cosf((deg) * D), (r) * sinf((deg) * D))
        CHECK(virgo >= 0 && AT(va, 0.825f) == virgo && AT(va - 14.f, 0.76f) == virgo && AT(va + 14.f, 0.76f) == virgo && AT(va - 14.f, 0.89f) == virgo &&
              AT(va + 14.f, 0.89f) == virgo, "Virgo's wedge: its centre and four corners");
        CHECK(AT(va + 16.f, 0.8f) == libra && AT(va, 0.95f) == -1, "past its edge the next sign; outside the ring nothing");
        CHECK(sun >= 0 && scene_spot_at(sc, 1.f, sc->items[sun].x + 0.03f, sc->items[sun].y - 0.03f) == sun &&
              scene_spot_at(sc, 1.f, sc->items[sun].x + 0.07f, sc->items[sun].y) == sun, "a tap on a planet (the Moon, by Pisces' wedge) is the planet's");
#undef AT
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
        for (int q = 0; q < R3; q++) { sc->focus = q & 1 ? moon : -1; sc->glow = q & 1 ? 1.f : 0.f; scene_fill(s, sc, 1.f, &cap, L, &nl, dots, &hd); }
        const double full_us = (now_us() - t0) / R3;
        sc->focus = -1; sc->glow = 0.f;
        scene_fill(s, sc, 1.f, &cap, L, &nl, dots, &hd);
        t0 = now_us();
        for (int q = 0; q < R3; q++) scene_fill(s, sc, 1.f, &cap, L, &nl, dots, &hd);
        const double kept_us = (now_us() - t0) / R3;
        printf("  chart fill (host): %.0f us when its items change, %.0f us a frame otherwise (%.0fx); on the device (23 ms measured): %.1f ms\n",
               full_us, kept_us, full_us / kept_us, 23.0 * kept_us / full_us);
        CHECK(kept_us * 3.0 < full_us, "a calm frame's fill keeps the items' raster (%.0f vs %.0f us)", kept_us, full_us);
        /* in high definition with its still cells the grown drawing never shows on the dots: a tap repaints nothing */
        uint8_t *stl = calloc((size_t)N, 1);
        scene_shapes_t hs = { shp, 0, keep, stl };
        scene_fill(s, sc, 1.f, &cap, L, &nl, dots, &hs);
        t0 = now_us();
        for (int q = 0; q < R3; q++) { sc->focus = q & 1 ? moon : -1; sc->glow = q & 1 ? 1.f : 0.f; scene_fill(s, sc, 1.f, &cap, L, &nl, dots, &hs); }
        const double tap_us = (now_us() - t0) / R3;
        sc->focus = -1; sc->glow = 0.f;
        scene_fill(s, sc, 1.f, &cap, L, &nl, dots, &hd);
        free(stl);
        CHECK(tap_us * 2.5 < full_us, "a tap in high definition repaints no dots (%.0f vs %.0f us)", tap_us, full_us);
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

/* ── a tap opens a layout on the device itself (a spot's `open`): the chart's sign wedges open that sign up close, from
 * the chart's own params; it plays over the chart, which comes back when it ends or at a tap beside it, its clock
 * having run on ── */
static void test_open(const char *dir) {
    char path[512]; size_t len; const char *err;
    snprintf(path, sizeof(path), "%s/astrology.json", dir);
    char *buf = read_file(path, &len);
    pkg_t *p = buf ? pkg_parse(buf, len, &err) : NULL;
    free(buf);
    snprintf(path, sizeof(path), "%s/astrology-commands.jsonl", dir);
    char *cmds = p ? read_file(path, &len) : NULL;
    scene_t *chart = NULL;
    cJSON *args = NULL;
    for (char *line = cmds ? strtok(cmds, "\n") : NULL; line && !chart; line = strtok(NULL, "\n")) {
        cJSON *c = cJSON_Parse(line), *a = cJSON_GetObjectItem(c, "args");
        const char *layout = cJSON_GetStringValue(cJSON_GetObjectItem(a, "layout"));
        if (layout && !strcmp(layout, "chart")) { chart = scene_build(p, layout, cJSON_GetObjectItem(a, "params"), 100.f, &err); args = cJSON_Duplicate(cJSON_GetObjectItem(a, "params"), true); }
        cJSON_Delete(c);
    }
    free(cmds);
    CHECK(chart && chart->params, "the chart builds and keeps its params (a spot of it opens a layout)");
    if (!chart) { pkg_unref(p); return; }
    const float D = 0.017453293f;
    const int leo = spot_named(chart, "leo"), moon = spot_named(chart, "moon");
    /* Leo: sign 4, centred at 195 + 30 x 4 = 315 degrees; a corner of its wedge */
    CHECK(leo >= 0 && scene_spot_at(chart, 1.f, 0.76f * cosf(302.f * D), 0.76f * sinf(302.f * D)) == leo && chart->items[leo].nopen, "a tap in Leo's wedge lands on Leo, which opens a layout");
    /* how many bodies the sky has in Leo, and how long its reading plays */
    int inleo = 0; float ttl = 0.f;
    for (const cJSON *b = cJSON_GetObjectItem(args, "bodies")->child; b; b = b->next) inleo += !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(b, "of")), "leo");
    for (const cJSON *g = cJSON_GetObjectItem(args, "signs")->child; g; g = g->next)
        if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(g, "id")), "leo")) ttl = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(g, "ttl"));
    scene_player_t sp = { 0 };
    scene_play(&sp, chart);
    scene_take_start(&sp);
    for (int n = 0; n < 60; n++) scene_tick(&sp, DT);                       /* 2 s of chart */
    const unsigned gen0 = sp.gen;
    scene_t *up = scene_open_build(sp.cur, leo, &err);
    CHECK(up && !strcmp(up->layout, "sign-closeup") && fabsf(up->ttl - ttl) < 1e-3f && ttl >= 5.f, "it builds Leo up close from the chart's params, for its reading (%.0f s) (%s)", ttl, err ? err : "");
    if (up) {
        int glyph = 0, spots = 0, ruler = 0;
        for (int n = 0; n < up->nitems; n++) {
            const scene_item_t *it = &up->items[n];
            glyph += it->k == PK_GLYPH && it->size > 0.6f && it->asset >= 0 && !strcmp(p->assets[it->asset].name, "leo");
            spots += it->k == PK_SPOT;
            ruler += it->k == PK_GLYPH && fabsf(it->y + 0.42f) < 1e-3f;
        }
        CHECK(glyph == 1 && ruler == 1 && inleo == 3 && spots == inleo + 1, "Leo's glyph, its one ruler, its %d bodies of the sky's ten (%d spots)", inleo, spots);
        scene_open(&sp, up, leo);
        CHECK(sp.cur == up && sp.back == chart && sp.gen != gen0 && scene_take_start(&sp) && sp.t == 0.f, "it plays at once, the chart waiting under it (a new scene: the dust reshapes into it)");
        /* Leo's reading (the tapped spot's) plays over it once it is up whole: Molty's own, no agent; taken once. Its
         * clock waits meanwhile (the caller ticks it with 0), so a short layout does not end under its sentence */
        char say[PKG_SAY_MAX + 1], want[PKG_SAY_MAX + 1];
        snprintf(want, sizeof(want), "%.*s", (int)chart->items[leo].nstr, chart->say + chart->items[leo].str);
        for (int q = 0; q < 45; q++) scene_tick(&sp, 0.f);                  /* the change of picture: 1.5 s */
        const int ns = scene_open_say(&sp, say, sizeof(say));
        CHECK(ns > 10 && ns == (int)strlen(want) && !strcmp(say, want) && strstr(say, "Leo"), "then Leo's own reading plays over it (\"%s\")", say);
        CHECK(!scene_open_say(&sp, say, sizeof(say)), "... once");
        /* its words, a minute of them: the layout's clock runs on (its planets light in turn), its end waits */
        for (int q = 0; q < 1800; q++) scene_tick(&sp, fminf(DT, fmaxf(0.f, scene_room(&sp) - 0.1f)));
        CHECK(sp.cur == up && sp.back == chart && sp.t > ttl - 0.2f && sp.t < ttl && scene_room(&sp) < 0.2f, "while the reading plays its clock runs on, its end waits (%.1f of %.0f s)", sp.t, ttl);
        sp.t = 0.f; sp.back_t = 2.f;                                       /* (the checks below: from its start) */
        int n = 0;
        for (; n < 3000 && sp.cur == up; n++) scene_tick(&sp, DT);
        CHECK(sp.cur == chart && !sp.back && fabsf(n * DT - ttl) < 2 * DT && scene_take_start(&sp), "it ends after %.1f s: the chart is back", n * DT);
        CHECK(fabsf(sp.t - (2.f + ttl)) < 0.1f && fabsf(scene_end(chart) - sp.t - (100.f - 2.f - ttl)) < 0.1f, "... its clock ran on: %.1f s of its 100 left", scene_end(chart) - sp.t);
        /* again, and a tap beside its spots goes back at once */
        scene_open(&sp, scene_open_build(sp.cur, leo, &err), leo);
        for (n = 0; n < 30; n++) scene_tick(&sp, DT);
        CHECK(sp.back == chart && scene_spot_at(sp.cur, sp.t, 0.f, -0.8f) < 0, "open again; the bottom of the glass is beside its spots");
        const float t0 = sp.back_t;
        CHECK(scene_back(&sp) && sp.cur == chart && !sp.back && fabsf(sp.t - t0) < 1e-3f && !scene_back(&sp), "a tap there: back at the chart, once");
        CHECK(!scene_open_say(&sp, say, sizeof(say)), "... and the reading it had not played yet is cut");
        /* the chart's ttl ends both */
        scene_open(&sp, scene_open_build(sp.cur, leo, &err), leo);
        sp.back_t = 100.f - 1.f;
        for (n = 0; n < 60 && sp.cur; n++) scene_tick(&sp, DT);
        CHECK(!sp.cur && !sp.back && n * DT < 1.2f, "the chart's ttl runs out under it: both end");
    } else scene_clear(&sp);
    /* a planet opens nothing: it reads */
    scene_t *again = scene_build(p, "chart", args, 100.f, &err);
    CHECK(again && moon >= 0 && !scene_open_build(again, moon, &err) && !err, "a planet's spot opens nothing");
    scene_free(again);
    cJSON_Delete(args);
    CHECK(p->refs == 1, "the scenes gave their references back (%d)", p->refs);
    pkg_unref(p);
}

/* ── fine dust: the chart as the design draws it ── */
typedef struct { int wins; long px; int odd; long comp; } cost_t;
static long composed(const fluid_draw_t *d, int S, int x, int y, int w, int h, const uint16_t *px);
static const fluid_draw_t *s_cost_d;                    /* (set: send_cost counts the composed pixels against its cache) */
static int send_cost(void *ctx, int x, int y, int w, int h, const uint16_t *px) {
    cost_t *c = ctx;
    c->wins++; c->px += (long)w * h;
    c->odd += (y & 1) || (h & 1);
    if (s_cost_d) c->comp += composed(s_cost_d, s_cost_d->fb_w, x, y, w, h, px);
    return 0;
}
/* a frame as a PPM at path (NULL: none), to look at */
static void dump_ppm(const char *path, const uint16_t *fb, int S) {
    FILE *pf = path ? fopen(path, "wb") : NULL;
    if (!pf) return;
    fprintf(pf, "P6 %d %d 255\n", S, S);
    for (int q = 0; q < S * S; q++) {
        const uint16_t le = (uint16_t)(fb[q] >> 8 | fb[q] << 8);
        fputc((le >> 11) * 255 / 31, pf); fputc((le >> 5 & 63) * 255 / 63, pf); fputc((le & 31) * 255 / 31, pf);
    }
    fclose(pf);
}
/* the panel's time for a frame's windows, as measured on the 1.75C (calm chart: 22 windows and 6.3 kpx took 9 ms with the
 * drawing cached): a window's commands 0.23 ms, a pixel drawn and sent 0.55 us. The cells pass before it takes ~11 ms. */
#define CELLS_MS 11.0
#define MORPH_PASS_MS 12.0   /* the drawer's one cells pass for a change's new look (bench1: ~10 ms outside the change's work) */
#define MORPH_FIT_MS  30.0   /* a frame's cells + send that keeps 30 fps (benches 2-5: the interval is cells + send + 2 to 3 ms; p95 <= 33) */
static double panel_ms(const cost_t *c) { return c->wins * 0.23 + c->px * 0.55e-3; }
/* the panel's glass: the windows land there */
typedef struct { uint16_t *px; int S; cost_t c; const fluid_draw_t *d; uint16_t *mirror; } glass_t;   /* mirror: kept as the panel's */
static int send_glass(void *ctx, int x, int y, int w, int h, const uint16_t *px) {
    glass_t *g = ctx;
    for (int r = 0; r < h; r++) memcpy(g->px + (size_t)(y + r) * g->S + x, px + (size_t)r * w, sizeof(uint16_t) * (size_t)w);
    for (int r = 0; g->mirror && r < h; r++) memcpy(g->mirror + (size_t)(y + r) * g->S + x, px + (size_t)r * w, sizeof(uint16_t) * (size_t)w);
    g->c.wins++; g->c.px += (long)w * h; g->c.odd += (y & 1) || (h & 1);
    if (g->d) g->c.comp += composed(g->d, g->S, x, y, w, h, px);
    return 0;
}
/* a window's pixels composed: those that are not the drawing's cache copied as it is (a speck, a dot, a letter) */
static long composed(const fluid_draw_t *d, int S, int x, int y, int w, int h, const uint16_t *px) {
    long n = 0;
    for (int r = 0; r < h; r++) for (int q = 0; q < w; q++) {
        const int X = x + q, Y = y + r;
        int i = X * d->W / S, j = Y * d->H / S;
        while (i < d->W - 1 && d->cx[i + 1] <= X) i++;
        while (i > 0 && d->cx[i] > X) i--;
        while (j < d->H - 1 && d->cy[j + 1] <= Y) j++;
        while (j > 0 && d->cy[j] > Y) j--;
        n += !d->hdc || !d->hdcv[j * d->W + i] || px[r * w + q] != d->hdc[(size_t)Y * d->fb_w + X];
    }
    return n;
}
/* the panel on the device's measured constants: a window handed over 0.15 ms, a pixel copied (the drawing's cache) 0.05 us,
 * a pixel composed (a speck, a dot, a letter over it) 0.9 us */
static bool s_glass_model;                               /* (the ride's rigs: comp is the px of changed cells drawn into the kept glass) */
static double model_ms(const cost_t *c) {
    /* over a kept glass (a scene on fine dust, the device's way: fluid_draw_send2): a window 0.17 ms, a pixel sent 0.06 us
     * (copied from the glass, or straight from it), a pixel of a changed cell drawn into it 0.2 us (an estimate until the
     * bench's cmp and push confirm it) */
    if (s_glass_model) return c->wins * 0.17 + c->px * 0.06e-3 + c->comp * 0.2e-3;
    return c->wins * 0.15 + (c->px - c->comp) * 0.05e-3 + c->comp * 0.9e-3;
}
/* how many pixels of a kept glass fb differ from the look drawn whole now: none when every change was sent */
static int glass_off(const fluid_draw_t *d, const fluid_draw_opts_t *o, const uint16_t *fb, int S) {
    static uint16_t *ref;
    if (!ref) ref = calloc((size_t)S * S, 2);
    fluid_draw_rect(d, o, &(fluid_rect_t){ ref, S, 0, 0, S, S }, true);
    int n = 0;
    for (int q = 0; q < S * S; q++) {
        if (ref[q] != fb[q] && getenv("OFF_TRACE") && n < 6) { const int i = (q % S) * d->W / S, j = (q / S) * d->H / S, k = j * d->W + i;
            printf("    off at %d,%d cell %d,%d: glass %04x look %04x key %04x dirty %d sig %08x sig0 %08x vis %d\n", q % S, q / S, i, j, fb[q], ref[q], d->key[k], d->dirty[k], o->fine ? o->fine->sig[k] : 0, o->fine ? o->fine->sig0[k] : 0, o->fine ? o->fine->vis[k] : 0); }
        n += ref[q] != fb[q];
    }
    return n;
}
static void outline_bump(void *ctx, float x, float y) { fluid_sim_impulse(ctx, x, -y, SCENE_OUTLINE_WAVE, SCENE_OUTLINE_R); }

static void test_fine(const char *dir) {
    fluid_fine_dust_kind(FINE_DUST_SPECKS);              /* (the fine specks: the Lab's other dust) */
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
    s_cost_d = d;                                       /* (the panel's time on the measured constants: model_ms) */
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
    fluid_fine_want(fine, 1, key, true, full, nf, FLUID_PAL_PASTEL);
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
        CHECK(on == 0 && d0 > 0.25f && d0 > d1 && d1 > d2 && d2 > 0.02f && d2 < 0.1f && lum_near / n_near > lum_far / n_far,
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
        /* half the dust along the lines it had (Juan): within 6 px of the drawing, against the count at FINE_PEAK 0.85
         * (NEAR_085 on this chart); the far dust (past 25 px) as it was (FAR_085) */
        enum { NEAR_085 = 21712, FAR_085 = 1123 };
        int nnear = 0, nfar = 0;
        for (int n = 0; n < st->n; n++) {
            const int x = st->pos[n] & 511, y = st->pos[n] >> 9 & 511, dl = fine->dist[(y >> 1) * fine->S2 + (x >> 1)];
            nnear += dl < 600; nfar += dl > 2500;
        }
        printf("  the dust along the lines: %d specks within 6 px (%d at 0.85: %.0f%%), %d past 25 px (%d)\n", nnear, NEAR_085, 100.0 * nnear / NEAR_085, nfar, FAR_085);
        CHECK(nnear > 0.4 * NEAR_085 && nnear < 0.6 * NEAR_085 && abs(nfar - FAR_085) < FAR_085 / 50,
              "half the dust along the lines (%d specks within 6 px of the drawing, %d before), the far dust as before (%d, %d)", nnear, NEAR_085, nfar, FAR_085);
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
        cost_t c = { 0, 0, 0, 0 };
        const double tc = now_us();
        fluid_draw_cells(d, fr, &o);
        cells_us += now_us() - tc;
        fluid_draw_send(d, &o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, false, wbuf, 4096, send_cost, &c);
        fluid_draw_rect(d, &o, &(fluid_rect_t){ fb, S, 0, 0, S, S }, false);
        const double ms = model_ms(&c);
        calm_px += c.px; calm_w += c.wins;
        sum += ms; worst = ms > worst ? ms : worst; changed += c.px > 0;
    }
    {   /* the light, as the design's */
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
    {   /* the drawing's cache gives the very pixels a live draw does */
        uint16_t *a = calloc((size_t)S * S, 2), *b = calloc((size_t)S * S, 2);
        fluid_draw_rect(d, &o, &(fluid_rect_t){ a, S, 0, 0, S, S }, true);
        uint16_t *keep_c = d->hdc; uint8_t *keep_v = d->hdcv;
        d->hdc = NULL; d->hdcv = NULL;
        fluid_draw_opts_t o2 = o; o2.fine = NULL;               /* no cache made without the dust: a live draw ... */
        fluid_draw_rect(d, &o2, &(fluid_rect_t){ b, S, 0, 0, S, S }, true);
        fluid_fine_rect(fine, d->dirty, &(fluid_rect_t){ b, S, 0, 0, S, S }, true);   /* ... with the same dust over it */
        d->hdc = keep_c; d->hdcv = keep_v;
        int diff = 0;
        for (int q = 0; q < S * S; q++) diff += a[q] != b[q];
        CHECK(d->hdc && diff == 0, "the cached drawing is the live one (%d px differ)", diff);
        free(a); free(b);
    }
    printf("  fine dust, calm (panel model): %.1f ms a frame, worst %.1f; %.0f kpx and %.1f windows a frame; the dust's frame (cells) %.0f us on the host\n",
           sum / 60, worst, calm_px / 60e3, calm_w / 60.0, cells_us / 60);
    dump_ppm(getenv("FINE_DUMP"), fb, S);       /* the calm frame, to look at against the page */
    CHECK(changed >= 55 && sum / 60 < 12.0 && worst + CELLS_MS < 33.0, "calm: the light flows every frame (%d of 60), %.1f ms a frame on the panel, worst %.1f", changed, sum / 60, worst);

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
    int tap_off = 0, tap_late = 0, tap_on = 0;
    glass_t gl = { calloc((size_t)S * S, 2), S, { 0, 0, 0, 0 }, d, NULL };
    fluid_draw_rect(d, &o, &(fluid_rect_t){ gl.px, S, 0, 0, S, S }, true);
    uint16_t *held = calloc((size_t)S * S, 2);
    int moved_items = 0, still_cells = 0, broke_still = 0;
    for (int f = 0; f < 60; f++) {
        scene_grow_tick(&g, true, DT);
        scene_grow_apply(&g, sc);
        scene_fill(s, sc, 1.f, NULL, L, &nl, dots, &hd);
        o.nshapes = hd.n;
        const int nn = scene_full_shapes(s, sc, 1.f, full, &key);
        fluid_fine_want(fine, 1, key, false, full, nn, FLUID_PAL_PASTEL);
        if (fluid_fine_work(fine, 0)) fluid_fine_swap(fine);
        fluid_sim_step(s, DT);
        fluid_sim_snapshot(s, fr, false);
        o.time = 3.f + f / 30.f;
        cost_t c = { 0, 0, 0, 0 };
        fluid_draw_cells(d, fr, &o);
        fluid_draw_rect(d, &o, &(fluid_rect_t){ fb, S, 0, 0, S, S }, false);
        gl.c = c;
        fluid_draw_send(d, &o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, false, wbuf, 4096, send_glass, &gl);
        c = gl.c;
        const double ms = model_ms(&c);
        tap_off += glass_off(d, &o, gl.px, S) != 0; tap_late |= d->anim_late; tap_on |= d->anim_on;
        tap_sum += ms; tap_worst = ms > tap_worst ? ms : tap_worst;
        if (getenv("FINE_TRACE")) printf("    tap f%d: %.1f ms (%d windows, %ld px)\n", f, ms, c.wins, c.px);
        if (f == 6) dump_ppm(getenv("FINE_DUMP_TAP"), fb, S);
        if (f == 0) CHECK(ms + CELLS_MS < 33.0, "tap: its rings come into the middle of the shapes, those after them stay as drawn (%ld px sent, %.1f ms)", c.px, ms);
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
    CHECK(tap_off == 0 && tap_late == 0 && (tap_on & (1 << FLUID_ANIM_GROW)) && (tap_on & (1 << FLUID_ANIM_WATER)),
          "tap: the growth, its colour and the water the dust rides are sent every frame they change (%d frames the glass was behind the look; late %x, on %x)", tap_off, tap_late, tap_on);
    CHECK(tap_worst + CELLS_MS < 33.0, "tap: under a frame on the panel (%.1f ms mean, worst %.1f, and %.0f for the cells)", tap_sum / 60, tap_worst, CELLS_MS);

    /* the reading in the middle: each word forms from the dust on a dark backdrop, the last breaks up into the water */
    const char *words[] = { "Mars", "opposes", "Saturn:", "drive", "meets", "limits." };
    double rd_sum = 0., rd_worst = 0.;
    int frames = 0, dark_bad = 0, half_ok = 0, broke = 0, rd_off = 0, rd_late = 0, rd_on = 0;
    float tw = 0.f;
    {   /* the backdrop: one box for the whole reading, as the device gives it (the union of its words' boxes) */
        float ub[4] = { 0 };
        for (int w = 0; w < 6; w++) {
            float b[4];
            const scene_caption_t c = { words[w], (int)strlen(words[w]), 1 };
            scene_caption_box(&c, sc->caption_y, S, b);
            if (!w) memcpy(ub, b, sizeof(b));
            ub[0] = fminf(ub[0], b[0]); ub[1] = fmaxf(ub[1], b[1]); ub[2] = fminf(ub[2], b[2]); ub[3] = fmaxf(ub[3], b[3]);
        }
        fluid_fine_caption_box(fine, ub);
    }
    uint32_t gen_first = 0;
    int gens = 0;
    for (int w = 0; w <= 6; w++) {
        const scene_caption_t cap = w < 6 ? (scene_caption_t){ words[w], (int)strlen(words[w]), 1 } : (scene_caption_t){ NULL, 0, -1 };
        for (int f = 0; f < 9; f++, frames++) {
            scene_fill(s, sc, 1.f, &cap, L, &nl, dots, &hd);
            o.nshapes = hd.n; o.letters = L; o.nletters = nl;
            if (!f && cap.n) for (int q = 0; q < nl; q++) if (!L[q].mask && !L[q].rgb && L[q].ch != ' ')
                fluid_sim_impulse(s, (L[q].x + L[q].w * 0.5f) / S * 2.f - 1.f, (L[q].y + L[q].h * 0.5f) / S * 2.f - 1.f, SCENE_CAPTION_RIPPLE, SCENE_CAPTION_RIPPLE_R);
            o.cap_word = cap.n ? (unsigned)(w + 1) : 0;
            o.cap_unformed = 0.f;                               /* (FINE_REVEAL_S 0: a word is whole on its first frame) */
            fluid_sim_step(s, DT);
            fluid_sim_snapshot(s, fr, false);
            o.time = 5.f + (tw += DT);
            cost_t c = { 0, 0, 0, 0 };
            fluid_draw_cells(d, fr, &o);
            fluid_draw_rect(d, &o, &(fluid_rect_t){ fb, S, 0, 0, S, S }, false);
            gl.c = c;
            fluid_draw_send(d, &o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, false, wbuf, 4096, send_glass, &gl);
            c = gl.c;
            const double ms = model_ms(&c);
            rd_off += glass_off(d, &o, gl.px, S) != 0; rd_late |= d->anim_late; rd_on |= d->anim_on;
            rd_sum += ms; rd_worst = ms > rd_worst ? ms : rd_worst;
            if (getenv("FINE_TRACE")) printf("    read w%d f%d: %.1f ms (%d windows, %ld px)\n", w, f, ms, c.wins, c.px);
            if (cap.n && f == 0 && fine->cbox[1] > fine->cbox[0]) {   /* whole on its first frame, sent in that frame */
                int lit_px = 0, sent_cells = 0;
                for (int y = (int)fine->cbox[2]; y < (int)fine->cbox[3]; y++) for (int x = (int)fine->cbox[0]; x < (int)fine->cbox[1]; x++) lit_px += lum(fb[y * S + x]) > 600;
                for (int k = 0; k < N; k++) sent_cells += d->dirty[k] && fine->imm[k];
                half_ok += lit_px > 40 * cap.n && sent_cells > 0;
            }
            if (cap.n && f == 8) {                        /* whole: no dust on its backdrop */
                const fine_set_t *st = fine->drawn;
                for (int n = 0; st && n < st->n; n++) {
                    const int x = st->pos[n] & 511, y = st->pos[n] >> 9 & 511;
                    if (x > fine->cbox[0] && x < fine->cbox[1] && y > fine->cbox[2] && y < fine->cbox[3] && fine->lv[n] > 0.17f * 255) { if (getenv("FINE_TRACE") && dark_bad < 5) printf("    dark: %d,%d lv %d box %.1f %.1f %.1f %.1f\n", x, y, fine->lv[n], fine->cbox[0], fine->cbox[1], fine->cbox[2], fine->cbox[3]); dark_bad++; }
                }
            }
            if (w == 6 && f == 4) broke = fine->npd;
            if (w == 2 && f == 8) dump_ppm(getenv("FINE_DUMP_READ"), fb, S);
            if (w == 6 && f == 5) dump_ppm(getenv("FINE_DUMP_END"), fb, S);
            if (cap.n) { if (!gen_first) gen_first = fine->gen; gens += fine->gen != gen_first; }
        }
    }
    printf("  fine dust, reading to its end (panel model): %.1f ms a frame, worst %.1f\n", rd_sum / frames, rd_worst);
    CHECK(dark_bad == 0 && half_ok == 6 && broke > 50, "reading: each word on dark (%d specks on it), whole on its first frame (%d of 6), the last breaking into %d specks", dark_bad, half_ok, broke);
    CHECK(gen_first > 1 && gens == 0, "reading: the dark backdrop holds still from word to word (%d frames of another box)", gens);
    CHECK(rd_off == 0 && rd_late == 0 && (rd_on & (1 << FLUID_ANIM_WORD)), "reading: each word, its backdrop and its lifting sent every frame they change (%d frames behind; late %x)", rd_off, rd_late);
    CHECK(rd_worst + CELLS_MS < 33.0, "reading: under a frame on the panel (%.1f ms mean, worst %.1f, and %.0f for the cells)", rd_sum / frames, rd_worst, CELLS_MS);

    /* a listen's ring over the scene: a thin line that draws itself round the rim, a few cells a frame; the chart stays
     * in high definition under it, as it was; held, it costs nothing more; the agent's comet turns a part or so a frame */
    {
        scene_fill(s, sc, 1.f, NULL, L, &nl, dots, &hd);
        fluid_sim_destroy(s); s = fluid_sim_create(W, H, S); s->p.still = true;
        fluid_sim_snapshot(s, fr, false);
        o.nshapes = hd.n; o.letters = L; o.nletters = nl; o.cap_word = 0;
        for (int f = 0; f < 12; f++) { o.time = 20.f + f / 30.f; fluid_draw_cells(d, fr, &o); fluid_draw_rect(d, &o, &(fluid_rect_t){ fb, S, 0, 0, S, S }, false); }
        uint16_t *before = calloc((size_t)S * S, 2);
        fluid_draw_rect(d, &o, &(fluid_rect_t){ before, S, 0, 0, S, S }, true);
        static fluid_shape_t rs[FLUID_MAX_SHAPES];
        fluid_ring_t rg = { 0 };
        fluid_progress_t pg = { 0 };
        double rworst = 0.;
        int nmax = 0, undone = 0;
        for (int f = 0; f < 20; f++) {
            fluid_ring_tick(&rg, DT, true);
            memcpy(rs, shp, sizeof(fluid_shape_t) * (size_t)hd.n);
            const int n = scene_rings(rs, hd.n, S, &rg, &pg, false, 0xFF2C3Eu, 0x35E07Cu);
            o.shapes = rs; o.nshapes = n; o.time = 21.f + f / 30.f;
            cost_t c = { 0, 0, 0, 0 };
            fluid_draw_cells(d, fr, &o);
            fluid_draw_send(d, &o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, false, wbuf, 4096, send_cost, &c);
            fluid_draw_rect(d, &o, &(fluid_rect_t){ fb, S, 0, 0, S, S }, false);
            rworst = fmax(rworst, model_ms(&c));
            nmax = n - hd.n;
            for (int k = 0; k < N; k++) undone += d->key[k] && (d->key[k] & 0xF000) != 0x8000;
        }
        /* the line: red at every degree of the rim; the chart inside it as before */
        uint16_t *with = calloc((size_t)S * S, 2);
        fluid_draw_rect(d, &o, &(fluid_rect_t){ with, S, 0, 0, S, S }, true);
        dump_ppm(getenv("FINE_DUMP_RING"), with, S);
        int red = 0, changed = 0;
        const float R = SCENE_RING_R * S / 2;
        for (int a = 0; a < 360; a++) {
            const uint16_t px = with[(int)(S / 2.f - R * sinf(a * 0.0174533f)) * S + (int)(S / 2.f + R * cosf(a * 0.0174533f))];
            const uint16_t le = (uint16_t)(px >> 8 | px << 8);
            red += (le >> 11) > 20 && (le >> 5 & 63) < 24;
        }
        /* (the dust's light flows meanwhile: compare the drawing itself, the lit pixels of the chart) */
        for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
            if (hypotf(x + 0.5f - S / 2.f, y + 0.5f - S / 2.f) > (SCENE_RING_R - 0.02f) * S / 2) continue;
            changed += lum(before[y * S + x]) > 700 && before[y * S + x] != with[y * S + x];   /* (a bright speck: about 540) */
        }
        printf("  a listen's ring over the chart (panel model): forming, worst %.1f ms a frame\n", rworst);
        CHECK(nmax == SCENE_RING_SEGS && red >= 356 && changed == 0 && undone == 0 && rworst + CELLS_MS < 33.0,
              "the ring draws itself round the rim (%d parts, red at %d of 360 degrees), worst %.1f ms a frame; the chart under it as it was (%d px changed, %d cells out of high definition)",
              nmax, red, rworst, changed, undone);
        /* held: the ring itself sends nothing more (only the dust's light, as calm) */
        double held = 0.;
        for (int f = 0; f < 20; f++) {
            o.time = 22.f + f / 30.f;
            cost_t c = { 0, 0, 0, 0 };
            fluid_draw_cells(d, fr, &o);
            fluid_draw_send(d, &o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, false, wbuf, 4096, send_cost, &c);
            held = fmax(held, model_ms(&c));
        }
        CHECK(held + CELLS_MS < 33.0, "held, it costs as the calm chart (worst %.1f ms a frame)", held);
        /* the voice at its loudest: touches of the water just inside the line; the dust by the rim rides them, the
         * drawing holds, the frame stays inside its budget */
        {
            fluid_fine_rim(fine, true);
            double vworst = 0.;
            int vbroke = 0;
            long vpx = 0;
            for (int f = 0; f < 60; f++) {
                if (f % 3 != 2) { const float a = f * 2.399f; fluid_sim_impulse(s, (SCENE_RING_R - 0.05f) * cosf(a), (SCENE_RING_R - 0.05f) * sinf(a), 0.12f, 0.05f); }
                fluid_sim_step(s, DT);
                fluid_sim_snapshot(s, fr, false);
                o.time = 22.7f + f / 30.f;
                cost_t c = { 0, 0, 0, 0 };
                fluid_draw_cells(d, fr, &o);
                fluid_draw_send(d, &o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, false, wbuf, 4096, send_cost, &c);
                const double ms = model_ms(&c);
                if (getenv("FINE_TRACE")) printf("    voice f%d: %.1f ms (%d windows, %ld px)\n", f, ms, c.wins, c.px);
                vworst = fmax(vworst, ms); vpx += c.px;
                for (int k = 0; k < N; k++) vbroke += d->key[k] && (d->key[k] & 0xF000) != 0x8000;
            }
            CHECK(vworst + CELLS_MS < 33.0 && vbroke == 0 && vpx > 60 * 2000, "the voice on the ring: the dust by the rim stirs (%ld px a frame), worst %.1f ms a frame, the drawing whole", vpx / 60, vworst);
            fluid_fine_rim(fine, false);
            fluid_sim_destroy(s); s = fluid_sim_create(W, H, S); s->p.still = true;
            fluid_sim_snapshot(s, fr, false);
        }
        /* the agent's comet (thinking): about a fifth of the line, turning */
        fluid_ring_t off = { 0 };
        int n0 = 0, moved = 0;
        double cworst = 0.;
        for (int f = 0; f < 30; f++) {
            fluid_progress_tick(&pg, DT, false, -1.f, false);
            memcpy(rs, shp, sizeof(fluid_shape_t) * (size_t)hd.n);
            const int n = scene_rings(rs, hd.n, S, &off, &pg, true, 0xFF2C3Eu, 0x35E07Cu);
            o.shapes = rs; o.nshapes = n; o.time = 23.f + f / 30.f;
            cost_t c = { 0, 0, 0, 0 };
            fluid_draw_cells(d, fr, &o);
            fluid_draw_send(d, &o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, false, wbuf, 4096, send_cost, &c);
            if (f > 2) cworst = fmax(cworst, model_ms(&c));
            if (f == 3) n0 = n - hd.n;
            moved += f > 3 && rs[hd.n].a0 != 0.f && n - hd.n > 0;
        }
        CHECK(n0 >= 9 && n0 <= 12 && moved > 20 && cworst + CELLS_MS < 33.0, "thinking: a comet of %d parts circles, worst %.1f ms a frame", n0, cworst);
        o.shapes = shp; o.nshapes = hd.n;
        free(before); free(with);
    }

    /* sets are found again by their drawing, the scene's calm set is never given up, another scene's dust goes */
    {
        uint32_t kt, kc, k2 = 0x1234567u;
        sc->focus = asp; sc->glow = 1.f;
        const int nt = scene_full_shapes(s, sc, 1.f, full, &kt);
        sc->focus = -1; sc->glow = 0.f;
        static fluid_shape_t calm_sh[FLUID_MAX_SHAPES];
        const int ncalm = scene_full_shapes(s, sc, 1.f, calm_sh, &kc);
        CHECK(kt != kc && fluid_fine_want(fine, 1, kt, false, full, nt, FLUID_PAL_PASTEL) && fine->build < 0, "the tapped drawing's set is known by its shapes: no build");
        if (fluid_fine_work(fine, 0)) fluid_fine_swap(fine);
        CHECK(fine->drawn && fine->drawn->key == kt, "... and swaps in");
        /* another scene comes (its calm drawing: a few of these shapes) */
        CHECK(fluid_fine_want(fine, 2, k2, true, calm_sh, 5, FLUID_PAL_PASTEL) && fine->build >= 0 && !fluid_fine_shown(fine, 2), "a new scene's calm set builds; it is not on the glass yet");
        CHECK(!fluid_fine_want(fine, 2, kt ^ 1u, false, full, nt, FLUID_PAL_PASTEL) && fine->bkey == k2, "... and is not given up for a tapped spot's");
        fluid_fine_swap(fine);
        CHECK(fine->drawn == NULL && fluid_fine_specks(fine) == 0, "the old scene's dust is off the glass at once");
        fluid_draw_cells(d, fr, &o);
        int dusty = 0;
        for (int k = 0; k < N; k++) dusty += fine->vis[k];
        CHECK(dusty == 0, "... no cell shows any (%d)", dusty);
        const bool done = fluid_fine_work(fine, 0);
        fluid_fine_swap(fine);
        CHECK(done && fine->drawn && fine->drawn->key == k2 && fluid_fine_shown(fine, 2), "the new scene's dust is on");
        /* back at the chart: its calm set is still there */
        CHECK(fluid_fine_want(fine, 3, kc, true, calm_sh, ncalm, FLUID_PAL_PASTEL) && fine->build < 0 && fluid_fine_work(fine, 0), "back at the chart: its dust is known, no build");
        fluid_fine_swap(fine);
        CHECK(fine->drawn && fine->drawn->key == kc && fine->drawn->n == nsp, "... and on the glass (%d specks)", fluid_fine_specks(fine));
    }

    /* back to calm: the calm set comes back at once (kept) */
    scene_grow_tick(&g, false, 1.f);
    scene_grow_apply(&g, sc);
    const int nn = scene_full_shapes(s, sc, 1.f, full, &key);
    fluid_fine_want(fine, 1, key, true, full, nn, FLUID_PAL_PASTEL);
    CHECK(fine->build < 0 && fine->drawn && fine->drawn->n == nsp, "untapped: the calm dust is back without a build");
    free(held); free(gl.px);
    free(dots); free(keep); free(still); free(fb); free(only);
    fluid_fine_destroy(fine); fluid_draw_destroy(d); fluid_frame_destroy(fr); fluid_sim_destroy(s);
    scene_free(sc);
    pkg_unref(p);
    fluid_fine_dust_kind(FINE_DUST_SPECKS);                    /* (the default back) */
    s_cost_d = NULL;
}

/* ── EXPERIMENT scene dust (fluid_fine_dust_kind; delete with it once Juan picks): a scene's dust
 * as the creature screen's (braille dots riding the water) against today's fine specks, on the panel model with the
 * device's measured constants (2113895: a window handed over ~0.15 ms, a pixel drawn and sent ~0.9 us; the cells ~10 ms) ── */
#define SD_CELLS_MS 10.0
static double measured_ms(const cost_t *c) { return c->wins * 0.15 + c->px * 0.9e-3; }
typedef struct { double sum, worst; int n, over; long px, pxmax; int wins, winmax; double msum, mworst; int mover; } sd_stat_t;
static void sd_add(sd_stat_t *t, const cost_t *c) {
    const double ms = measured_ms(c), mm = model_ms(c);
    t->msum += mm; t->mworst = mm > t->mworst ? mm : t->mworst; t->mover += mm + SD_CELLS_MS > 33.0;
    t->over += ms + SD_CELLS_MS > 33.0;
    t->sum += ms; t->worst = ms > t->worst ? ms : t->worst; t->n++; t->px += c->px; t->wins += c->wins;
    if (c->px > t->pxmax) t->pxmax = c->px;
    if (c->wins > t->winmax) t->winmax = c->wins;
}
static void sd_print(const char *what, const sd_stat_t *t) {
    printf("    %-34s %5.1f ms mean, %5.1f worst (+%.0f cells: %4.1f / %4.1f, %2d of %d frames over 33); %4.1f kpx mean, %4.1f max; %4.1f windows mean, %d max\n", what,
           t->sum / t->n, t->worst, SD_CELLS_MS, t->sum / t->n + SD_CELLS_MS, t->worst + SD_CELLS_MS, t->over, t->n, t->px / 1e3 / t->n, t->pxmax / 1e3, (double)t->wins / t->n, t->winmax);
}
static scene_t *sd_scene(pkg_t *p, const char *dir, const char *want) {
    char path[512]; size_t len; const char *err;
    snprintf(path, sizeof(path), "%s/astrology-commands.jsonl", dir);
    char *cmds = read_file(path, &len);
    scene_t *sc = NULL;
    for (char *line = cmds ? strtok(cmds, "\n") : NULL; line && !sc; line = strtok(NULL, "\n")) {
        cJSON *c = cJSON_Parse(line), *a = cJSON_GetObjectItem(c, "args");
        const char *layout = cJSON_GetStringValue(cJSON_GetObjectItem(a, "layout"));
        if (layout && !strcmp(layout, want)) sc = scene_build(p, layout, cJSON_GetObjectItem(a, "params"), 0.f, &err);
        cJSON_Delete(c);
    }
    free(cmds);
    return sc;
}
/* one run: the scene calm (8 frames), then a tap at (x, y) (glass units, y up) on spot `spot` (-1: empty space) as the
 * device makes it (kind 1: the finger's ripple always; kind 0: a spot's outline), 60 frames of its wave and its drawing's
 * highlight. With kind 1 the creature screen's drawer is run on the same water with the same dots (crisp, every cell
 * looked at): the dust must be the very dots it shows. */
typedef struct { int mismatch, off, front0, front1, fmax, fmax_at, frames_front; sd_stat_t calm, tap; uint16_t *calm_fb; } sd_run_t;
static void sd_run(scene_t *sc, int kind, int spot, float x, float y, sd_run_t *r, const char *dump, const char *dump3) {
    const int S = 466; int W, H;
    fluid_grid_for_stage(S, &W, &H);
    const int N = W * H;
    fluid_fine_dust_kind(kind);
    fluid_sim_t *s = fluid_sim_create(W, H, S); s->p.still = true;
    fluid_frame_t *fr = fluid_frame_create(W, H);
    fluid_draw_t *d = fluid_draw_create(W, H, S, S), *dc = fluid_draw_create(W, H, S, S);
    fluid_fine_t *fine = fluid_fine_create(W, H, S, S);
    uint8_t *dots = calloc((size_t)N, 1), *keep = calloc((size_t)N, 1), *still = calloc((size_t)N, 1), *cdots = calloc((size_t)N, 1);
    static fluid_shape_t shp[FLUID_MAX_SHAPES], full[FLUID_MAX_SHAPES];
    static uint16_t wbuf[466 * 18];
    fluid_letter_t L[FLUID_MAX_LETTERS]; int nl;
    scene_shapes_t hd = { shp, 0, keep, still };
    glass_t gl = { calloc((size_t)S * S, 2), S, { 0, 0, 0, 0 }, NULL, NULL };
    memset(r, 0, sizeof(*r));
    r->calm_fb = calloc((size_t)S * S, 2);
    fluid_draw_opts_t o = { .palette = FLUID_PAL_PASTEL, .edge_mode = FLUID_EDGE_CONTOURS, .letters = L, .font = gc_font5x7,
                            .dots = dots, .scene = true, .shapes = shp, .kdots = keep, .fine = fine, .still = still };
    fluid_draw_opts_t oc = { .palette = FLUID_PAL_PASTEL, .edge_mode = FLUID_EDGE_CONTOURS, .font = gc_font5x7, .dots = cdots, .scene = true };
    scene_grow_t g;
    if (spot >= 0) scene_grow_tap(&g, spot);
    const float tx = (x + 1.f) * S / 2, ty = (1.f - y) * S / 2;
    for (int f = -8; f < 60; f++) {
        if (f >= 0 && spot >= 0) { scene_grow_tick(&g, true, DT); scene_grow_apply(&g, sc); }
        scene_fill(s, sc, 1.f, NULL, L, &nl, dots, &hd);
        o.nshapes = hd.n; o.nletters = nl;
        if (fluid_fine_specks_kind(kind)) {
            uint32_t key;
            const int nn = scene_full_shapes(s, sc, 1.f, full, &key);
            fluid_fine_want(fine, 1, key, f < 0, full, nn, FLUID_PAL_PASTEL);
            if (fluid_fine_work(fine, 0)) fluid_fine_swap(fine);
        }
        if (f == 0) {
            if (spot >= 0 && kind == FINE_DUST_SPECKS) scene_outline(sc, spot, outline_bump, s);   /* (kind 3: the finger's ripple, as the device) */
            else fluid_sim_impulse(s, x, -y, 1.6f, 0.09f);
        }
        for (int n = 0; f >= 0 && n < 2; n++) fluid_sim_step(s, DT / 2);   /* (as the device: two 1/60 s steps a frame at 30 fps) */
        fluid_sim_snapshot(s, fr, false);
        o.time = 1.f + (f + 8) / 30.f;
        fluid_draw_cells(d, fr, &o);
        cost_t c = { 0, 0, 0, 0 };
        gl.c = c;
        fluid_draw_send(d, &o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, f == -8, wbuf, 4096, send_glass, &gl);
        if (f > -8) sd_add(f < 0 ? &r->calm : &r->tap, &gl.c);
        if (getenv("SD_TRACE") && (measured_ms(&gl.c) > 20.0 || getenv("SD_ALL"))) {
            int nd = 0, nw = 0, nh = 0, nhw = 0;
            for (int k = 0; k < N; k++) if (d->dirty[k]) { nd++; nw += (d->why[k] >> FLUID_ANIM_WATER) & 1; nh += d->hch[k]; nhw += d->hch[k] && ((d->why[k] >> FLUID_ANIM_WATER) & 1); }
            printf("    kind %d f%d: %.1f ms, %d windows %ld px; %d cells dirty, %d water, %d hch (%d both), m %d; w %d o %d c %d a %.2f\n", kind, f, measured_ms(&gl.c), gl.c.wins, gl.c.px, nd, nw, nh, nhw, d->sd_m, d->sd_w, d->sd_o, d->sd_c, d->sd_a);
        }
        { const int go = glass_off(d, &o, gl.px, S); r->off += go != 0; if (go && getenv("SD_TRACE")) printf("    behind: kind %d spot %d f%d, %d px\n", kind, spot, f, go); }
        if (f == -1) memcpy(r->calm_fb, gl.px, (size_t)S * S * 2);
        if (dump && f == 10) dump_ppm(dump, gl.px, S);
        if (dump3 && f == 10) { char pth[600]; snprintf(pth, sizeof(pth), "%s-wave.ppm", dump3); dump_ppm(pth, gl.px, S); }
        if (dump3 && f == -1) { char pth[600]; snprintf(pth, sizeof(pth), "%s-calm.ppm", dump3); dump_ppm(pth, gl.px, S); }
        if ((kind != FINE_DUST_DOTS && kind != FINE_DUST_FINE_DOTS) || f < 0) continue;
        memcpy(cdots, d->sd_live, (size_t)N);           /* the creature screen, the same dots on the same water */
        dc->force = true;
        fluid_draw_cells(dc, fr, &oc);
        int far = 0;
        const bool k3 = kind == FINE_DUST_FINE_DOTS;
        for (int k = 0; k < N; k++) {
            if (!fr->mask[k] || keep[k] || (d->key[k] & 0xF000u) != 0x8000u) continue;   /* (the drawing's own cells broken by the water: its dots) */
            if (k3) {                                   /* (kind 3: the hard dots, where the cell shows none of the drawing's) */
                if ((d->key[k] & 0xFF) & ~d->sd_hard[k]) continue;
                r->mismatch += d->sd_hard[k] != (dc->key[k] & 0xFF);
                if (d->sd_hard[k] == d->sd_live[k]) continue;
                const int i = k % W, j = k / W;
                const int dd = (int)hypotf((d->cx[i] + d->cx[i + 1]) * 0.5f - tx, (d->cy[j] + d->cy[j + 1]) * 0.5f - ty);
                if (dd > far) far = dd;
                continue;
            }
            r->mismatch += (d->key[k] & 0xFF) != (dc->key[k] & 0xFF);
            if ((d->key[k] & 0xFF) != (dc->key[k] & 0xFF) && getenv("SD_TRACE") && r->mismatch < 8) printf("    f%d cell %d,%d scene %04x cat %04x lattice %02x dots %02x dirty %d fx %.2f fy %.2f catlat %02x\n", f, k % W, k / W, d->key[k], dc->key[k], d->sd_dots[k], dots[k], d->dirty[k], (fr->h[k + 1] - fr->h[k - 1]) * 3.3f / 1024 * 2, (fr->h[k + W] - fr->h[k - W]) * 3.3f / 1024 * 4 * H / W, cdots[k]);
            if ((d->key[k] & 0xFF) == d->sd_live[k]) continue;   /* moved by the wave: how far from the finger */
            const int i = k % W, j = k / W;
            const int dd = (int)hypotf((d->cx[i] + d->cx[i + 1]) * 0.5f - tx, (d->cy[j] + d->cy[j + 1]) * 0.5f - ty);
            if (dd > far) far = dd;
        }
        if (f == 3) r->front0 = far;
        if (f == 30) r->front1 = far;
        if (far > r->fmax) { r->fmax = far; r->fmax_at = f; }
        r->frames_front += far > 0;
    }
    fluid_fine_dust_kind(FINE_DUST_SPECKS);   /* (the default back) */
    free(gl.px); free(dots); free(keep); free(still); free(cdots);
    fluid_fine_destroy(fine); fluid_draw_destroy(d); fluid_draw_destroy(dc); fluid_frame_destroy(fr); fluid_sim_destroy(s);
}

/* the drawing rides the water ("lines wobble with water"): a tap's wave breaks each part of the drawing it moves into its
 * dots as the front reaches it, every such cell sent every frame it moves, whole again the first frame the water there
 * is calm, and once the wave has passed the glass is the still drawing pixel for pixel. The fine specks left out (no
 * set): the drawing alone. */
static void test_wobble(const char *dir, scene_t *sc, const char *name, float x, float y) {
    (void)dir;
    const int S = 466; int W, H;
    fluid_grid_for_stage(S, &W, &H);
    const int N = W * H;
    fluid_fine_dust_kind(FINE_DUST_SPECKS);
    fluid_sim_t *s = fluid_sim_create(W, H, S); s->p.still = true;
    fluid_frame_t *fr = fluid_frame_create(W, H);
    fluid_draw_t *d = fluid_draw_create(W, H, S, S);
    fluid_fine_t *fine = fluid_fine_create(W, H, S, S);
    uint8_t *dots = calloc((size_t)N, 1), *keep = calloc((size_t)N, 1), *still = calloc((size_t)N, 1);
    static fluid_shape_t shp[FLUID_MAX_SHAPES];
    static uint16_t wbuf[466 * 18];
    fluid_letter_t L[FLUID_MAX_LETTERS]; int nl;
    scene_shapes_t hd = { shp, 0, keep, still };
    glass_t gl = { calloc((size_t)S * S, 2), S, { 0, 0, 0, 0 }, NULL, NULL };
    uint16_t *calm = calloc((size_t)S * S, 2);
    fluid_draw_opts_t o = { .palette = FLUID_PAL_PASTEL, .edge_mode = FLUID_EDGE_CONTOURS, .letters = L, .font = gc_font5x7,
                            .dots = dots, .scene = true, .shapes = shp, .kdots = keep, .fine = fine, .still = still };
    const float tx = (x + 1.f) * S / 2, ty = (1.f - y) * S / 2, rk = 3.3f / FQ_H;
    int off = 0, late = 0, frames_broken = 0, far = 0, broken_max = 0, last_broken = -1;
    sd_stat_t st = { 0 };
    const int F = 150;
    for (int f = -4; f < F; f++) {
        scene_fill(s, sc, 1.f, NULL, L, &nl, dots, &hd);
        o.nshapes = hd.n; o.nletters = nl;
        if (f == 0) fluid_sim_impulse(s, x, -y, 1.6f, 0.09f);
        for (int n = 0; f >= 0 && n < 2; n++) fluid_sim_step(s, DT / 2);
        fluid_sim_snapshot(s, fr, false);
        o.time = 1.f + (f + 4) / 30.f;
        fluid_draw_cells(d, fr, &o);
        gl.c = (cost_t){ 0, 0, 0, 0 };
        fluid_draw_send(d, &o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, f == -4, wbuf, 4096, send_glass, &gl);
        if (f >= 0) sd_add(&st, &gl.c);
        off += glass_off(d, &o, gl.px, S) != 0;
        if (f == -1) memcpy(calm, gl.px, (size_t)S * S * 2);
        if (f == 12 && getenv("WOB_DUMP")) dump_ppm(getenv("WOB_DUMP"), gl.px, S);
        int nb = 0;
        for (int j = 1; j < H - 1; j++) for (int i = 1; i < W - 1; i++) {
            const int k = j * W + i;
            if (!fr->mask[k] || !dots[k]) continue;
            const float fx = (fr->h[k + 1] - fr->h[k - 1]) * rk * 2.f, fy = (fr->h[k + W] - fr->h[k - W]) * rk * 4.f * H / W;
            const bool broken = (d->key[k] & 0xF000u) != 0x8000u;
            if (!broken) { if (fx * fx + fy * fy > (FINE_BREAK + 0.3f) * (FINE_BREAK + 0.3f) && !still[k]) late++; continue; }   /* (must break where the water is strong) */
            if (fx * fx + fy * fy < (FINE_BREAK - 0.1f) * (FINE_BREAK - 0.1f)) late++;   /* calm there: whole again this very frame */
            nb++;
            const int dd = (int)hypotf((d->cx[i] + d->cx[i + 1]) * 0.5f - tx, (d->cy[j] + d->cy[j + 1]) * 0.5f - ty);
            if (dd > far) far = dd;
        }
        if (nb) { frames_broken++; last_broken = f; }
        if (nb > broken_max) broken_max = nb;
        if (getenv("WOB_TRACE")) printf("    wobble %s f%d: %d cells broken, %.1f ms (%d windows, %ld px)\n", name, f, nb, measured_ms(&gl.c), gl.c.wins, gl.c.px);
    }
    int diff = 0;
    for (int q = 0; q < S * S; q++) diff += gl.px[q] != calm[q];
    printf("    the drawing wobbles (%s): %d frames, at most %d cells, reaching %d px from the finger, whole from frame %d; ", name, frames_broken, broken_max, far, last_broken + 1);
    sd_print("drawing alone, the wave", &st);
    CHECK(frames_broken >= 10 && far >= 150 && off == 0 && late == 0,
          "the drawing rides the water (%s): broken where the wave moves it, out to %d px, %d frames, every frame sent (%d behind) and whole where calm (%d cell frames late)",
          name, far, frames_broken, off, late);
    CHECK(diff == 0 && last_broken < F - 1, "the drawing rides the water (%s): once it has passed, the glass is the still drawing pixel for pixel (%d px differ)", name, diff);
    free(gl.px); free(calm); free(dots); free(keep); free(still);
    fluid_fine_destroy(fine); fluid_draw_destroy(d); fluid_frame_destroy(fr); fluid_sim_destroy(s);
}

/* a reading of three words over the chart and its end, as the device drives it (each word's ripple, one backdrop for
 * the reading), on dust kind `kind`: its cost, the dust dots on the backdrop once it is up, the frames the glass was
 * behind the look, an earlier word's bright pixels left round the word (none may be: the water never breaks the
 * caption or its backdrop, and what it broke round them is whole the first calm frame) */
typedef struct { sd_stat_t st; int on_box, off, late, left; } sd_read_t;
static void sd_read(scene_t *chart, int kind, sd_read_t *r, const char *dump) {
    const int S = 466; int W, H;
    fluid_grid_for_stage(S, &W, &H);
    const int N = W * H;
    memset(r, 0, sizeof(*r));
    fluid_fine_dust_kind(kind);
    fluid_sim_t *s = fluid_sim_create(W, H, S); s->p.still = true;
    fluid_frame_t *fr = fluid_frame_create(W, H);
    fluid_draw_t *d = fluid_draw_create(W, H, S, S);
    fluid_fine_t *fine = fluid_fine_create(W, H, S, S);
    uint8_t *dots = calloc((size_t)N, 1), *keep = calloc((size_t)N, 1), *still = calloc((size_t)N, 1);
    static fluid_shape_t shp[FLUID_MAX_SHAPES], full[FLUID_MAX_SHAPES];
    static uint16_t wbuf[466 * 18];
    fluid_letter_t L[FLUID_MAX_LETTERS]; int nl;
    scene_shapes_t hd = { shp, 0, keep, still };
    scene_fill(s, chart, 1.f, NULL, L, &nl, dots, &hd);
    if (fluid_fine_specks_kind(kind)) {                 /* (the specks' set, built ahead) */
        uint32_t key;
        const int nn = scene_full_shapes(s, chart, 1.f, full, &key);
        fluid_fine_want(fine, 1, key, true, full, nn, FLUID_PAL_PASTEL);
        if (fluid_fine_work(fine, 0)) fluid_fine_swap(fine);
    }
    uint16_t *plain = calloc((size_t)S * S, 2);        /* the scene with no word, its drawing whole */
    { static int16_t pb[FLUID_MAX_SHAPES][4];
      for (int q = 0; q < hd.n; q++) fluid_shape_box(&shp[q], 0, 0, pb[q]);
      for (int y = 0; y < S; y++) fluid_shapes_row(shp, hd.n, (const int16_t (*)[4])pb, 0, 0, FLUID_PAL_PASTEL, NULL, 0.f, plain + (size_t)y * S, 0, y, 0, S); }
    fluid_sim_snapshot(s, fr, false);
    fluid_draw_opts_t o = { .palette = FLUID_PAL_PASTEL, .edge_mode = FLUID_EDGE_CONTOURS, .letters = L, .nletters = nl, .font = gc_font5x7,
                            .dots = dots, .scene = true, .shapes = shp, .nshapes = hd.n, .kdots = keep, .fine = fine, .still = still, .time = 1.f };
    fluid_draw_cells(d, fr, &o);
    float ub[4];
    const char *words[] = { "Mars", "opposes", "Saturn." };
    for (int w = 0; w < 3; w++) {
        float b[4];
        const scene_caption_t c = { words[w], (int)strlen(words[w]), 1 };
        scene_caption_box(&c, chart->caption_y, S, b);
        if (!w) memcpy(ub, b, sizeof(b));
        ub[0] = fminf(ub[0], b[0]); ub[1] = fmaxf(ub[1], b[1]); ub[2] = fminf(ub[2], b[2]); ub[3] = fmaxf(ub[3], b[3]);
    }
    fluid_fine_caption_box(fine, ub);
    glass_t gl = { calloc((size_t)S * S, 2), S, { 0, 0, 0, 0 }, d, NULL };
    fluid_draw_send(d, &o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, true, wbuf, 4096, send_glass, &gl);
    int frames = 0;
    for (int w = 0; w <= 3; w++) {
        const scene_caption_t cap = w < 3 ? (scene_caption_t){ words[w], (int)strlen(words[w]), 1 } : (scene_caption_t){ NULL, 0, -1 };
        for (int f = 0; f < 12; f++, frames++) {
            scene_fill(s, chart, 1.f, &cap, L, &nl, dots, &hd);
            o.nshapes = hd.n; o.nletters = nl;
            if (!f && cap.n) for (int q = 0; q < nl; q++) if (!L[q].mask && !L[q].rgb && L[q].ch != ' ')
                fluid_sim_impulse(s, (L[q].x + L[q].w * 0.5f) / S * 2.f - 1.f, (L[q].y + L[q].h * 0.5f) / S * 2.f - 1.f, SCENE_CAPTION_RIPPLE, SCENE_CAPTION_RIPPLE_R);
            o.cap_word = cap.n ? (unsigned)(w + 1) : 0; o.cap_unformed = 0.f;
            fluid_sim_step(s, DT / 2); fluid_sim_step(s, DT / 2);
            fluid_sim_snapshot(s, fr, false);
            o.time = 1.f + (frames + 1) / 30.f;
            fluid_draw_cells(d, fr, &o);
            gl.c = (cost_t){ 0, 0, 0, 0 };
            fluid_draw_send(d, &o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, false, wbuf, 4096, send_glass, &gl);
            sd_add(&r->st, &gl.c);
            if (getenv("SD_TRACE")) { int nd = 0; for (int k = 0; k < N; k++) nd += d->dirty[k] != 0; printf("    read k%d w%d f%d: %d windows %ld px; %d dirty (%.1f ms)\n", kind, w, f, gl.c.wins, gl.c.px, nd, measured_ms(&gl.c)); }
            r->off += glass_off(d, &o, gl.px, S) != 0; r->late |= d->anim_late;
            if (cap.n && w == 2 && f == 11) {
                dump_ppm(dump, gl.px, S);
                for (int k = 0; k < N; k++) {
                    const int i = k % W, j = k / W, dts = d->key[k] & 0xFF;
                    for (int qq = 0; dts && !keep[k] && qq < 8; qq++) {
                        if (!(dts >> fluid_braille_bit(qq >> 2, qq & 3) & 1)) continue;
                        const float x = d->dx[2 * i + (qq >> 2)] + 1.f, y = d->dy[4 * j + (qq & 3)] + 1.f;
                        r->on_box += x > ub[0] && x < ub[1] && y > ub[2] && y < ub[3] && !(dots[k]);
                    }
                }
            }
            if (f != 11) continue;
            for (int y = (int)ub[2] - 14; y < (int)ub[3] + 14; y++) for (int x = (int)ub[0] - 14; x < (int)ub[1] + 14; x++) {
                if (x < 0 || y < 0 || x >= S || y >= S) continue;
                bool in_word = false;
                for (int q = 0; q < hd.n && !in_word; q++) {
                    const fluid_shape_t *sh = &shp[q];
                    if (sh->tag != FLUID_TAG_CAPTION) continue;
                    in_word = fabsf(x + 0.5f - sh->x) <= sh->w * 0.5f + 1.5f && fabsf(y + 0.5f - sh->y) <= sh->h * 0.5f + 1.5f;
                }
                r->left += !in_word && lum(gl.px[y * S + x]) > 420 && lum(plain[y * S + x]) < 150;   /* an earlier word's */
            }
        }
    }
    fluid_fine_dust_kind(FINE_DUST_SPECKS);
    free(gl.px); free(plain); free(dots); free(keep); free(still);
    fluid_fine_destroy(fine); fluid_draw_destroy(d); fluid_frame_destroy(fr); fluid_sim_destroy(s);
}

static void test_scene_dust(const char *dir) {
    char path[512]; size_t len; const char *err;
    snprintf(path, sizeof(path), "%s/astrology.json", dir);
    char *buf = read_file(path, &len);
    pkg_t *p = buf ? pkg_parse(buf, len, &err) : NULL;
    free(buf);
    scene_t *chart = p ? sd_scene(p, dir, "chart") : NULL, *close = p ? sd_scene(p, dir, "sign-closeup") : NULL;
    CHECK(chart && close, "scene dust: the chart and a sign's close-up build");
    if (!chart || !close) { scene_free(chart); scene_free(close); pkg_unref(p); return; }
    const int S = 466;
    const float D = 3.14159265f / 180.f;
    const int moon = spot_named(chart, "moon"), leo = spot_named(chart, "leo");
    struct { const char *name; scene_t *sc; int spot; float x, y; } TAP[] = {
        { "chart, empty space", chart, -1, 0.15f, 0.3f },
        { "chart, a planet (the Moon)", chart, moon, moon >= 0 ? chart->items[moon].x : 0.f, moon >= 0 ? chart->items[moon].y : 0.f },
        { "chart, a sign (Leo)", chart, leo, 0.76f * cosf(302.f * D), 0.76f * sinf(302.f * D) },
        { "sign close-up, empty space", close, -1, -0.4f, -0.35f },
    };
    for (int q = 0; q < 4; q++) test_wobble(dir, TAP[q].sc, TAP[q].name, TAP[q].x, TAP[q].y);
    printf("  scene dust A/B, panel model with the measured constants (0.15 ms a window, 0.9 us a pixel; cells %.0f ms):\n", SD_CELLS_MS);
    sd_run_t r0, r1, r2;
    for (int q = 0; q < 4; q++) {
        sd_run(TAP[q].sc, FINE_DUST_DOTS, TAP[q].spot, TAP[q].x, TAP[q].y, &r1, q == 2 ? getenv("SD_DUMP_TAP") : NULL, NULL);
        sd_run(TAP[q].sc, FINE_DUST_DOTS_ILACE, TAP[q].spot, TAP[q].x, TAP[q].y, &r2, NULL, NULL);
        sd_run(TAP[q].sc, FINE_DUST_SPECKS, TAP[q].spot, TAP[q].x, TAP[q].y, &r0, NULL, NULL);
        printf("   %s:\n", TAP[q].name);
        sd_print("dots, calm", &r1.calm); sd_print("dots, the tap's wave (60 frames)", &r1.tap);
        sd_print("dots interlaced 2:1, the tap's wave", &r2.tap);
        sd_print("dots interlaced 2:1, calm", &r2.calm);
        /* the default: alive at rest (some dust changes every calm frame: its twinkle and the light on the water), and
         * every frame of a tap's wave within 33 ms on the measured panel (the water's rows taking turns as the creature's) */
        CHECK(r2.calm.px > 0 && r2.calm.worst + SD_CELLS_MS <= 33.0 && r2.tap.worst + SD_CELLS_MS <= 33.0 && r2.off == 0,
              "scene dust by default (%s): alive at rest (%.1f kpx a calm frame), a tap's wave within a frame (worst %.1f ms with the cells; calm %.1f)",
              TAP[q].name, r2.calm.px / 1e3 / r2.calm.n, r2.tap.worst + SD_CELLS_MS, r2.calm.worst + SD_CELLS_MS);
        sd_print("specks, calm", &r0.calm); sd_print("specks, the tap", &r0.tap);
        CHECK(r1.mismatch == 0, "scene dust (%s): every cell's dust is the creature screen's dots on the same water (%d cell frames differ)", TAP[q].name, r1.mismatch);
        printf("    the dots' wave: front %d px from the finger at frame 3, %d at frame 30, %d at most (frame %d); %d frames moving\n", r1.front0, r1.front1, r1.fmax, r1.fmax_at, r1.frames_front);
        CHECK(r1.front0 > 0 && r1.fmax > r1.front0 + 50 && r1.frames_front >= 40,
              "scene dust (%s): the tap's wave carries the dust outward as on the creature's screen (front %d px at frame 3, %d at frame 30, %d at most at frame %d; %d frames moving)",
              TAP[q].name, r1.front0, r1.front1, r1.fmax, r1.fmax_at, r1.frames_front);
        CHECK(r1.off == 0 && r0.off == 0, "scene dust (%s): the glass is the look every frame, both kinds (%d, %d frames behind)", TAP[q].name, r1.off, r0.off);
        if (q == 0 || q == 3) {
            /* the lines as today's: with the dots, every pixel that differs from the drawing alone (no dust) was black and
             * is a dust dot's; none of the drawing's pixels changed */
            sd_run_t rn;
            sd_run(TAP[q].sc, FINE_DUST_SPECKS, -2, 0.f, 0.f, &rn, NULL, NULL);   /* (no spot, no set: the drawing alone) */
            free(rn.calm_fb);
            int W, H; fluid_grid_for_stage(S, &W, &H);
            fluid_draw_t *dd = fluid_draw_create(W, H, S, S);
            int changed_line = 0, dust_px = 0, stray = 0;
            /* the drawing alone: kind 0 with no fine set (its specks never built) */
            {
                fluid_sim_t *s = fluid_sim_create(W, H, S); s->p.still = true;
                fluid_frame_t *fr = fluid_frame_create(W, H);
                fluid_fine_t *fine = fluid_fine_create(W, H, S, S);
                const int N = W * H;
                uint8_t *dots = calloc((size_t)N, 1), *keep = calloc((size_t)N, 1), *still = calloc((size_t)N, 1);
                static fluid_shape_t shp[FLUID_MAX_SHAPES];
                fluid_letter_t L[FLUID_MAX_LETTERS]; int nl;
                scene_shapes_t hd = { shp, 0, keep, still };
                scene_fill(s, TAP[q].sc, 1.f, NULL, L, &nl, dots, &hd);
                fluid_sim_snapshot(s, fr, false);
                fluid_draw_opts_t o = { .palette = FLUID_PAL_PASTEL, .edge_mode = FLUID_EDGE_CONTOURS, .letters = L, .nletters = nl, .font = gc_font5x7,
                                        .dots = dots, .scene = true, .shapes = shp, .nshapes = hd.n, .kdots = keep, .fine = fine, .still = still, .time = 1.f };
                uint16_t *alone = calloc((size_t)S * S, 2);
                fluid_fine_dust_kind(FINE_DUST_SPECKS);
                fluid_draw_cells(dd, fr, &o);
                fluid_draw_rect(dd, &o, &(fluid_rect_t){ alone, S, 0, 0, S, S }, true);
                fluid_fine_dust_kind(FINE_DUST_SPECKS);
                for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
                    const int qq = y * S + x;
                    if (alone[qq] == r1.calm_fb[qq]) continue;
                    if (alone[qq]) { changed_line++; continue; }
                    dust_px++;
                    stray += dd->yrow[y] < 0;                   /* off every dot row: not a dot */
                }
                free(alone); free(dots); free(keep); free(still);
                fluid_fine_destroy(fine); fluid_frame_destroy(fr); fluid_sim_destroy(s);
            }
            fluid_draw_destroy(dd);
            CHECK(changed_line == 0 && stray == 0 && dust_px > 2000, "scene dust (%s): the lines are today's pixel for pixel, the dust beside them on black only (%d drawn px changed, %d dust px, %d off the dots)",
                  TAP[q].name, changed_line, dust_px, stray);
            dump_ppm(q ? getenv("SD_DUMP_CLOSE") : getenv("SD_DUMP_CALM"), r1.calm_fb, S);
        }
        free(r0.calm_fb); free(r1.calm_fb); free(r2.calm_fb);
    }
    {   /* the accretion: denser and on the lattice near the drawing, sparse far from it, brighter near */
        int W, H; fluid_grid_for_stage(S, &W, &H);
        const int N = W * H;
        fluid_sim_t *s = fluid_sim_create(W, H, S); s->p.still = true;
        fluid_frame_t *fr = fluid_frame_create(W, H);
        fluid_draw_t *d = fluid_draw_create(W, H, S, S);
        fluid_fine_t *fine = fluid_fine_create(W, H, S, S);
        uint8_t *dots = calloc((size_t)N, 1), *keep = calloc((size_t)N, 1), *still = calloc((size_t)N, 1);
        static fluid_shape_t shp[FLUID_MAX_SHAPES];
        fluid_letter_t L[FLUID_MAX_LETTERS]; int nl;
        scene_shapes_t hd = { shp, 0, keep, still };
        scene_fill(s, chart, 1.f, NULL, L, &nl, dots, &hd);
        fluid_sim_snapshot(s, fr, false);
        fluid_draw_opts_t o = { .palette = FLUID_PAL_PASTEL, .edge_mode = FLUID_EDGE_CONTOURS, .letters = L, .nletters = nl, .font = gc_font5x7,
                                .dots = dots, .scene = true, .shapes = shp, .nshapes = hd.n, .kdots = keep, .fine = fine, .still = still, .time = 1.f };
        fluid_fine_dust_kind(FINE_DUST_DOTS_ILACE);
        fluid_draw_cells(d, fr, &o);
        int on[3] = { 0 }, all[3] = { 0 }, lv_near = 0, lv_far = 0, n_near = 0, n_far = 0;
        for (int y = 0; y < 4 * H; y++) for (int x = 0; x < 2 * W; x++) {
            const float px = d->sd_dist[y * 2 * W + x] * 0.25f;
            const int b = px >= 2.f && px < 7.f ? 0 : px >= 14.f && px < 24.f ? 1 : px >= 40.f ? 2 : -1;
            const int k = (y >> 2) * W + (x >> 1);
            if (b < 0 || !fr->mask[k]) continue;
            all[b]++;
            if (d->sd_dots[k] >> fluid_braille_bit(x & 1, y & 3) & 1) {
                on[b]++;
                if (b == 0) { lv_near += lum(d->sd_col[k]); n_near++; } else if (b == 2) { lv_far += lum(d->sd_col[k]); n_far++; }
            }
        }
        const float d0 = (float)on[0] / all[0], d1 = (float)on[1] / all[1], d2 = (float)on[2] / all[2];
        printf("    the dots gather: density %.2f within 7 px of the drawing, %.2f at 14..24, %.2f past 40\n", d0, d1, d2);
        CHECK(d0 > 0.3f && d0 > 1.5f * d1 && d1 > d2 && d2 > 0.02f && d2 < 0.12f && n_near && n_far && lv_near / n_near > lv_far / n_far,
              "scene dust: the dots gather to the drawing (density %.2f within 7 px, %.2f at 14..24, %.2f past 40), brighter near", d0, d1, d2);
        {   /* the reading: each word on its dark backdrop (no dot on the box once it is up), sent every frame it changes */
            sd_read_t rr;
            sd_read(chart, FINE_DUST_DOTS_ILACE, &rr, NULL);
            sd_print("dots 2:1, a reading (3 words, its end)", &rr.st);
            CHECK(rr.st.worst + SD_CELLS_MS <= 33.0, "scene dust, kind 2: a reading within a frame (worst %.1f ms with the cells)", rr.st.worst + SD_CELLS_MS);
            CHECK(rr.on_box == 0 && rr.off == 0 && rr.late == 0, "scene dust: a reading's words on their dark backdrop (%d dots on it), sent every frame they change (%d frames behind, late %x)", rr.on_box, rr.off, rr.late);
        }
        fluid_fine_dust_kind(FINE_DUST_SPECKS);
        free(dots); free(keep); free(still);
        fluid_fine_destroy(fine); fluid_draw_destroy(d); fluid_frame_destroy(fr); fluid_sim_destroy(s);
    }
    scene_free(chart); scene_free(close);
    pkg_unref(p);
}

/* ── the fine dust rides the water everywhere (kind 0): a tap anywhere sends its wave through all the dust, at the water's
 * speed, every moved speck sent the frame it moves, all of it back in place once the water is calm; at rest it drifts
 * slowly; near the lines it is half as dense as it was. Each run beside an untapped twin (the same clock): what the tap
 * moved is where the two differ. Costs on the device's measured constants (model_ms), cells SD_CELLS_MS. ── */
typedef struct { sd_stat_t calm, tap; int far_moved, far_sent, ahead, off, late, behind_twin, frames_twin, far3, far15, far30, calm_cells, calm_moved, jitter, calm_px; } ride_t;
static void ride_print(const char *what, const sd_stat_t *t) {
    printf("    %-30s %5.1f / %5.1f / %2d of %d over 33 (ms with the cells: mean / worst); %4.1f kpx mean, %4.1f max; %4.1f windows mean, %d max\n", what,
           t->msum / t->n + SD_CELLS_MS, t->mworst + SD_CELLS_MS, t->mover, t->n, t->px / 1e3 / t->n, t->pxmax / 1e3, (double)t->wins / t->n, t->winmax);
}
typedef struct { fluid_sim_t *s; fluid_frame_t *fr; fluid_draw_t *d; fluid_fine_t *fine; uint8_t *dots, *keep, *still; fluid_shape_t *shp; glass_t gl; fluid_letter_t L[FLUID_MAX_LETTERS]; fluid_draw_opts_t o; uint16_t *snap; } ride_rig_t;
static void rig_open(ride_rig_t *g, int S) {
    int W, H; fluid_grid_for_stage(S, &W, &H);
    const int N = W * H;
    memset(g, 0, sizeof(*g));
    g->s = fluid_sim_create(W, H, S); g->s->p.still = true;
    g->fr = fluid_frame_create(W, H); g->d = fluid_draw_create(W, H, S, S); g->fine = fluid_fine_create(W, H, S, S);
    g->dots = calloc((size_t)N, 1); g->keep = calloc((size_t)N, 1); g->still = calloc((size_t)N, 1);
    g->shp = calloc(FLUID_MAX_SHAPES, sizeof(fluid_shape_t));
    g->gl = (glass_t){ calloc((size_t)S * S, 2), S, { 0, 0, 0, 0 }, NULL, NULL };
    g->o = (fluid_draw_opts_t){ .palette = FLUID_PAL_PASTEL, .edge_mode = FLUID_EDGE_CONTOURS, .letters = g->L, .font = gc_font5x7,
                                .dots = g->dots, .scene = true, .kdots = g->keep, .fine = g->fine, .still = g->still };
    /* the scene's glass kept, as the device keeps it (the panel's framebuffer): its changed cells are drawn into it and
     * every window copied from it (fluid_draw_send2); glass_off checks the panel against the look each frame */
    g->snap = calloc((size_t)S * S, 2);
    fluid_draw_morph_look(g->d, FLUID_LOOK_KEEP | 1u, g->snap, true);
}
static void rig_close(ride_rig_t *g) {
    free(g->gl.px); free(g->snap); free(g->dots); free(g->keep); free(g->still); free(g->shp);
    fluid_fine_destroy(g->fine); fluid_draw_destroy(g->d); fluid_frame_destroy(g->fr); fluid_sim_destroy(g->s);
}
/* one frame of a rig at time t: the scene's shapes and dust, two water steps, the cells, the send onto its glass */
static void rig_frame(ride_rig_t *g, scene_t *sc, float t, bool full, bool keep_set) {
    static fluid_shape_t full_sh[FLUID_MAX_SHAPES];
    static uint16_t wbuf[466 * 18];
    fluid_shape_t *sh = g->shp;
    int nl;
    scene_shapes_t hd = { sh, 0, g->keep, g->still };
    scene_fill(g->s, sc, 1.f, NULL, g->L, &nl, g->dots, &hd);
    g->o.shapes = sh; g->o.nshapes = hd.n; g->o.nletters = nl;
    uint32_t key;
    const int nn = scene_full_shapes(g->s, sc, 1.f, full_sh, &key);
    fluid_fine_want(g->fine, 1, key, keep_set, full_sh, nn, FLUID_PAL_PASTEL);
    if (fluid_fine_work(g->fine, 0)) fluid_fine_swap(g->fine);
    for (int n = 0; !full && n < 2; n++) fluid_sim_step(g->s, DT / 2);   /* (as the device: two 1/60 s steps a frame at 30 fps) */
    fluid_sim_snapshot(g->s, g->fr, false);
    g->o.time = t;
    fluid_draw_cells(g->d, g->fr, &g->o);
    g->gl.c = (cost_t){ 0, 0, 0, 0 };
    for (int k = 0; !full && k < g->d->W * g->d->H; k++)
        if (g->d->dirty[k]) g->gl.c.comp += (g->d->cx[k % g->d->W + 1] - g->d->cx[k % g->d->W]) * (g->d->cy[k / g->d->W + 1] - g->d->cy[k / g->d->W]);
    fluid_draw_send(g->d, &g->o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, full, wbuf, 4096, send_glass, &g->gl);
}
static void ride_run(pkg_t *p, const char *dir, const char *layout, int spot, float x, float y, ride_t *r, const char *dump_calm, const char *dump_wave) {
    const int S = 466; int W, H;
    fluid_grid_for_stage(S, &W, &H);
    const int N = W * H;
    scene_t *sc = sd_scene(p, dir, layout), *twin = sd_scene(p, dir, layout);
    memset(r, 0, sizeof(*r));
    fluid_fine_dust_kind(FINE_DUST_SPECKS);
    ride_rig_t A, B;
    rig_open(&A, S); rig_open(&B, S);
    s_glass_model = true;
    scene_grow_t g;
    if (spot >= 0) scene_grow_tap(&g, spot);
    const float tx = (x + 1.f) * S / 2, ty = (1.f - y) * S / 2;
    int8_t *px0 = calloc((size_t)N, 2), *ever = calloc((size_t)N, 1);
    uint8_t *last = calloc((size_t)N, 2);
    for (int f = -48; f < 180; f++) {
        if (f >= 0 && spot >= 0) { scene_grow_tick(&g, true, DT); scene_grow_apply(&g, sc); }
        if (f == 0) {                                   /* the device's tap: a spot's outline, and the finger's ripple */
            if (spot >= 0) scene_outline(sc, spot, outline_bump, A.s);
            fluid_sim_impulse(A.s, x, -y, 1.6f, 0.09f);
        }
        const float t = 1.f + (f + 48) / 30.f;
        rig_frame(&A, sc, t, f == -48, f < 0);
        rig_frame(&B, twin, t, f == -48, true);
        if (f >= -40 && f < 0) sd_add(&r->calm, &A.gl.c);
        if (f >= 0 && f < 90) sd_add(&r->tap, &A.gl.c);
        if (getenv("RIDE_TRACE") && f >= -2 && f < 100) { int nd = 0; for (int k = 0; k < N; k++) nd += A.d->dirty[k] != 0; printf("    ride f%d: %.1f ms, %d windows, %ld px (%ld composed), %d cells\n", f, model_ms(&A.gl.c), A.gl.c.wins, A.gl.c.px, A.gl.c.comp, nd); }
        if (f > -48) { const int go = glass_off(A.d, &A.o, A.gl.px, S); r->off += go != 0; if (go && getenv("RIDE_TRACE")) printf("    behind f%d: %d px\n", f, go); }
        if (f == -1 && dump_calm) dump_ppm(dump_calm, A.gl.px, S);
        if (f == 10 && dump_wave) dump_ppm(dump_wave, A.gl.px, S);
        const fluid_fine_t *fa = A.fine, *fb = B.fine;
        int far = 0;
        for (int j = 1; j < H - 1; j++) for (int i = 1; i < W - 1; i++) {
            const int k = j * W + i;
            if (!A.fr->mask[k] || !fa->drawn || fa->drawn->start[k] == fa->drawn->start[k + 1]) continue;
            /* every speck that moves is sent the frame it moves: a cell whose shift changed is drawn again */
            const bool stepped = fa->sx[k] != px0[2 * k] || fa->sy[k] != px0[2 * k + 1];
            if (f > -40 && stepped && fa->tot[k] && !A.d->dirty[k]) r->late++;   /* (tot: a speck shows) */
            if (f >= -40 && f < 0) {                    /* at rest: the dust drifts, slowly (no cell steps again on an axis within 8 frames), a pixel at a time */
                r->calm_cells += f == -1;
                const int dx = fa->sx[k] - px0[2 * k], dy = fa->sy[k] - px0[2 * k + 1];
                if (f > -40 && (dx || dy)) {
                    if (!ever[k]) r->calm_moved++;
                    ever[k] = 1;
                    for (int a = 0; a < 2; a++) {
                        if (!(a ? dy : dx)) continue;
                        if (last[2 * k + a] && f - ((int)last[2 * k + a] - 64) < 8) r->jitter++;
                        last[2 * k + a] = (uint8_t)(f + 64);
                    }
                    if (abs(dx) > 1 || abs(dy) > 1) r->jitter++;
                }
            }
            px0[2 * k] = fa->sx[k]; px0[2 * k + 1] = fa->sy[k];
            if (f < 0) continue;
            const bool moved = fa->sx[k] != fb->sx[k] || fa->sy[k] != fb->sy[k];   /* moved by the tap's wave */
            if (!moved) continue;
            const float cx = (A.d->cx[i] + A.d->cx[i + 1]) * 0.5f, cy = (A.d->cy[j] + A.d->cy[j + 1]) * 0.5f;
            const int dd = (int)hypotf(cx - tx, cy - ty);
            if (dd > far) far = dd;
            /* only where the water moves */
            const int16_t *h = A.fr->h;
            if (!(h[k + 1] - h[k - 1]) && !(h[k + W] - h[k - W])) r->ahead++;
            const int sx2 = (int)cx >> 1, sy2 = (int)cy >> 1;
            const int dl = fa->dist ? fa->dist[sy2 * fa->S2 + sx2] : 0;   /* (1/100 px to the drawing) */
            if (dl > 3000 && dd > 150) { r->far_moved++; r->far_sent += stepped && A.d->dirty[k]; }
            if (f >= 150) r->behind_twin++;
        }
        if (f == 3) r->far3 = far;
        if (f == 15) r->far15 = far;
        if (f == 30) r->far30 = far;
        if (f >= 150 && spot < 0) { int dif = 0; for (int q = 0; q < S * S; q++) dif += A.gl.px[q] != B.gl.px[q]; r->frames_twin += dif != 0; }
        if (f == -1) { long px = 0; for (int q = 0; q < S * S; q++) px += A.gl.px[q] != 0; r->calm_px = (int)px; }
    }
    free(px0); free(ever); free(last);
    rig_close(&A); rig_close(&B);
    s_glass_model = false;
    scene_free(sc); scene_free(twin);
}
static void test_ride(const char *dir) {
    char path[512]; size_t len; const char *err;
    snprintf(path, sizeof(path), "%s/astrology.json", dir);
    char *buf = read_file(path, &len);
    pkg_t *p = buf ? pkg_parse(buf, len, &err) : NULL;
    free(buf);
    scene_t *chart = p ? sd_scene(p, dir, "chart") : NULL;
    CHECK(chart != NULL, "ride: the chart builds");
    if (!chart) { pkg_unref(p); return; }
    const float D = 3.14159265f / 180.f;
    const int moon = spot_named(chart, "moon"), leo = spot_named(chart, "leo");
    struct { const char *name, *layout; int spot; float x, y; const char *dump; } TAP[] = {
        { "a tap in the open middle", "chart", -1, 0.15f, 0.3f, "RIDE_DUMP_MIDDLE" },
        { "a tap on a planet (the Moon)", "chart", moon, moon >= 0 ? chart->items[moon].x : 0.f, moon >= 0 ? chart->items[moon].y : 0.f, "RIDE_DUMP_PLANET" },
        { "a tap on a sign (Leo)", "chart", leo, 0.76f * cosf(302.f * D), 0.76f * sinf(302.f * D), NULL },
        { "a sign close-up, a tap", "sign-closeup", -1, -0.4f, -0.35f, NULL },
    };
    printf("  the fine dust riding the water (kind 0, over its kept glass; panel model 0.17 ms a window, 0.06 us a px sent, 0.2 us a px drawn into the glass; cells %.0f ms):\n", SD_CELLS_MS);
    for (int q = 0; q < 4; q++) {
        ride_t r;
        ride_run(p, dir, TAP[q].layout, TAP[q].spot, TAP[q].x, TAP[q].y, &r, q == 0 ? getenv("RIDE_DUMP_CALM") : NULL, TAP[q].dump ? getenv(TAP[q].dump) : NULL);
        printf("   %s:\n", TAP[q].name);
        if (q == 0 || q == 3) ride_print("calm", &r.calm);
        ride_print("the tap's wave (90 frames)", &r.tap);
        printf("    the wave's front: %d px from the tap at frame 3, %d at 15, %d at 30; %d cell frames moved far from both the tap and the drawing\n", r.far3, r.far15, r.far30, r.far_moved);
        CHECK(r.far_moved > 200 && r.far3 < r.far15 && r.far15 < r.far30 && r.far30 >= 100 && r.ahead == 0,
              "ride (%s): the tap's wave moves the dust over the whole glass, far from any drawing too (%d cell frames), along the water's front (%d, %d, %d px at frames 3, 15, 30; %d moved where the water is still)",
              TAP[q].name, r.far_moved, r.far3, r.far15, r.far30, r.ahead);
        CHECK(r.off == 0 && r.late == 0 && r.far_sent > 100, "ride (%s): every speck that moves is sent the frame it moves, far ones too (%d cell frames; %d frames behind, %d cell frames late)",
              TAP[q].name, r.far_sent, r.off, r.late);
        CHECK(r.behind_twin == 0 && r.frames_twin == 0, "ride (%s): once the water is calm every speck is exactly in its rest place (%d cells off, %d frames of the glass off)", TAP[q].name, r.behind_twin, r.frames_twin);
        if (q == 0) CHECK(r.calm_moved * 2 > r.calm_cells && r.jitter == 0,
                          "ride: at rest the dust drifts, slowly and softly (%d of %d cells moved in 40 frames; %d moves faster than every 8 frames or by more than a pixel)", r.calm_moved, r.calm_cells, r.jitter);
        CHECK(r.tap.mworst + SD_CELLS_MS <= 33.0 && (q != 0 || r.calm.mworst + SD_CELLS_MS <= 33.0),
              "ride (%s): within a frame on the measured panel (worst %.1f ms with the cells; calm %.1f)", TAP[q].name, r.tap.mworst + SD_CELLS_MS, r.calm.mworst + SD_CELLS_MS);
    }
    {
        sd_read_t rr;
        sd_read(chart, FINE_DUST_SPECKS, &rr, getenv("RIDE_DUMP_READ"));
        ride_print("a reading (3 words, its end)", &rr.st);
        CHECK(rr.st.mworst + SD_CELLS_MS <= 33.0, "ride: a reading within a frame on the measured panel (worst %.1f ms with the cells)", rr.st.mworst + SD_CELLS_MS);
    }
    scene_free(chart);
    pkg_unref(p);
}

/* the reading's word over a scene is kept once folded (scene_fill): every frame it shows after its first comes out exactly
 * as the first (cells, dots, letters, shapes), and a new word in the same buffer is folded again, never the last one */
static void test_caption_kept(const char *dir) {
    char path[512]; size_t len; const char *err;
    snprintf(path, sizeof(path), "%s/astrology.json", dir);
    char *buf = read_file(path, &len);
    pkg_t *p = buf ? pkg_parse(buf, len, &err) : NULL;
    free(buf);
    scene_t *sc = p ? sd_scene(p, dir, "chart") : NULL;
    CHECK(sc != NULL, "caption kept: the chart builds");
    if (!sc) { pkg_unref(p); return; }
    int W, H; fluid_grid_for_stage(466, &W, &H);
    const int N = W * H;
    fluid_sim_t *s = fluid_sim_create(W, H, 466);
    static fluid_shape_t sh[2][FLUID_MAX_SHAPES];
    static fluid_letter_t L[2][FLUID_MAX_LETTERS];
    uint8_t *dots = calloc(4 * (size_t)N, 1), *keep = dots + 2 * N, *still = calloc((size_t)N, 1);
    int16_t *T0 = calloc((size_t)N, 2); uint32_t *c0 = calloc((size_t)N, 4);
    const char *words[] = { "Mars", "opposes", "Saturn.", "Mars" };
    char word[16];
    int bad = 0, hits = 0;
    for (int w = 0; w < 4; w++) {
        snprintf(word, sizeof(word), "%s", words[w]);          /* (one buffer for every word, as a reading's text) */
        const scene_caption_t cap = { word, (int)strlen(word), 1 };
        int nl[2];
        scene_shapes_t hd[2];
        for (int r = 0; r < 2; r++) {
            hd[r] = (scene_shapes_t){ sh[r], 0, keep + r * N, still };
            scene_fill(s, sc, 1.f, &cap, L[r], &nl[r], dots + r * N, &hd[r]);
            if (!r) { memcpy(T0, s->T, 2 * (size_t)N); memcpy(c0, s->tcol, 4 * (size_t)N); }
        }
        hits++;
        bad += memcmp(T0, s->T, 2 * (size_t)N) != 0 || memcmp(c0, s->tcol, 4 * (size_t)N) != 0 || memcmp(dots, dots + N, (size_t)N) != 0 ||
               memcmp(keep, keep + N, (size_t)N) != 0 || nl[0] != nl[1] || memcmp(L[0], L[1], sizeof(fluid_letter_t) * (size_t)nl[0]) != 0 ||
               hd[0].n != hd[1].n || (hd[0].n > 0 && memcmp(sh[0], sh[1], sizeof(fluid_shape_t) * (size_t)hd[0].n) != 0);
        int caps = 0;
        for (int q = 0; q < hd[1].n; q++) caps += sh[1][q].tag == FLUID_TAG_CAPTION;
        bad += caps == 0;                                       /* (the word's own letters are there) */
    }
    CHECK(bad == 0 && hits == 4, "caption kept: a word's frames after its first are exactly its first (%d of %d words differ), a new word in the same buffer folded anew", bad, hits);
    free(dots); free(still); free(T0); free(c0);
    fluid_sim_destroy(s);
    scene_free(sc);
    pkg_unref(p);
}

/* ── the two cores (the device's split, mole_fluid.c): the dust's next frame worked out on the sim task while the drawer
 * draws this one (fluid_fine_frame_ahead, shown by fluid_fine_publish at the handoff), and the kept glass's windows sent by
 * another task while the drawer draws the next band (fluid_draw_glass_sender). Here in one thread in the device's order:
 * each frame's dust is worked out before the frame before is sent (it must change nothing that send reads), its windows
 * queued and sent only at the frame's end (the rows they send must not change once queued). Beside it a twin drawn in
 * line as before: the same glass every frame, the same windows in the same order (each once), and the glass the look when
 * a frame ends; a tap in the open middle, a reading to its end, a tap on the Moon, the mark on and off. ── */
typedef struct { const uint16_t *glass; int stride, x, y, w, h, mark, pal; bool direct; uint32_t sum; } split_win_t;
typedef struct { split_win_t q[256]; int n, late, over, taken, bad; bool draining; } split_q_t;
typedef struct { glass_t *g; int n, mixed; int16_t v[512][4]; const split_q_t *q; } win_log_t;
/* the panel's bus as the device's board layer keeps it (molty_board.c): one owner at a time, a task's async transfer its
 * own till collected. Here: who has one out (0 none, 1 the drawer in line, 2 the sender), and transfers started by the
 * other meanwhile (breaches: every sender suite fails on one) */
static struct { int out, breach; } s_bus;
static void bus_send(int who) { if (s_bus.out && s_bus.out != who) s_bus.breach++; s_bus.out = who; }
static uint32_t rows_sum(const uint16_t *g, int stride, int x, int y, int w, int h) {
    uint32_t a = 2166136261u;
    for (int r = y; r < y + h; r++) for (int c = x; c < x + w; c++) a = (a ^ g[(size_t)r * stride + c]) * 16777619u;
    return a;
}
static int split_take(void *ctx, const uint16_t *glass, int stride, int x, int y, int w, int h, bool direct, int mark, int pal) {
    split_q_t *q = ctx;
    if (!q->n && s_bus.out == 1) s_bus.out = 0;          /* (the hand-over: the drawer's own last window collected first, tx_glass) */
    if (q->n == 256) { q->over++; return 1; }
    /* (what the device's sender takes: whole row pairs, x on 4 px, inside the glass; a copy fits its 4096 px buffer,
     * a straight send is whole rows) */
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > stride || (y & 1) || (h & 1) || (x & 3) ||
        (direct ? x != 0 || w != stride : w * h > 4096)) { q->bad++; if (getenv("SPLIT_TRACE")) printf("    bad record %d,%d %dx%d direct %d\n", x, y, w, h, direct); }
    q->q[q->n++] = (split_win_t){ glass, stride, x, y, w, h, mark, pal, direct, rows_sum(glass, stride, x, y, w, h) };
    q->taken++;
    return 0;
}
static int send_log(void *ctx, int x, int y, int w, int h, const uint16_t *px) {
    win_log_t *L = ctx;
    if (L->q && L->q->n) L->mixed++;                    /* (a window sent here while some wait queued: two senders) */
    bus_send(L->q && !L->q->n && L->q->draining ? 2 : 1);
    if (L->n < 512) { L->v[L->n][0] = (int16_t)x; L->v[L->n][1] = (int16_t)y; L->v[L->n][2] = (int16_t)w; L->v[L->n][3] = (int16_t)h; }
    L->n++;
    return send_glass(L->g, x, y, w, h, px);
}
/* the frame's end: each window queued as the device's sender sends it (copied and the mark over it, or straight), in order */
static void split_drain(split_q_t *q, const fluid_draw_t *d, win_log_t *L) {
    static uint16_t buf[4096];
    const int n = q->n;
    q->n = 0; q->draining = true;
    for (int k = 0; k < n; k++) {
        const split_win_t *r = &q->q[k];
        if (rows_sum(r->glass, r->stride, r->x, r->y, r->w, r->h) != r->sum) q->late++;
        if (r->direct) { send_log(L, r->x, r->y, r->w, r->h, r->glass + (size_t)r->y * r->stride); continue; }
        for (int y = 0; y < r->h; y++) memcpy(buf + (size_t)y * r->w, r->glass + (size_t)(r->y + y) * r->stride + r->x, sizeof(uint16_t) * (size_t)r->w);
        fluid_draw_mark(d, &(fluid_rect_t){ buf, r->w, r->x, r->y, r->x + r->w, r->y + r->h }, r->pal, r->mark);
        send_log(L, r->x, r->y, r->w, r->h, buf);
    }
    q->draining = false;
    if (s_bus.out == 2) s_bus.out = 0;                   /* (the frame end: the sender waits its own last one) */
}
typedef struct { fluid_shape_t shp[FLUID_MAX_SHAPES]; fluid_letter_t L[FLUID_MAX_LETTERS]; uint8_t *dots, *keep, *still; int n, nl; } split_side_t;
static void test_split(const char *dir) {
    char path[512]; size_t len; const char *err;
    snprintf(path, sizeof(path), "%s/astrology.json", dir);
    char *buf = read_file(path, &len);
    pkg_t *p = buf ? pkg_parse(buf, len, &err) : NULL;
    free(buf);
    scene_t *sc = p ? sd_scene(p, dir, "chart") : NULL;
    CHECK(sc != NULL, "split: the chart builds");
    if (!sc) { pkg_unref(p); return; }
    const int S = 466; int W, H;
    fluid_grid_for_stage(S, &W, &H);
    const int N = W * H;
    fluid_fine_dust_kind(FINE_DUST_SPECKS);
    ride_rig_t *A = calloc(1, sizeof(*A)), *C = calloc(1, sizeof(*C));
    split_side_t *sim = calloc(1, sizeof(*sim));
    split_q_t *q = calloc(1, sizeof(*q));
    win_log_t *la = calloc(1, sizeof(*la)), *lc = calloc(1, sizeof(*lc));
    rig_open(A, S); rig_open(C, S);
    sim->dots = calloc((size_t)N, 1); sim->keep = calloc((size_t)N, 1); sim->still = calloc((size_t)N, 1);
    la->g = &A->gl; lc->g = &C->gl; lc->q = q;
    static fluid_shape_t full_sh[FLUID_MAX_SHAPES];
    static uint16_t wbuf[4096];
    const char *words[] = { "Mars", "opposes", "Saturn." };
    float ub[4];
    for (int w = 0; w < 3; w++) {
        float b[4];
        const scene_caption_t c = { words[w], (int)strlen(words[w]), 1 };
        scene_caption_box(&c, sc->caption_y, S, b);
        if (!w) memcpy(ub, b, sizeof(b));
        ub[0] = fminf(ub[0], b[0]); ub[1] = fmaxf(ub[1], b[1]); ub[2] = fminf(ub[2], b[2]); ub[3] = fmaxf(ub[3], b[3]);
    }
    fluid_fine_caption_box(A->fine, ub); fluid_fine_caption_box(C->fine, ub);
    const int moon = spot_named(sc, "moon");
    scene_grow_t g;
    int frames = 0, diff = 0, lists = 0, off = 0, ahead = 0, hazard = 0, bands = 0, mark_was = FLUID_MARK_NONE, mark_c = FLUID_MARK_NONE;
    bool go = false, full_c = true, grow = false;
    const int F0 = -16, F1 = 150;
    for (int f = F0; f <= F1; f++) {
        /* the script: calm, a tap in the middle at 0, a reading from 30 (3 words, 12 frames each, its end), the Moon at 90;
         * the mark from 60 to 100 */
        const int rw = f >= 30 && f < 66 ? (f - 30) / 12 : -1;
        const scene_caption_t cap = rw >= 0 ? (scene_caption_t){ words[rw], (int)strlen(words[rw]), 1 } : (scene_caption_t){ NULL, 0, -1 };
        const int mark = f >= 60 && f < 100 ? FLUID_MARK_DOT : FLUID_MARK_NONE;
        const bool full = f == F0 || mark != mark_was;
        mark_was = mark;
        const float t = 1.f + (f - F0) / 30.f;
        if (f == 90 && moon >= 0) { scene_grow_tap(&g, moon); grow = true; }
        if (grow) { scene_grow_tick(&g, true, DT); scene_grow_apply(&g, sc); }
        ride_rig_t *R[2] = { A, C };
        for (int r = 0; r < 2; r++) {
            if (f == 0) fluid_sim_impulse(R[r]->s, 0.15f, -0.3f, 1.6f, 0.09f);
            if (f == 90 && moon >= 0) { scene_outline(sc, moon, outline_bump, R[r]->s); fluid_sim_impulse(R[r]->s, sc->items[moon].x, -sc->items[moon].y, 1.6f, 0.09f); }
        }
        /* the split: the sim's frame (its own shapes and dots), the dust worked out ahead ... */
        scene_shapes_t hd = { sim->shp, 0, sim->keep, sim->still };
        scene_fill(C->s, sc, 1.f, &cap, sim->L, &sim->nl, sim->dots, &hd);
        sim->n = hd.n;
        uint32_t key;
        const int nn = scene_full_shapes(C->s, sc, 1.f, full_sh, &key);
        fluid_fine_want(C->fine, 1, key, !grow, full_sh, nn, FLUID_PAL_PASTEL);
        const bool ready = fluid_fine_work(C->fine, 0);
        if (f > F0) { fluid_sim_step(C->s, DT / 2); fluid_sim_step(C->s, DT / 2); }
        const unsigned word = cap.n ? (unsigned)(rw + 1) : 0;
        if (go && !fluid_fine_swap_due(C->fine)) {
            const fluid_frame_t fv = { .W = W, .H = H, .h = C->s->h };
            const fluid_draw_opts_t fo = { .palette = FLUID_PAL_PASTEL, .time = t, .shapes = sim->shp, .nshapes = sim->n, .kdots = sim->keep, .cap_word = word };
            C->fine->pause = false;
            fluid_fine_frame_ahead(C->fine, &fv, &fo);
            hazard += f > F0;
        }
        /* ... before the drawer's frame before goes out, its windows through the queue */
        if (f > F0) {
            fluid_draw_glass_sender(split_take, q);
            lc->n = 0;
            fluid_draw_send(C->d, &C->o, &(fluid_overlay_t){ 0 }, mark_c, full_c, wbuf, 4096, send_log, lc);
            fluid_draw_glass_sender(NULL, NULL);
            bands += q->n > 0;
            split_drain(q, C->d, lc);
            int dif = 0;                                /* (the twin's glass and windows: still the frame before's) */
            for (int k = 0; k < S * S; k++) dif += C->gl.px[k] != A->gl.px[k];
            diff += dif != 0;
            lists += lc->n != la->n || memcmp(lc->v, la->v, sizeof(int16_t) * 4 * (size_t)(lc->n < 512 ? lc->n : 512));
            if (mark_c == FLUID_MARK_NONE) off += glass_off(C->d, &C->o, C->gl.px, S) != 0;
            if (dif && getenv("SPLIT_TRACE")) printf("    split f%d: %d px off the twin; windows %d / %d\n", f - 1, dif, lc->n, la->n);
            frames++;
        }
        /* the twin, in line */
        {
            scene_shapes_t hd = { A->shp, 0, A->keep, A->still };
            int nl;
            scene_fill(A->s, sc, 1.f, &cap, A->L, &nl, A->dots, &hd);
            A->o.shapes = A->shp; A->o.nshapes = hd.n; A->o.nletters = nl; A->o.cap_word = cap.n ? (unsigned)(rw + 1) : 0;
            uint32_t key;
            const int nn = scene_full_shapes(A->s, sc, 1.f, full_sh, &key);
            fluid_fine_want(A->fine, 1, key, !grow, full_sh, nn, FLUID_PAL_PASTEL);
            if (fluid_fine_work(A->fine, 0)) fluid_fine_swap(A->fine);
            if (f > F0) { fluid_sim_step(A->s, DT / 2); fluid_sim_step(A->s, DT / 2); }
            fluid_sim_snapshot(A->s, A->fr, false);
            A->o.time = t;
            fluid_draw_cells(A->d, A->fr, &A->o);
            la->n = 0;
            fluid_draw_send(A->d, &A->o, &(fluid_overlay_t){ 0 }, mark, full, wbuf, 4096, send_log, la);
        }
        /* the handoff: the set that waits, the drawer's copies, the frame shown, the drawer's next frame */
        if (ready) fluid_fine_swap(C->fine);
        memcpy(C->shp, sim->shp, sizeof(fluid_shape_t) * (size_t)(sim->n > 0 ? sim->n : 0)); memcpy(C->L, sim->L, sizeof(sim->L));
        memcpy(C->dots, sim->dots, (size_t)N); memcpy(C->keep, sim->keep, (size_t)N); memcpy(C->still, sim->still, (size_t)N);
        C->o.shapes = C->shp; C->o.nshapes = sim->n; C->o.nletters = sim->nl; C->o.cap_word = word; C->o.time = t;
        fluid_sim_snapshot(C->s, C->fr, false);
        go = true;
        if (!C->fine->bready) { C->fine->pause = false; fluid_fine_frame_ahead(C->fine, C->fr, &C->o); }
        fluid_fine_publish(C->fine, go);
        ahead += C->fine->ahead;
        fluid_draw_cells(C->d, C->fr, &C->o);
        mark_c = mark; full_c = full;
    }
    printf("  the two cores (the dust ahead, the glass's windows queued): %d frames, %d with their dust worked out ahead (%d of them before the frame before went out), %d sent through the queue (%d windows)\n",
           frames, ahead, hazard, bands, q->taken);
    CHECK(ahead > frames / 2 && hazard > frames / 2 && bands > frames / 2 && q->taken > 4 * frames,
          "split: the dust worked out ahead and the windows queued on most frames (%d, %d, %d of %d)", ahead, hazard, bands, frames);
    CHECK(diff == 0 && lists == 0 && lc->mixed == 0 && q->over == 0,
          "split: the same glass as the drawer in line every frame (%d off), the same windows in the same order, each once (%d frames differ; %d sent while some waited; %d not taken)",
          diff, lists, lc->mixed, q->over);
    CHECK(q->late == 0 && off == 0 && q->bad == 0 && s_bus.breach == 0, "split: no queued row drawn again before it went out (%d), every window one the sender takes (%d not), "
          "one owner of the panel at a time (%d breaches), the glass the look when each frame ends (%d frames off)", q->late, q->bad, s_bus.breach, off);
    s_bus.breach = 0;
    free(sim->dots); free(sim->keep); free(sim->still); free(sim); free(q); free(la); free(lc);
    rig_close(A); rig_close(C); free(A); free(C);
    scene_free(sc);
    pkg_unref(p);
}

/* ── a tap on a planet, in turns (scene_grow_settled, SCENE_GROW_HOLD_S): its drawing grows, then its reading's first
 * word comes on its backdrop (mole_fluid: the word waits for the growth, and stays while the backdrop comes in), and
 * after the reading the backdrop lifts before the drawing eases back. The growth and the caption's backdrop never
 * change the same frame. ── */
static void test_retime(const char *dir) {
    char path[512]; size_t len; const char *err;
    snprintf(path, sizeof(path), "%s/astrology.json", dir);
    char *buf = read_file(path, &len);
    pkg_t *p = buf ? pkg_parse(buf, len, &err) : NULL;
    free(buf);
    scene_t *sc = p ? sd_scene(p, dir, "chart") : NULL;
    const int moon = sc ? spot_named(sc, "moon") : -1;
    CHECK(sc && moon >= 0, "retime: the chart and its Moon");
    if (!sc || moon < 0) { scene_free(sc); pkg_unref(p); return; }
    const int S = 466, N0 = 15;                         /* (the first word stays FINE_DARK_IN_S: 15 frames) */
    fluid_fine_dust_kind(FINE_DUST_SPECKS);
    ride_rig_t *A = calloc(1, sizeof(*A));
    rig_open(A, S);
    static fluid_shape_t full_sh[FLUID_MAX_SHAPES];
    static uint16_t wbuf[4096];
    const char *words[] = { "Mars", "opposes", "Saturn." };
    float ub[4];
    for (int w = 0; w < 3; w++) {
        float b[4];
        const scene_caption_t c = { words[w], (int)strlen(words[w]), 1 };
        scene_caption_box(&c, sc->caption_y, S, b);
        if (!w) memcpy(ub, b, sizeof(b));
        ub[0] = fminf(ub[0], b[0]); ub[1] = fmaxf(ub[1], b[1]); ub[2] = fminf(ub[2], b[2]); ub[3] = fmaxf(ub[3], b[3]);
    }
    scene_grow_t g = { -1, 0.f, 0.f, 0.f };
    int word = -1, until = 0, both = 0, grew = 0, worded = 0, first = -1, settled_at = -1, lifted = -1, back = -1;
    bool on = false;
    float g0 = 1.f;
    for (int f = -8; f < 150; f++) {
        if (f == 0) {                                   /* the tap: its outline's wave, its drawing grows, its reading waits */
            scene_grow_tap(&g, moon);
            scene_outline(sc, moon, outline_bump, A->s); fluid_sim_impulse(A->s, sc->items[moon].x, -sc->items[moon].y, 1.6f, 0.09f);
            on = true; word = -1; until = 0;
        }
        if (on && f >= until && (word >= 0 || scene_grow_settled(&g))) {   /* (mole_fluid's reading clock, at 8 frames a word) */
            if (word + 1 < 3) { word++; until = f + (word ? 8 : N0); if (first < 0) first = f; }
            else if (f >= until + 30) on = false;      /* (its last word held SPOT_READ_HOLD) */
        }
        if (settled_at < 0 && f >= 0 && scene_grow_settled(&g)) settled_at = f;
        scene_grow_tick(&g, on, DT);
        scene_grow_apply(&g, sc);
        const scene_caption_t cap = on && word >= 0 ? (scene_caption_t){ words[word], (int)strlen(words[word]), 1 } : (scene_caption_t){ NULL, 0, -1 };
        fluid_fine_caption_box(A->fine, cap.n ? ub : NULL);
        scene_shapes_t hd = { A->shp, 0, A->keep, A->still };
        int nl;
        scene_fill(A->s, sc, 1.f, &cap, A->L, &nl, A->dots, &hd);
        A->o.shapes = A->shp; A->o.nshapes = hd.n; A->o.nletters = nl; A->o.cap_word = cap.n ? (unsigned)(word + 1) : 0;
        uint32_t key;
        const int nn = scene_full_shapes(A->s, sc, 1.f, full_sh, &key);
        fluid_fine_want(A->fine, 1, key, true, full_sh, nn, FLUID_PAL_PASTEL);
        if (fluid_fine_work(A->fine, 0)) fluid_fine_swap(A->fine);
        fluid_sim_step(A->s, DT / 2); fluid_sim_step(A->s, DT / 2);
        fluid_sim_snapshot(A->s, A->fr, false);
        A->o.time = 1.f + (f + 8) / 30.f;
        fluid_draw_cells(A->d, A->fr, &A->o);
        fluid_draw_send(A->d, &A->o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, f == -8, wbuf, 4096, send_glass, &A->gl);
        bool caption = false;                           /* (the caption, its backdrop coming or lifting, its word breaking up) */
        for (int k = 0; k < A->d->W * A->d->H && !caption; k++) caption = A->fine->why[k] >> FLUID_ANIM_WORD & 1;
        const bool grows = fabsf(sc->grow - g0) > 1e-6f;
        g0 = sc->grow;
        both += grows && caption; grew += grows; worded += caption;
        if (f > 0 && !on && lifted < 0 && !caption) lifted = f;
        if (lifted >= 0 && back < 0 && g.spot < 0) back = f;
        if (getenv("RETIME_TRACE") && f >= 0) printf("    retime f%d: word %d, grow %.3f%s%s\n", f, word, sc->grow, grows ? " (growing)" : "", caption ? " caption" : "");
    }
    printf("  retime: the growth settles at frame %d, the first word at %d; growing %d frames, the caption's %d; both %d; lifted at %d, back at %d\n",
           settled_at, first, grew, worded, both, lifted, back);
    CHECK(grew > 10 && worded > 40 && first >= settled_at && settled_at == (int)ceilf(SCENE_GROW_IN_S * 30.f - 1e-3f) && back > lifted && both == 0,
          "retime: the drawing grows (%d frames), then its reading's word and backdrop come (%d frames, from %d), lifting before it eases back (%d, %d); never the same frame (%d)",
          grew, worded, first, lifted, back, both);
    CHECK(SCENE_GROW_HOLD_S >= FINE_END_S, "retime: the drawing holds (%.2f s) while the backdrop lifts (%.2f s)", SCENE_GROW_HOLD_S, FINE_END_S);
    rig_close(A); free(A);
    scene_free(sc);
    pkg_unref(p);
}

/* EXPERIMENT kind 3 (fluid_fine_dust_kind FINE_DUST_FINE_DOTS; delete with it): the fine specks of kind 0 with a
 * sparse layer of the creature screen's dots on the water. At rest the glass is kind 0's plus hard dots on black pixels
 * only; a tap anywhere sends the creature screen's wave through them over the whole glass, every moved dot sent the
 * frame it moves; the cost on the measured panel model for each density. */
static void test_fine_hard(const char *dir) {
    char path[512]; size_t len; const char *err;
    snprintf(path, sizeof(path), "%s/astrology.json", dir);
    char *buf = read_file(path, &len);
    pkg_t *p = buf ? pkg_parse(buf, len, &err) : NULL;
    free(buf);
    scene_t *chart = p ? sd_scene(p, dir, "chart") : NULL;
    CHECK(chart != NULL, "kind 3: the chart builds");
    if (!chart) { pkg_unref(p); return; }
    const int S = 466;
    const float D = 3.14159265f / 180.f;
    const int moon = spot_named(chart, "moon"), leo = spot_named(chart, "leo");
    struct { const char *name; int spot; float x, y; } TAP[] = {
        { "a tap in the open middle", -1, 0.15f, 0.3f },
        { "a tap on a planet (the Moon)", moon, moon >= 0 ? chart->items[moon].x : 0.f, moon >= 0 ? chart->items[moon].y : 0.f },
        { "a tap on a sign (Leo)", leo, 0.76f * cosf(302.f * D), 0.76f * sinf(302.f * D) },
    };
    {   /* kind 0 (the default) with the drawing riding the water: a reading leaves nothing of an earlier word */
        sd_read_t rr;
        sd_read(chart, FINE_DUST_SPECKS, &rr, NULL);
        sd_print("kind 0, a reading (3 words, its end)", &rr.st);
        CHECK(rr.left == 0 && rr.off == 0 && rr.on_box == 0, "kind 0: after each word of a reading nothing of an earlier one is left (%d px), every frame sent (%d behind)", rr.left, rr.off);
    }
    static const int PCT[] = { 1, 2, 3, 5, 8 };
    printf("  kind 3, fine specks + hard dots (measured panel model, cells %.0f ms; ms with the cells: mean / worst / frames over 33):\n", SD_CELLS_MS);
    for (int n = 0; n < (int)(sizeof(PCT) / sizeof(PCT[0])); n++) {
        fluid_fine_hard_density(PCT[n]);
        char line[600];
        int lp = snprintf(line, sizeof(line), "    %d %%:", PCT[n]);
        for (int q = 0; q < 3; q++) {
            sd_run_t r0, r1, r3;
            char dp[600];
            const char *pre = getenv("SD3_DUMP");
            if (pre) snprintf(dp, sizeof(dp), "%s-%dpct-%d", pre, PCT[n], q);
            scene_t *c3 = sd_scene(p, dir, "chart"), *c0 = sd_scene(p, dir, "chart");   /* (each run from the untapped chart) */
            sd_run(c3, FINE_DUST_FINE_DOTS, TAP[q].spot, TAP[q].x, TAP[q].y, &r3, NULL, pre ? dp : NULL);
            sd_run(c0, FINE_DUST_SPECKS, TAP[q].spot, TAP[q].x, TAP[q].y, &r0, NULL, NULL);
            sd_run(chart, FINE_DUST_DOTS, -1, TAP[q].x, TAP[q].y, &r1, NULL, NULL);   /* (the creature screen's front from the same point) */
            scene_free(c3); scene_free(c0);
            fluid_fine_hard_density(PCT[n]);
            int lost = 0, added = 0;
            for (int k = 0; k < S * S; k++) { lost += r0.calm_fb[k] && r3.calm_fb[k] != r0.calm_fb[k]; added += !r0.calm_fb[k] && r3.calm_fb[k]; }
            if (!n) printf("    kind 0 (%s): calm %.1f / %.1f / %d, tap %.1f / %.1f / %d\n", TAP[q].name, r0.calm.sum / r0.calm.n + SD_CELLS_MS, r0.calm.worst + SD_CELLS_MS, r0.calm.over,
                           r0.tap.sum / r0.tap.n + SD_CELLS_MS, r0.tap.worst + SD_CELLS_MS, r0.tap.over);
            if (!q) lp += snprintf(line + lp, sizeof(line) - (size_t)lp, " calm %.1f / %.1f / %d;", r3.calm.sum / r3.calm.n + SD_CELLS_MS, r3.calm.worst + SD_CELLS_MS, r3.calm.over);
            lp += snprintf(line + lp, sizeof(line) - (size_t)lp, " %s %.1f / %.1f / %d;", q == 0 ? "middle" : q == 1 ? "planet" : "sign", r3.tap.sum / r3.tap.n + SD_CELLS_MS, r3.tap.worst + SD_CELLS_MS, r3.tap.over);
            CHECK(lost == 0 && added > 0, "kind 3 at %d %% (%s): at rest kind 0's glass, nothing of the drawing or the specks lost (%d px), hard dots on black only (%d px)", PCT[n], TAP[q].name, lost, added);
            /* (kind 0 itself leaves a tapped planet's first growth frame 1 px behind: a shape's edge past its stale cells) */
            CHECK(r3.mismatch == 0 && r3.off <= r0.off, "kind 3 at %d %% (%s): the hard dots are the creature screen's on the same water (%d cell frames differ), every moved one sent the frame it moves (%d frames behind, kind 0 %d)", PCT[n], TAP[q].name, r3.mismatch, r3.off, r0.off);
            CHECK(r3.frames_front >= 40 && abs(r3.front1 - r1.front1) <= 30 && r3.fmax >= r1.fmax - 30,
                  "kind 3 at %d %% (%s): the wave carries the hard dots over the glass with the creature screen's front (frame 30: %d px, the creature's %d; at most %d, %d; %d frames)",
                  PCT[n], TAP[q].name, r3.front1, r1.front1, r3.fmax, r1.fmax, r3.frames_front);
            free(r0.calm_fb); free(r1.calm_fb); free(r3.calm_fb);
        }
        sd_read_t rr;
        char rp[600];
        if (getenv("SD3_DUMP")) snprintf(rp, sizeof(rp), "%s-%dpct-read.ppm", getenv("SD3_DUMP"), PCT[n]);
        sd_read(chart, FINE_DUST_FINE_DOTS, &rr, getenv("SD3_DUMP") ? rp : NULL);
        printf("%s reading %.1f / %.1f / %d\n", line, rr.st.sum / rr.st.n + SD_CELLS_MS, rr.st.worst + SD_CELLS_MS, rr.st.over);
        CHECK(rr.on_box == 0 && rr.off == 0 && rr.left == 0, "kind 3 at %d %%: a reading's backdrop free of hard dots (%d), every frame sent (%d behind), nothing of an earlier word left (%d px)", PCT[n], rr.on_box, rr.off, rr.left);
    }
    fluid_fine_hard_density(FINE_HARD_PCT);
    scene_free(chart);
    pkg_unref(p);
}

/* ── the whole picture changing, as dust ("A for Molty", fluid_morph_t). A small driver as mole_fluid.c's: each frame
 * the look wanted is told to the drawer, its inputs written, the windows landing on a glass, as the panel's ── */
typedef struct {
    int S, W, H, N;
    fluid_sim_t *s; fluid_frame_t *fr; fluid_draw_t *d; fluid_fine_t *fine;
    glass_t g; uint16_t *snap, *ref, *prev;
    uint16_t wbuf[466 * 18];
    /* the look being made (the sim's side) and the drawer's own copy of its inputs */
    fluid_shape_t shp[FLUID_MAX_SHAPES], dshp[FLUID_MAX_SHAPES], full[FLUID_MAX_SHAPES];
    uint8_t *dots, *keep, *still, *ddots, *dkeep, *dstill, *cat, *catline, *word;
    fluid_rgba16_t *ccol, *wcol;       /* the creature's and the word's colours */
    fluid_letter_t L[FLUID_MAX_LETTERS], qL[FLUID_MAX_LETTERS];
    fluid_askview_t *v; fluid_ask_screen_t scr;   /* a question over the chart */
    fluid_draw_opts_t o;
    float now;
    int frame;
    int job;                           /* the new picture read on the job: its slice a frame, the model's us (0: in line) */
    int full_at;                       /* (morph_run: a full repaint asked at this frame of it, the job reading on after it) */
    bool tx;                           /* the kept glass's windows queued to a sender, sent at the frame's end (the device's) */
    int sendp;                         /* (a send this long guessed each frame, us: the device's slow ones) */
    int room;                          /* (the gate's room told each frame, us: the device's frame drawn in one refresh) */
    int hole;                          /* (a frame of the change whose inputs have no shapes, as a reading's teardown: -1 none) */
} morph_rig_t;
/* what a change did (morph_run): its frames, the panel model's ms a frame (mean, worst, over 33), what it showed */
typedef struct {
    int frames, over, unsent, empty_max, empty0, empty1, diff, hard, phase, new_seen;
    int even;                          /* its frames' changes over the new picture: of any frame past 0.6 s changing
                                        * 400 px or more, the most any of 8 bands of rows holds of them against its share of
                                        * the picture's lit px, percent (everywhere at once: 100-390, a creature's lines fading in where it stands; lines shown as read: 540+) */
    int na, nb, fly, spare, born;      /* its grains: old, new; the pairs flying; the old with no pair, the new with none */
    uint32_t read;                     /* the new picture as read (fluid_morph_census_t.read), once whole */
    int work, idle;                    /* the most of the change's own work in a frame that sends but its pass (us); in one
                                        * of its first frames that send nothing (the old picture read, its pairs chosen) */
    int hang, slow, gap0, gapf;        /* frames nothing moved (or the clock stood) between the first move and the dust
                                        * down; its clock slowed; the first new line's frame and the lines' full frame
                                        * against the dust's down (fluid_morph_t.dg) */
    int still;                         /* frames after a full repaint's that showed anything new (none may: the job may read) */
    int late, mixed, queued;           /* windows whose rows changed once queued, sent beside the sender's; those queued */
    double mean, worst;
} morph_res_t;
enum { MK_CAT, MK_SCENE, MK_WORD, MK_ASK };
#define MORPH_BUDGET (FLUID_MORPH_CELLS_US - FLUID_MORPH_SPENT_US)   /* (the change's own work a frame, the host's) */
#define MORPH_STALE 0x1FF8u   /* what the old picture's buffer holds before a change (it is not cleared): never on the glass */
static int s_morph_stale;
static int lit_px(const uint16_t *px, int n) { int c = 0; for (int q = 0; q < n; q++) c += px[q] != 0; return c; }
/* the 32 x 32 px blocks wholly on the glass with no lit pixel, of 1000 */
static int empty_blocks(const uint16_t *g, int S) {
    int n = 0, e = 0;
    for (int by = 0; by + 32 <= S; by += 32) for (int bx = 0; bx + 32 <= S; bx += 32) {
        bool in = true;
        for (int q = 0; q < 4 && in; q++) in = hypotf(bx + (q & 1) * 32 - S / 2.f, by + (q >> 1) * 32 - S / 2.f) < S / 2.f - 2.f;
        if (!in) continue;
        n++;
        bool lit = false;
        for (int y = by; y < by + 32 && !lit; y++) for (int x = bx; x < bx + 32 && !lit; x++) lit = g[y * S + x] != 0;
        e += !lit;
    }
    return n ? e * 1000 / n : 0;
}
/* the drawer's inputs for the look kind (the scene sc's, the creature's, the boot word's, a question over sc) */
static void morph_inputs(morph_rig_t *r, int kind, scene_shapes_t *hd) {
    const int N = r->N;
    fluid_sim_snapshot(r->s, r->fr, false);
    if (kind == MK_SCENE) {
        memcpy(r->ddots, r->dots, (size_t)N); memcpy(r->dkeep, r->keep, (size_t)N); memcpy(r->dstill, r->still, (size_t)N);
        memcpy(r->dshp, r->shp, sizeof(fluid_shape_t) * (size_t)(hd->n > 0 ? hd->n : 0));
        r->o = (fluid_draw_opts_t){ .palette = FLUID_PAL_PASTEL, .edge_mode = FLUID_EDGE_CONTOURS, .font = gc_font5x7, .dots = r->ddots, .scene = true,
                                    .shapes = r->dshp, .nshapes = hd->n, .kdots = r->dkeep, .fine = r->fine, .still = r->dstill };
    } else if (kind == MK_ASK) {                       /* (as mole_fluid.c's ask_compose over a scene) */
        memcpy(r->ddots, r->dots, (size_t)N);
        for (int k = 0; k < N; k++) {
            const uint32_t c = r->s->tcol[k];
            r->fr->col[k] = (fluid_rgba16_t){ (uint16_t)((c & 255) * 257), (uint16_t)((c >> 8 & 255) * 257), (uint16_t)((c >> 16 & 255) * 257), 0 };
        }
        r->scr.t += 1.f / 30.f;
        fluid_askview_frame(r->v, r->s, &r->scr, r->now);
        fluid_askview_compose(r->v, r->ddots, NULL, r->fr->col);
        const int nq = fluid_askview_letters(r->v, r->qL, FLUID_MAX_LETTERS);
        r->o = (fluid_draw_opts_t){ .palette = FLUID_PAL_PASTEL, .edge_mode = FLUID_EDGE_CONTOURS, .font = gc_font5x7 };
        fluid_askview_opts(r->v, &r->o, r->ddots, r->qL, nq);
    } else {
        memcpy(r->ddots, kind == MK_CAT ? r->cat : r->word, (size_t)N);
        memcpy(r->fr->col, kind == MK_CAT ? r->ccol : r->wcol, sizeof(fluid_rgba16_t) * (size_t)N);
        r->o = (fluid_draw_opts_t){ .palette = FLUID_PAL_PASTEL, .edge_mode = FLUID_EDGE_CONTOURS, .font = gc_font5x7, .dots = r->ddots, .scene = true };
    }
    fluid_draw_morph_lines(r->d, kind == MK_CAT ? r->catline : NULL);   /* (the creature's own cells are its lines) */
    r->o.time = r->now;
}
/* The look changes to kind's (the scene's dust asked for at once and built `late` frames after, as the device builds it;
 * its set swaps in before the look is told, which carries FLUID_LOOK_DUST once it shows, as mole_fluid.c); runs to the
 * change's end and a little past (or `stop` frames). Every frame of it: the glass is the change as composed whole
 * (fluid_draw_morph_rect: every grain that moved was sent), its time on the panel model, its grains and lines against
 * the prototype's phases, how much of the glass is empty; at the end, the glass against the look drawn whole. A strip
 * of 8 frames evenly through it to MORPH_DUMP-<name>.ppm (4 x 2, as the prototype's window.strip). */
static void morph_run(morph_rig_t *r, int kind, scene_t *sc, uint32_t look, int late, int stop, const char *name, morph_res_t *res) {
    const int S = r->S;
    scene_shapes_t hd = { r->shp, 0, r->keep, r->still };
    int nl = 0, f0 = -1, nfr = 0, tiles = 0;
    uint32_t key = 0;
    double sum = 0.;
    *res = (morph_res_t){ 0 };
    res->even = 0;
    static long bands[200][9];                         /* (each frame's changes by band of rows; [8]: all) */
    int nband = 0;
    res->na = res->nb = -1;
    res->empty0 = empty_blocks(r->g.px, S);
    if (!fluid_draw_morphing(r->d) && !r->d->m.whole) for (int k = 0; k < S * S; k++) r->snap[k] = MORPH_STALE;
    enum { COLS = 4, TILES = 8 };
    uint16_t *sheet = getenv("MORPH_DUMP") ? calloc((size_t)COLS * S * (TILES / COLS) * S, 2) : NULL;
    for (int f = 0; f < 200; f++, r->frame++) {
        r->now += 1.f / 30.f;
        if (sc) {
            scene_fill(r->s, sc, 1.f, NULL, r->L, &nl, r->dots, &hd);
            if (kind == MK_SCENE && f == 0) { const int n = scene_full_shapes(r->s, sc, 1.f, r->full, &key); fluid_fine_want(r->fine, look, key, true, r->full, n, FLUID_PAL_PASTEL); }
            if (kind == MK_SCENE && f >= late) fluid_fine_work(r->fine, 0);
        }
        /* (the device's order: the sim task's share of the reading while the drawer draws, with the inputs it last handed
         * over; then the handoff) */
        if (r->job) fluid_draw_morph_work(r->d, r->fr, &r->o, r->job);
        if (kind == MK_SCENE && (!fluid_draw_morphing(r->d) || fluid_draw_morph_open(r->d))) fluid_fine_swap(r->fine);
        const bool dust = kind != MK_SCENE || fluid_fine_shown(r->fine, look);
        fluid_draw_morph_look(r->d, kind == MK_SCENE && dust ? look | FLUID_LOOK_DUST : look, r->snap, dust);
        morph_inputs(r, kind, &hd);
        if (f == r->hole || f == r->hole + 1) { r->o.shapes = NULL; r->o.nshapes = 0; }
        fluid_draw_morph_handoff(r->d);
        const bool in = fluid_draw_morphing(r->d);    /* (a frame of the change: its last one draws the look whole) */
        memcpy(r->prev, r->g.px, (size_t)S * S * 2);
        r->g.c = (cost_t){ 0, 0, 0, 0 };
        r->g.mirror = (look & FLUID_LOOK_GLASS) ? r->snap : NULL;   /* (the panel's mirror keeps such a look's glass) */
        if (r->sendp) r->d->m.sendp = r->sendp;
        if (r->room) fluid_draw_morph_room(r->d, r->room);
        fluid_draw_cells(r->d, r->fr, &r->o);
        const bool full = f == r->full_at && in;
        static split_q_t q;
        win_log_t L = { &r->g, 0, 0, { { 0 } }, &q };
        if (r->tx) fluid_draw_glass_sender(split_take, &q);
        fluid_draw_send(r->d, &r->o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, full, r->wbuf, 4096, r->tx ? send_log : send_glass, r->tx ? (void *)&L : (void *)&r->g);
        if (r->tx) {                                    /* (the frame's end: its queued windows sent, each as it was queued) */
            res->queued += q.taken; q.taken = 0;
            split_drain(&q, r->d, &L);
            res->late += q.late + q.over + q.bad + s_bus.breach; res->mixed += L.mixed; q.late = q.over = q.bad = s_bus.breach = 0;
            fluid_draw_glass_sender(NULL, NULL);
        }
        if (full && r->job) {                           /* (the job still at it after the drawer ended the change: as it may be) */
            fluid_draw_morph_work(r->d, r->fr, &r->o, r->job);
            res->still += memcmp(r->prev, r->g.px, (size_t)S * S * 2) != 0;
        }
        const fluid_morph_t *m = &r->d->m;
        const bool on = fluid_draw_morphing(r->d);
        long changed = 0, band[8] = { 0 };
        for (int k = 0; k < S * S; k++) { const bool c = r->g.px[k] != r->prev[k]; changed += c; band[k / S * 8 / S] += c; }
        /* (once the old picture is gone, 0.6 s: while the new one forms, and the frames after the change) */
        if (changed >= 400 && nband < 200 && (!fluid_draw_morphing(r->d) || r->d->m.t >= 0.6f)) { memcpy(bands[nband], band, sizeof(band)); bands[nband++][8] = changed; }
        /* the panel model, the device's (bench1): a window 0.15 ms, a pixel sent from the kept glass 0.055 us, the change's
         * own work (fluid_morph_t.work: its per-item costs fitted to the device); the drawer's cells only in the change's one
         * pass for its new look (MORPH_PASS_MS), else ~1 ms (the touch poll). A frame fits the panel's 31.5 ms step with
         * its handoff when it is under MORPH_FIT_MS. */
        const double ms = in ? (m->pass ? MORPH_PASS_MS : 1.0) + m->work * 1e-3 + r->g.c.wins * 0.15 + r->g.c.px * 0.055e-3 : CELLS_MS + model_ms(&r->g.c);
        for (int k = 0; k < S * S; k++) s_morph_stale += r->g.px[k] == MORPH_STALE;
        if (in) {
            sum += ms; nfr++; res->worst = fmax(res->worst, ms); res->over += ms > MORPH_FIT_MS;
            if (!m->pass && r->g.c.px && m->work > res->work) res->work = m->work;   /* (the change's own work on the drawer, the model's us) */
            if (!m->pass && !r->g.c.px && m->work > res->idle) res->idle = m->work;
            if (getenv("MORPH_TRACE")) printf("    %s f%d t %.3f: %.1f ms (%d windows, %ld px, %ld changed, work %.1f), %d/%d grains, %d pairs\n", name, f, m->t,
                                              ms, r->g.c.wins, r->g.c.px, changed, m->work * 1e-3, m->na, m->nb, m->nfly);
        }
        if (on) {
            fluid_draw_morph_rect(r->d, &(fluid_rect_t){ r->ref, S, 0, 0, S, S });
            int un = 0;
            for (int k = 0; k < S * S; k++) {
                if (r->ref[k] != r->g.px[k] && getenv("UNSENT") && un < 6) { const int x = k % S, y = k / S, c = r->d->m.rowy[y] * r->W + r->d->m.colx[x];
                    printf("    %s f%d at %d,%d: glass %04x composed %04x, cell %d key %04x\n", name, f, x, y, r->g.px[k], r->ref[k], c, r->d->key[c]); }
                un += r->ref[k] != r->g.px[k];
            }
            res->unsent += un != 0;
            if (un && getenv("MORPH_TRACE")) printf("    %s f%d: %d px not as composed\n", name, f, un);
            const int e = empty_blocks(r->g.px, S);
            res->empty_max = e > res->empty_max ? e : res->empty_max;
            /* the phases (fluid_draw.c MP_*): old lines gone by 0.32 s, new lines from 0.85 s only, every pair down by
             * 1.04 s and every new grain with no pair by 1.06 s (the still layers a moment or two late at most when a
             * frame has no room, never early) */
            fluid_morph_census_t c;
            fluid_draw_morph_census(r->d, &c);
            const float t = m->t, eps = 1e-3f, late = 0.1f;
            if (m->nb >= 0) { res->na = m->na; res->nb = m->nb; res->fly = m->nfly; res->spare = m->na - m->nfly; res->born = m->nb - m->nfly; }
            const bool bad = (t >= 0.32f + late && c.old_lines) || (t < 0.85f - eps && c.new_lines) ||
                             (t >= 1.04f + eps && (c.fly || c.wait)) || (m->nb >= 0 && t >= 1.06f + late && c.born_landed != c.born) ||
                             (t <= eps && (c.fly || c.landed || c.born));
            if (bad && getenv("MORPH_TRACE")) printf("    %s f%d t %.3f: off the phases (lines %d %d, pairs %d %d %d, born %d/%d)\n", name, f, t, c.old_lines, c.new_lines,
                                                     c.wait, c.fly, c.landed, c.born_landed, c.born);
            res->phase += bad;
            res->new_seen += c.new_lines > 0;
            if (!res->read) res->read = c.read;
        }
        if (sheet && tiles < TILES && (on || f0 < 0) && m->t + 1e-4f >= tiles / (TILES - 1.f) * m->dur) {
            const int x0 = (tiles % COLS) * S, y0 = (tiles / COLS) * S;
            for (int y = 0; y < S; y++) memcpy(sheet + (size_t)(y0 + y) * COLS * S + x0, r->g.px + (size_t)y * S, (size_t)S * 2);
            tiles++;
        }
        if (f0 < 0 && f > 0 && !on) {
            f0 = f;
            if (sheet && tiles < TILES) {                 /* (its last: the look whole) */
                const int x0 = (tiles % COLS) * S, y0 = (tiles / COLS) * S;
                for (int y = 0; y < S; y++) memcpy(sheet + (size_t)(y0 + y) * COLS * S + x0, r->g.px + (size_t)y * S, (size_t)S * 2);
                tiles++;
            }
        }
        if ((f0 >= 0 && f >= f0 + 3) || (stop && f >= stop)) break;
    }
    res->frames = f0;
    {
        const int *g = r->d->m.dg;
        res->hang = g[FLUID_MD_HANG]; res->slow = g[FLUID_MD_SLOW];
        res->gap0 = g[FLUID_MD_LIN0] ? g[FLUID_MD_LIN0] - g[FLUID_MD_SETTLE] : -1;
        res->gapf = g[FLUID_MD_LINF] - g[FLUID_MD_SETTLE];
    }
    res->mean = nfr ? sum / nfr : 0.;
    if (stop) { free(sheet); return; }
    /* the end: the look itself, drawn whole */
    fluid_draw_rect(r->d, &r->o, &(fluid_rect_t){ r->ref, S, 0, 0, S, S }, true);
    for (int k = 0; k < S * S; k++) {
        if (r->ref[k] != r->g.px[k] && getenv("MORPH_DIFF") && res->diff < 12) printf("    differs at %d,%d: look %04x glass %04x\n", k % S, k / S, r->ref[k], r->g.px[k]);
        res->diff += r->ref[k] != r->g.px[k];
        res->hard += r->ref[k] != r->g.px[k] && (lum(r->ref[k]) > 700 || lum(r->g.px[k]) > 700);
    }
    res->empty1 = empty_blocks(r->g.px, S);
    {   /* (the frames' changes against where the new picture is: everywhere at once, never by rows) */
        long lit[8] = { 0 }, all = 0;
        for (int k = 0; k < S * S; k++) if (r->ref[k]) { lit[k / S * 8 / S]++; all++; }
        for (int f = 0; f < nband; f++) {               /* (a band's share of the frame's changes against its share of the picture) */
            int most = 0;
            for (int b = 0; b < 8; b++) {
                if (lit[b] * 50 < all) continue;
                const int ratio = (int)((double)bands[f][b] / bands[f][8] / ((double)lit[b] / all) * 100.0);
                most = ratio > most ? ratio : most;
            }
            if (most > res->even) res->even = most;
            if (most > 300 && getenv("MORPH_TRACE")) printf("    %s: frame %d's changes (%ld px) %d %% over one band's share of the picture\n", name, f, bands[f][8], most);
        }
    }
    if (sheet) {
        char path[600];
        snprintf(path, sizeof(path), "%s-%s.ppm", getenv("MORPH_DUMP"), name);
        FILE *pf = fopen(path, "wb");
        if (pf) {
            const int w = COLS * S, h = (TILES / COLS) * S;
            fprintf(pf, "P6 %d %d 255\n", w, h);
            for (int q = 0; q < w * h; q++) { const uint16_t le = (uint16_t)(sheet[q] >> 8 | sheet[q] << 8); fputc((le >> 11) * 255 / 31, pf); fputc((le >> 5 & 63) * 255 / 63, pf); fputc((le & 31) * 255 / 31, pf); }
            fclose(pf);
        }
        free(sheet);
    }
}

/* a rig for changes of picture: the boot word, the cat's cells (run from cat, or as `like` has them), a question over the
 * chart; job: the new picture read on the job, its slice a frame (fluid_draw_morph_work, the model's us), else in line */
static morph_rig_t *morph_rig(cpk_t *cat, const morph_rig_t *like, int job) {
    morph_rig_t *r = calloc(1, sizeof(*r));
    const int S = r->S = 466;
    fluid_grid_for_stage(S, &r->W, &r->H);
    const int W = r->W, H = r->H, N = r->N = W * H;
    r->s = fluid_sim_create(W, H, S); r->s->p.still = true;
    r->fr = fluid_frame_create(W, H); r->d = fluid_draw_create(W, H, S, S); r->fine = fluid_fine_create(W, H, S, S);
    r->g = (glass_t){ calloc((size_t)S * S, 2), S, { 0, 0, 0, 0 }, r->d, NULL };
    r->snap = calloc((size_t)S * S, 2); r->ref = calloc((size_t)S * S, 2); r->prev = calloc((size_t)S * S, 2);
    r->dots = calloc((size_t)N, 6); r->keep = r->dots + N; r->still = r->keep + N; r->ddots = r->still + N; r->dkeep = r->ddots + N; r->dstill = r->dkeep + N;
    r->cat = calloc((size_t)N, 3); r->catline = r->cat + N; r->word = r->catline + N;
    r->ccol = calloc((size_t)N, sizeof(fluid_rgba16_t)); r->wcol = calloc((size_t)N, sizeof(fluid_rgba16_t));
    r->job = job; r->full_at = -1; r->hole = -1; r->tx = job > 0;
    fluid_draw_morph_job(r->d, job > 0);
    /* the boot word, in the fluid's dots (pale), and the cat (packages/cat: its own sim, a few seconds of life, then its
     * cells as the device shows them: its own cells its lines, the floor and the motes its dust) */
    fluid_text_t *tx = fluid_text_create(r->s);
    fluid_text_set_glyphs(tx, r->s, "molty.uno");
    memcpy(r->word, tx->dots, (size_t)N);
    fluid_text_destroy(tx);
    for (int k = 0; k < N; k++) if (r->word[k]) r->wcol[k] = (fluid_rgba16_t){ 52000, 50000, 62000, 0 };
    if (like) { memcpy(r->cat, like->cat, (size_t)N * 2); memcpy(r->ccol, like->ccol, sizeof(fluid_rgba16_t) * (size_t)N); }
    else {
        fluid_sim_t *cs = fluid_sim_create(W, H, S);
        cpk_prepare(cat, cs);
        for (int f = 0; f < 90; f++) { cpk_frame(cat, cs, 1 / 30.f, 1); cs->p.still = true; fluid_sim_step(cs, 1 / 30.f); fluid_sim_color(cs, 1 / 30.f); cpk_paint(cat, cs); }
        const uint8_t *cd; const uint32_t *cc;
        cpk_cells(cat, &cd, &cc);
        const uint8_t *own = cpk_cells_creature(cat);
        for (int k = 0; k < N; k++) {
            r->cat[k] = cd ? cd[k] : 0; r->catline[k] = own && own[k];
            const uint32_t c = cc ? cc[k] : 0;
            r->ccol[k] = (fluid_rgba16_t){ (uint16_t)((c >> 16 & 255) * 257), (uint16_t)((c >> 8 & 255) * 257), (uint16_t)((c & 255) * 257), 0 };
        }
        fluid_sim_destroy(cs);
    }
    {   /* a question over the chart: install a package? */
        fluid_ask_q_t q;
        memset(&q, 0, sizeof(q));
        strcpy(q.title, "install astrology?"); strcpy(q.detail, "a new package for molty");
        q.n = 2; strcpy(q.label[0], "install"); strcpy(q.label[1], "not now");
        q.rgb[0] = 0x3ccf96; q.rgb[1] = 0xff6b6b;
        r->v = fluid_askview_create(r->s, &q);
        fluid_ask_follow(&r->scr, true, 1, -1, q.n);
        fluid_askview_start(r->v, r->s);
    }
    return r;
}
static void morph_rig_free(morph_rig_t *r) {
    free(r->g.px); free(r->snap); free(r->ref); free(r->prev); free(r->dots); free(r->cat); free(r->ccol); free(r->wcol);
    fluid_askview_destroy(r->v);
    fluid_fine_destroy(r->fine); fluid_draw_destroy(r->d); fluid_frame_destroy(r->fr); fluid_sim_destroy(r->s);
    free(r);
}

/* every change of picture, its new picture read in line (job 0: the drawer's frames, the web) or on the job (its slice a
 * frame, us: the device's sim task) */
/* the scene sc up (look, its change over), a tap's wave on it for 30 frames in the device's order, the kept glass's windows
 * through the sender (the fake one: split_take / split_drain): windows it could not take, rows changed once queued,
 * frames whose glass is not the look, windows sent */
static void morph_tap(morph_rig_t *r, scene_t *sc, uint32_t look, int *bad, int *late, int *off, int *sent) {
    scene_shapes_t hd = { r->shp, 0, r->keep, r->still };
    static split_q_t q;
    int nl;
    fluid_sim_impulse(r->s, 0.15f, -0.3f, 1.6f, 0.09f);
    for (int f = 0; f < 30; f++, r->frame++) {
        r->now += 1.f / 30.f;
        scene_fill(r->s, sc, 1.f, NULL, r->L, &nl, r->dots, &hd);
        fluid_sim_step(r->s, 1.f / 60.f); fluid_sim_step(r->s, 1.f / 60.f);
        if (r->job) fluid_draw_morph_work(r->d, r->fr, &r->o, r->job);
        fluid_fine_swap(r->fine);
        fluid_draw_morph_look(r->d, look | FLUID_LOOK_DUST, r->snap, true);
        morph_inputs(r, MK_SCENE, &hd);
        fluid_draw_morph_handoff(r->d);
        fluid_draw_cells(r->d, r->fr, &r->o);
        win_log_t L = { &r->g, 0, 0, { { 0 } }, &q };
        fluid_draw_glass_sender(split_take, &q);
        fluid_draw_send(r->d, &r->o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, false, r->wbuf, 4096, send_log, &L);
        *sent += q.n;
        split_drain(&q, r->d, &L);
        fluid_draw_glass_sender(NULL, NULL);
        *bad += q.bad; *late += q.late + q.over + L.mixed + s_bus.breach; q.bad = q.late = q.over = s_bus.breach = 0;
        *off += glass_off(r->d, &r->o, r->g.px, r->S) != 0;
    }
}

/* ── the sender's hand-over (fluid_tx.h) with a sender that stops answering for a while, as on bench 12 (every frame then
 * paid the full bound): only the first wait runs to its bound; every frame after it goes on at once while nothing goes
 * to the panel; once the sender answers (every frame end, none lost) it is back. ── */
typedef struct { int queued, answered, stuck_until, now, waited, frame; } tx_fake_t;
static bool fake_end(void *ctx, int ms) { tx_fake_t *t = ctx; (void)ms; t->queued++; return true; }
static bool fake_done(void *ctx, int ms) {
    tx_fake_t *t = ctx;
    if (t->frame >= t->stuck_until) t->answered = t->queued;   /* (it answers every frame end it has, at once) */
    if (t->answered > 0) { t->answered--; t->queued--; return true; }
    t->waited += ms;
    return false;
}
static void test_tx(void) {
    tx_fake_t k = { 0, 0, 5, 0, 0, 0 };
    fluid_tx_t t = { 0 };
    const fluid_tx_io_t io = { fake_end, fake_done, &k };
    int idle = 0, lates = 0, most = 0;
    for (k.frame = 0; k.frame < 40; k.frame++) {        /* (frames 0..4 a sender that does not answer; each frame queues windows when it may) */
        const int w0 = k.waited;
        bool late = false;
        const bool ok = fluid_tx_idle(&t) || fluid_tx_drain(&t, &io, 300, &late);
        if (ok) t.n += 12;                              /* (the frame's windows) */
        if (t.n) { const bool e = fluid_tx_drain(&t, &io, 300, &late); idle += e && ok; }
        lates += late;
        most = k.waited - w0 > most ? k.waited - w0 : most;
    }
    printf("  the sender's hand-over, a sender stuck for 5 frames: waited %d ms in all (most %d a frame), late %d time(s), %d frames sent after\n", k.waited, most, lates, idle);
    CHECK(k.waited == 300 && lates == 1 && !t.late && fluid_tx_idle(&t) && idle >= 34 && k.queued == 0,
          "sender stuck a while: one bound waited in all (%d ms), logged once (%d), every frame end answered (%d left), back for the frames after (%d)", k.waited, lates, k.queued, idle);
}

/* every allocation of the drawer's (fluid_draw.c, FLUID_TEST_ALLOC): none may ask for more than the biggest it has
 * (the glass's drawing cache, 466 x 466 x 2 B): bench 33's change asked for 8.4 MB, a count gone wrong */
#define DRAW_ALLOC_MAX (466 * 466 * 2)
static size_t s_alloc_most;
void *fluid_test_alloc(void *p, size_t n, bool zero) {
    if (n > s_alloc_most) s_alloc_most = n;
    if (n > DRAW_ALLOC_MAX) return NULL;               /* (as the device: it fails, the change is cut) */
    return p ? realloc(p, n) : zero ? calloc(1, n) : malloc(n);
}
#define MORPH_GATE_ROOM_US 12500  /* (the device's room till the pulse a change's frame is sent on: bench 31, cells <= 12.5 ms) */
#define MORPH_JOB_OVER_US 1000   /* (a step past the slice's budget at most: a cell row of a list, a row of specks read) */
static void test_morph(const char *dir, const char *catp, int job) {
    char path[512]; size_t len; const char *err;
    snprintf(path, sizeof(path), "%s/astrology.json", dir);
    char *buf = read_file(path, &len);
    pkg_t *p = buf ? pkg_parse(buf, len, &err) : NULL;
    free(buf);
    snprintf(path, sizeof(path), "%s/astrology-commands.jsonl", dir);
    char *cmds = p ? read_file(path, &len) : NULL;
    scene_t *chart = NULL;
    for (char *line = cmds ? strtok(cmds, "\n") : NULL; line && !chart; line = strtok(NULL, "\n")) {
        cJSON *c = cJSON_Parse(line), *a = cJSON_GetObjectItem(c, "args");
        const char *layout = cJSON_GetStringValue(cJSON_GetObjectItem(a, "layout"));
        if (layout && !strcmp(layout, "chart")) chart = scene_build(p, layout, cJSON_GetObjectItem(a, "params"), 100.f, &err);
        cJSON_Delete(c);
    }
    free(cmds);
    scene_t *up = chart ? scene_open_build(chart, spot_named(chart, "leo"), &err) : NULL;
    char *cj = catp ? read_file(catp, &len) : NULL;
    cpk_t *cat = cj ? cpk_create(cj, len, 3, &err) : NULL;
    free(cj);
    CHECK(chart && up && cat, "the chart, Leo up close and the cat (%s) build", catp ? catp : "no cat.json");
    if (!chart || !up || !cat) { cpk_destroy(cat); scene_free(up); scene_free(chart); pkg_unref(p); return; }
    morph_rig_t *r = morph_rig(cat, NULL, job);
    const int S = r->S, N = r->N;
    morph_res_t res;
    if (job) printf("  the same, its new picture read on the job (the sim task's share: %.1f ms of the model a frame):\n", job * 1e-3);
    /* the boot word first (nothing to change from: it is there at once, kept by the panel's mirror), then the prototype's
     * five changes, a question over the chart and gone */
    morph_run(r, MK_WORD, NULL, FLUID_LOOK_GLASS, 0, 0, "start", &res);
    const int word_lit = lit_px(r->g.px, S * S);
    CHECK(word_lit > 1500 && res.frames <= 3, "the boot word, with nothing before it: at once (%d px lit)", word_lit);
    /* the prototype's grains (dissolve-proto/screens.js: its layer generator joins touching specks into one grain, the
     * device keeps each speck: the glass stays exact) and its pairs flying (fitted to its panel model) */
    static const struct { const char *name; int kind, scene; uint32_t look; int late; int na, nb, fly, empty; } RUN[] = {
        { "boot-cat", MK_CAT, 0, FLUID_LOOK_KEEP | 1, 0, 588, 1382, 588, 703 }, { "cat-chart", MK_SCENE, 1, FLUID_LOOK_KEEP | 100, 16, 1382, 11843, 1382, 125 },
        { "chart-leo", MK_SCENE, 2, FLUID_LOOK_KEEP | 101, 14, 11843, 6253, 2246, 22 }, { "leo-chart", MK_SCENE, 1, FLUID_LOOK_KEEP | 102, 0, 6253, 11843, 2441, 22 },
        { "chart-ask", MK_ASK, 1, FLUID_LOOK_KEEP | 0x20000001u, 0, 0, 0, 0, 0 }, { "ask-chart", MK_SCENE, 1, FLUID_LOOK_KEEP | 102, 0, 0, 0, 0, 0 },
        { "chart-cat", MK_CAT, 0, FLUID_LOOK_KEEP | 1, 0, 11843, 1382, 1382, 214 } };
    printf("  a change of picture, as dust (panel model, cells %.0f ms; ms a frame: mean / worst / over 33):\n", CELLS_MS);
    for (int q = 0; q < (int)(sizeof(RUN) / sizeof(RUN[0])); q++) {
        r->d->m.jover = 0;
        morph_run(r, RUN[q].kind, RUN[q].scene == 1 ? chart : RUN[q].scene == 2 ? up : NULL, RUN[q].look, RUN[q].late, 0, RUN[q].name, &res);
        /* (on the sim task: no slice of the job past its budget by more than a step's worth in the model's work, every
         * step's looks counted: one whole pass over the new lines against the chart's grains was ~30 ms of the device in
         * one go, the new grains' list made whole (bench 30) 25-40 ms: a frame lost each) */
        if (job > 0) CHECK(r->d->m.jover <= MORPH_JOB_OVER_US, "%s: the job's slices at most %d us past their budget (%d)", RUN[q].name, MORPH_JOB_OVER_US, r->d->m.jover);
        printf("    %-9s %4.0f ms  %5.1f / %5.1f / %d   grains %5d -> %5d (prototype %5d -> %5d), flying %4d (prototype %4d), fading %5d, coming %5d;"
               " empty %4.1f %% (calm %4.1f, %4.1f), phases off %d, not composed %d; own work at most %.1f ms (%.1f before it moves); middle still %d frames,"
               " slowed %d; first new line %+d, lines full %+d frames from the dust down\n", RUN[q].name, res.frames * 1000.0 / 30.0, res.mean, res.worst, res.over,
               res.na, res.nb, RUN[q].na, RUN[q].nb, res.fly, RUN[q].fly, res.spare, res.born, res.empty_max / 10.0, res.empty0 / 10.0, res.empty1 / 10.0, res.phase, res.unsent,
               res.work * 1e-3, res.idle * 1e-3, res.hang, res.slow, res.gap0, res.gapf);
        /* (the prototype's rule, its fit to the panel model: as many pairs as the smaller picture has, at most
         * FLUID_MORPH_FLY and FLUID_MORPH_FLY_PX px of grains, 2 x 2 px grains 4 px) */
        const int K = res.na < res.nb ? res.na : res.nb, most = K < FLUID_MORPH_FLY ? K : FLUID_MORPH_FLY, least = most < FLUID_MORPH_LAND_PX / 4 ? most : FLUID_MORPH_LAND_PX / 4;   /* (2 x 2 px grains: 4, landing or leaving) */
        CHECK(res.nb >= 0 && res.fly <= most && res.fly >= least && res.spare == res.na - res.fly && res.born == res.nb - res.fly,
              "%s: the pairs fly as many as the smaller picture has, at most %d and %d px (%d of %d -> %d); the rest fade (%d) or come up (%d)", RUN[q].name,
              FLUID_MORPH_FLY, FLUID_MORPH_FLY_PX, res.fly, res.na, res.nb, res.spare, res.born);
        /* (a scene's dust is read before its lines now: its specks on dim line pixels are grains too till the lines come,
         * ~1000 on the chart: within 20 %) */
        if (RUN[q].na)
            CHECK(abs(res.na - RUN[q].na) * 100 <= RUN[q].na * 10 && abs(res.nb - RUN[q].nb) * 100 <= RUN[q].nb * 20,
                  "%s: the pictures' grains as the prototype's, within 10 and 20 %% (%d, %d against %d, %d)", RUN[q].name, res.na, res.nb, RUN[q].na, RUN[q].nb);
        /* (a question's letters form in the fluid after its first frames: its picture as read has no lines, they come whole at
         * the end) */
        CHECK(res.phase == 0 && (res.new_seen > 0 || RUN[q].kind == MK_ASK), "%s: its phases (old lines gone by 0.32 s, the new from 0.85 s, every grain down by 1.06 s; %d frames off)",
              RUN[q].name, res.phase);
        CHECK(res.frames >= 33 && res.frames <= 45 && res.over == 0,
              "%s: one change of 1.5 s at most, every frame inside the panel's (%d frames, worst %.1f ms of %.0f)", RUN[q].name, res.frames, res.worst, MORPH_FIT_MS);
        CHECK(res.hang <= 3, "%s: the dissolve flows into the gathering: at most 3 frames nothing moves between them (%d)", RUN[q].name, res.hang);
        CHECK((res.gap0 <= -2 || RUN[q].kind == MK_ASK) && res.gapf <= 4,
              "%s: the dust converges into the lines: the new lines coming in 2 frames or more before the last grain is down (%+d), full 4 frames after at most (%+d)",
              RUN[q].name, res.gap0, res.gapf);
        CHECK(res.unsent == 0, "%s: every frame of it all sent, each grain the frame it moves (%d frames not)", RUN[q].name, res.unsent);
        if (job)                                        /* (the drawer's own: its reading on the job, the device's) */
            CHECK(res.work <= MORPH_BUDGET + MORPH_BUDGET / 10 && res.idle <= FLUID_MORPH_IDLE_US + FLUID_MORPH_IDLE_US / 10,
                  "%s: no frame's own work over the change's budget, %.1f ms (most %.1f ms; %.1f in its first frames, of %.1f)",
                  RUN[q].name, MORPH_BUDGET * 1e-3, res.work * 1e-3, res.idle * 1e-3, FLUID_MORPH_IDLE_US * 1e-3);
        if (r->tx)                                      /* (the device's sender after it: one sender at a time, no queued row changed before it went) */
            CHECK(res.late == 0 && res.mixed == 0, "%s: with the sender, no window's rows changed once queued, none sent beside it (%d, %d)", RUN[q].name, res.late, res.mixed);
        CHECK(res.even <= 450, "%s: everywhere at once, never by rows: no band of rows takes over 4.5 times its share of the picture in a frame's changes (most %d %%)",
              RUN[q].name, res.even);
        {   /* (the prototype's own sparse cloud: its emptiest frame, dissolve-proto's render on the same blocks; the device's
             * flies fewer pairs, FLUID_MORPH_FLY: a lighter cloud, as Juan allowed, up to 8 % more of it empty) */
            const int calm = res.empty0 > res.empty1 ? res.empty0 : res.empty1, most = (calm > RUN[q].empty ? calm : RUN[q].empty) + 80;
            CHECK(res.empty_max <= most, "%s: never a large empty area: at most %.1f %% of the glass's blocks empty (calm %.1f and %.1f, the prototype %.1f)",
                  RUN[q].name, res.empty_max / 10.0, res.empty0 / 10.0, res.empty1 / 10.0, RUN[q].empty / 10.0);
        }
        CHECK(s_morph_stale == 0, "%s: only the picture kept shows, never what its buffer held before (%d px)", RUN[q].name, s_morph_stale);
        s_morph_stale = 0;
        CHECK(res.diff == 0, "%s: at its end the glass is the new look to the pixel (%d px differ, %d of them bright)", RUN[q].name, res.diff, res.hard);
    }
    if (r->tx) {   /* (bench 12's way to a stalled panel, on a rig of its own: the boot word, the cat, the chart, a tap on it) */
        morph_rig_t *t = morph_rig(cat, r, job);
        morph_run(t, MK_WORD, NULL, FLUID_LOOK_GLASS, 0, 0, "start", &res);
        morph_run(t, MK_CAT, NULL, FLUID_LOOK_KEEP | 1, 0, 0, "boot-cat", &res);
        morph_run(t, MK_SCENE, chart, FLUID_LOOK_KEEP | 100, 16, 0, "cat-chart", &res);
        int bad = 0, late = 0, off = 0, sent = 0;
        morph_tap(t, chart, FLUID_LOOK_KEEP | 100, &bad, &late, &off, &sent);
        CHECK(bad == 0 && late == 0 && off == 0 && sent > 20, "boot word, cat, chart, then a tap's wave on it through the sender: every window one it takes (%d not), "
              "none changed once queued (%d), the glass the look each frame (%d off; %d windows)", bad, late, off, sent);
        morph_rig_free(t);
    }
    if (job) {  /* at the device's costs (about twice the model's at the peaks: bench 10's cells): the frame's budget holds every
                 * frame but the pass (what moves never waits, the still ones step late, everywhere at once), the change
                 * still ends on the look whole, in its phases, never by rows */
        static const struct { const char *name; int kind, scene; uint32_t look; } DEV[] = {
            { "cat-chart", MK_SCENE, 1, FLUID_LOOK_KEEP | 900 }, { "chart-leo", MK_SCENE, 2, FLUID_LOOK_KEEP | 901 },
            { "leo-chart", MK_SCENE, 1, FLUID_LOOK_KEEP | 902 }, { "chart-cat", MK_CAT, 0, FLUID_LOOK_KEEP | 1 } };
        fluid_draw_morph_costs(2.2f);
        for (int q = 0; q < (int)(sizeof(DEV) / sizeof(DEV[0])); q++) {
            morph_run(r, DEV[q].kind, DEV[q].scene == 1 ? chart : DEV[q].scene == 2 ? up : NULL, DEV[q].look, 0, 0, DEV[q].name, &res);
            printf("    at the device's costs: %-9s %4.0f ms, own work at most %.1f ms (%.1f before it moves), %d %% of a band; middle still %d frames, slowed %d;"
                   " first new line %+d, lines full %+d frames from the dust down\n", DEV[q].name,
                   res.frames * 1000.0 / 30.0, res.work * 1e-3, res.idle * 1e-3, res.even, res.hang, res.slow, res.gap0, res.gapf);
            CHECK(res.work <= MORPH_BUDGET + MORPH_BUDGET / 10 && res.idle <= FLUID_MORPH_IDLE_US + FLUID_MORPH_IDLE_US / 10 && res.diff == 0 && res.phase == 0 &&
                  res.unsent == 0 && res.even <= 450 && res.frames <= 45 && res.hang <= 3 && res.gap0 <= -2 && res.gapf <= 4 && s_morph_stale == 0,
                  "%s at the device's costs: no frame over the budget (most %.1f ms), the look whole at its end (%d px), in its phases (%d off), never by rows (%d %%), %d frames,"
                  " middle still %d, lines %+d / %+d from the dust down", DEV[q].name, res.work * 1e-3, res.diff, res.phase, res.even, res.frames, res.hang, res.gap0, res.gapf);
            s_morph_stale = 0;
        }
        /* the gate (bench 31): a change's frame is drawn in one refresh and sent on the next pulse, so its cells have the
         * room till that pulse (fluid_draw_morph_room): at the device's costs, the new dust late as the device builds it
         * (cat-chart paired ~f24, its lines ~f28: what moves, the still layers due and what came in all at once), no
         * moving frame's own work past that room less FLUID_MORPH_GATE_US; the change as approved */
        r->room = MORPH_GATE_ROOM_US;
        fluid_draw_morph_costs(1.6f);                  /* (bench 31's movers and lists: ~1.6 x the model's at their peaks) */
        static const struct { const char *name; int kind, scene; uint32_t look; int late; } GATE[] = {
            { "cat-chart", MK_SCENE, 1, FLUID_LOOK_KEEP | 910, 16 }, { "chart-leo", MK_SCENE, 2, FLUID_LOOK_KEEP | 911, 14 },
            { "leo-chart", MK_SCENE, 1, FLUID_LOOK_KEEP | 912, 12 } };
        for (int q = 0; q < (int)(sizeof(GATE) / sizeof(GATE[0])); q++) {
            morph_run(r, GATE[q].kind, GATE[q].scene == 1 ? chart : GATE[q].scene == 2 ? up : NULL, GATE[q].look, GATE[q].late, 0, GATE[q].name, &res);
            const int most = MORPH_GATE_ROOM_US - FLUID_MORPH_GATE_US;
            printf("    on the gate (%.1f ms), at the device's costs: %-9s %4.0f ms, own work at most %.1f ms of %.1f; middle still %d, slowed %d; lines %+d / %+d\n",
                   MORPH_GATE_ROOM_US * 1e-3, GATE[q].name, res.frames * 1000.0 / 30.0, res.work * 1e-3, most * 1e-3, res.hang, res.slow, res.gap0, res.gapf);
            CHECK(res.work <= most + most / 25 && res.diff == 0 && res.phase == 0 && res.unsent == 0 && res.even <= 450 && res.frames <= 45 && res.hang <= 3 &&
                  res.gap0 <= -2 && res.gapf <= 4 && s_morph_stale == 0,
                  "%s on the gate: every moving frame's own work inside its room (%.1f of %.1f ms), the look whole (%d px), in its phases (%d off), %d frames,"
                  " middle still %d, lines %+d / %+d from the dust down", GATE[q].name, res.work * 1e-3, most * 1e-3, res.diff, res.phase, res.frames, res.hang, res.gap0, res.gapf);
            s_morph_stale = 0;
        }
        r->room = 0;
        fluid_draw_morph_costs(0.f);
        /* bench 33's 8.4 MB: the new grains' list made a few cell rows a call, so a call ends where its first pass does and the
         * next starts at the second's first cell (it made the list's room again over counts already turned into places,
         * ~110 x the grains): no list past its picture (bound), no allocation over the drawer's biggest, the change whole */
        {
            const int ROWS[] = { r->H, 1, 7, r->H / 2 };
            int most = 0, bad = 0;
            for (int q = 0; q < 4; q++) {
                const uint32_t lk = (uint32_t)(980 + 2 * q);
                morph_run(r, MK_SCENE, up, FLUID_LOOK_KEEP | lk, 0, 0, "to-leo", &res);
                fluid_draw_morph_walk_rows(ROWS[q]); s_alloc_most = 0; r->d->m.bound = NULL;
                morph_run(r, MK_SCENE, chart, FLUID_LOOK_KEEP | (lk + 1), 0, 0, "leo-chart", &res);
                fluid_draw_morph_walk_rows(0);
                most = (int)s_alloc_most > most ? (int)s_alloc_most : most;
                bad += res.diff != 0 || res.phase != 0 || res.frames > 45 || r->d->m.bound != NULL || s_morph_stale != 0;
                s_morph_stale = 0;
            }
            printf("    the new picture's lists a few rows a call (a call ending with a pass): biggest allocation %d B, %d off\n", most, bad);
            CHECK(most <= DRAW_ALLOC_MAX && bad == 0, "the new picture's lists made a few rows a call, a call ending where a pass does: no allocation over %d B (%d), "
                  "none past its picture, each change whole (%d off)", DRAW_ALLOC_MAX, most, bad);
        }
        /* bench 33: a change into the chart while its inputs change under the job (two frames with no shapes, as the
         * reading over a sign goes) anywhere, on the drawer's pass too, the job resumed in slices of a few sizes: the pass
         * waits for a scene's inputs again (bench 34: the glass not the new look at its end otherwise), the job reads one kind
         * of picture (it read a scene's dust as a word's lines, then took the dust as read: counts from cells never read,
         * an 8.4 MB list asked for), no count past the picture's (bound), no allocation over the drawer's biggest block,
         * and the change ends whole */
        {
            static const int HOLE[] = { 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20 }, JOB[] = { 6000, 9000, 15000 };
            int most = 0, cut = 0, bad = 0, runs = 0;
            for (int h = 0; h < 13; h++) for (int jb = 0; jb < 3; jb++) {
                const uint32_t lk = (uint32_t)(940 + 2 * (h * 3 + jb));
                morph_run(r, MK_SCENE, up, FLUID_LOOK_KEEP | lk, 0, 0, "to-leo", &res);
                const int job0 = r->job, cuts0 = r->d->m.cuts;
                s_alloc_most = 0; r->job = JOB[jb]; r->hole = HOLE[h];
                morph_run(r, MK_SCENE, chart, FLUID_LOOK_KEEP | (lk + 1), 0, 0, "leo-chart", &res);
                r->job = job0; r->hole = -1;
                most = (int)s_alloc_most > most ? (int)s_alloc_most : most;
                cut += r->d->m.cuts != cuts0 || res.frames > 45;
                bad += res.diff != 0 || res.phase != 0 || s_morph_stale != 0 || r->d->m.bound != NULL;
                s_morph_stale = 0; runs++;
            }
            printf("    a reading's inputs changing under the job (%d runs: frame of it x slice): biggest allocation %d B, %d cut or long, %d off\n", runs, most, cut, bad);
            CHECK(most <= DRAW_ALLOC_MAX && cut == 0 && bad == 0, "a change into the chart with its inputs changing under the job: no allocation over %d B (%d), none cut "
                  "or over 1.5 s (%d), each whole at its end and in its phases (%d off)", DRAW_ALLOC_MAX, most, cut, bad);
        }
        /* a slow send (bench 21: a whole glass 17 to 19 ms with core 1 busy): the change's own work shrinks to what the
         * frame leaves beside it (FLUID_MORPH_FRAME_US: the still layers step aside, the movers never), and the change
         * still holds its pace */
        r->sendp = 17500;
        fluid_draw_morph_costs(1.6f);                  /* (bench 21's movers: ~1.6 x the model's at their peaks) */
        for (int q = 0; q < 2; q++) {
            morph_run(r, DEV[q + 1].kind, DEV[q + 1].scene == 1 ? chart : DEV[q + 1].scene == 2 ? up : NULL, DEV[q + 1].look + 10, 0, 0, DEV[q + 1].name, &res);
            const int most = FLUID_MORPH_FRAME_US - r->sendp - FLUID_MORPH_SPENT_US;
            printf("    a slow send (%.1f ms): %-9s %4.0f ms, own work at most %.1f ms of %.1f; middle still %d frames\n", r->sendp * 1e-3, DEV[q + 1].name,
                   res.frames * 1000.0 / 30.0, res.work * 1e-3, most * 1e-3, res.hang);
            CHECK(res.work <= most + most / 10 && res.frames <= 45 && res.hang <= 3 && res.diff == 0 && res.even <= 450,
                  "%s with a slow send: its own work inside what the send leaves (%.1f of %.1f ms), %d frames, middle still %d, the look whole (%d px)",
                  DEV[q + 1].name, res.work * 1e-3, most * 1e-3, res.frames, res.hang, res.diff);
        }
        fluid_draw_morph_costs(0.f);
        r->sendp = 0;
    }
    {   /* a change in the middle of one (Leo opened from the chart, the chart again 12 frames in): from the glass as it is
         * then, no cut, every frame sent, the chart whole at the end */
        morph_res_t a, b;
        morph_run(r, MK_SCENE, up, FLUID_LOOK_KEEP | 201, 0, 12, "leo-cut", &a);
        const int mid = fluid_draw_morphing(r->d);
        morph_run(r, MK_SCENE, chart, FLUID_LOOK_KEEP | 202, 0, 0, "cut-chart", &b);
        printf("    interrupted: worst %.1f ms then %.1f ms a frame; %.0f ms after; empty %.1f %%, not composed %d\n", a.worst, b.worst, b.frames * 1000.0 / 30.0, b.empty_max / 10.0, a.unsent + b.unsent);
        CHECK(s_morph_stale == 0, "interrupted: only the picture kept shows (%d px)", s_morph_stale);
        CHECK(mid && a.over + b.over == 0 && a.unsent + b.unsent == 0, "a change in the middle of one keeps the frame (worst %.1f, %.1f ms)", a.worst, b.worst);
        CHECK(b.diff == 0 && b.frames <= 70 && !fluid_draw_morphing(r->d), "... from the glass as it was, to the chart whole (%d px differ)", b.diff);
    }
    {   /* the creature, then the first scene after a boot (numbered 1, as the creature's own number), then the creature
         * again: each a change of picture (mole_fluid.c's looks: fluid_look_of; a scene's number once equal to the
         * creature's look read as "the same picture", no change at all) */
        morph_res_t a, b;
        const uint32_t cat = fluid_look_of(FLUID_LOOK_CREATURE, 1u, true), first = fluid_look_of(FLUID_LOOK_SCENE, 1u, false);
        morph_run(r, MK_CAT, NULL, cat, 0, 0, "to-cat", &a);
        morph_run(r, MK_SCENE, chart, first, 0, 0, "cat-first", &a);
        morph_run(r, MK_CAT, NULL, cat, 0, 0, "first-cat", &b);
        CHECK(((cat ^ first) & ~FLUID_LOOK_DUST) && a.frames >= 36 && b.frames >= 36 && a.diff == 0 && b.diff == 0,
              "the creature, the first scene (numbered 1), the creature again: each a change of picture (%d, %d frames)", a.frames, b.frames);
    }
    {   /* every wait at its limit at once (the device's slow reads): what is missing is made in that frame and the change
         * goes on as approved, everywhere at once, never a cut or a repaint by rows; the same with its dust not in */
        morph_res_t a, b, c, e;
        fluid_draw_morph_bounds(1, 1, 0);
        morph_run(r, MK_SCENE, up, FLUID_LOOK_KEEP | 500, 0, 0, "bound-leo", &a);
        morph_run(r, MK_SCENE, chart, FLUID_LOOK_KEEP | 501, 0, 0, "bound-chart", &b);
        morph_run(r, MK_SCENE, up, FLUID_LOOK_KEEP | 502, 1000, 0, "bound-nodust", &c);
        fluid_draw_morph_bounds(0, 0, 0);
        /* (the chart's lines read as slowly as the device reads them, a few cells a frame: still never seen as read) */
        fluid_draw_morph_bounds(0, 0, 24);
        morph_run(r, MK_SCENE, chart, FLUID_LOOK_KEEP | 503, 0, 0, "slow-chart", &e);
        fluid_draw_morph_bounds(0, 0, 0);
        printf("    its lines read slowly: %.0f ms, a band's most of a frame's changes against its share %d %%\n", e.frames * 1000.0 / 30.0, e.even);
        CHECK(e.even <= 450 && e.diff == 0 && e.frames >= 36 && e.frames <= 36 + 18 + 60 + 9, "its lines read slowly, a few cells a frame: they come in everywhere at once, never as read (%d %%, %d frames)", e.even, e.frames);
        printf("    every wait at its limit: %.0f, %.0f, %.0f ms; a band's most of a frame's changes against its share: %d, %d, %d %%\n", a.frames * 1000.0 / 30.0,
               b.frames * 1000.0 / 30.0, c.frames * 1000.0 / 30.0, a.even, b.even, c.even);
        CHECK(a.even <= 450 && b.even <= 450 && c.even <= 450 && a.diff == 0 && b.diff == 0 && c.diff == 0 && a.frames >= 36 && b.frames >= 36 && c.frames >= 36,
              "every wait at its limit: the change goes on everywhere at once, to the look whole, never by rows (%d, %d, %d %%)", a.even, b.even, c.even);
        s_morph_stale = 0;
    }
    {   /* the new picture's dust not in by the cloud (its build starved on the device): the clock waits for it briefly
         * (FLUID_MORPH_DUST_*, FLUID_MORPH_HOLD_*) and the change ends on time without it; the dust comes after it as a
         * change of its own (FLUID_LOOK_DUST), the lines staying, its grains coming up near their spots */
        morph_res_t a, b;
        const int most = 45 + FLUID_MORPH_HOLD_N + 3;   /* (1.5 s, the slower dissolve of a dust built late, and its stand) */
        morph_run(r, MK_SCENE, up, FLUID_LOOK_KEEP | 400, 1000, 0, "no-dust", &a);
        printf("    its dust late: the change %.0f ms (at most %.0f), worst %.1f ms\n", a.frames * 1000.0 / 30.0, most * 1000.0 / 30.0, a.worst);
        CHECK(a.frames >= 36 && a.frames <= most && a.unsent == 0 && a.diff == 0 && a.over == 0,
              "its dust never in: the change still ends on time, to the look without it (%d frames of at most %d, %d px differ)", a.frames, most, a.diff);
        morph_run(r, MK_SCENE, up, FLUID_LOOK_KEEP | 400, 0, 0, "dust-after", &b);
        printf("    the dust after it: %.0f ms, worst %.1f ms, %d grains coming\n", b.frames * 1000.0 / 30.0, b.worst, b.born);
        CHECK(b.frames >= 10 && b.frames <= 36 + (job > 0) && b.born > 1000 && b.fly == 0 && b.unsent == 0 && b.diff == 0 && b.over == 0 && s_morph_stale == 0,
              "... then it comes as a change of its own: its grains come up near their spots, the rest stays (%d frames, %d grains, %d px differ; read on the job: a frame more,"
              " the hand-over)", b.frames, b.born, b.diff);
        s_morph_stale = 0;
    }
    if (job) {  /* the job slow (still reading when each is cut short): a change replaced while the job reads it (the reading
                 * goes back to the drawer at the handoff), and one the drawer ends (a full repaint) while the job reads on in
                 * its slice after: that frame shows nothing new, the look comes whole the next, its memory freed at the
                 * handoff (an ASan build of this test is the check that nothing reads it after) */
        morph_res_t a, b, c;
        r->job = 400;
        morph_run(r, MK_SCENE, chart, FLUID_LOOK_KEEP | 700, 0, 12, "job-replaced", &a);
        const bool mid = fluid_draw_morphing(r->d) && r->d->m.jmm;
        morph_run(r, MK_SCENE, up, FLUID_LOOK_KEEP | 701, 0, 0, "replacing", &b);
        CHECK(mid && a.unsent + b.unsent == 0 && b.diff == 0 && s_morph_stale == 0 && !r->d->m.jmm && !fluid_draw_morphing(r->d),
              "a change replaced while the job reads it: the next goes on from the glass as it was, to the look whole (%d px differ)", b.diff);
        r->full_at = 12;
        morph_run(r, MK_SCENE, chart, FLUID_LOOK_KEEP | 702, 0, 0, "job-ended", &c);
        r->full_at = -1;
        CHECK(c.still == 0 && c.diff == 0 && c.frames >= 12 && c.frames <= 14 && s_morph_stale == 0 && !r->d->m.jmm && !r->d->m.jfree,
              "a change the drawer ends while the job reads: nothing drawn till the handoff, then the look whole (%d frames, %d px differ)", c.frames, c.diff);
        r->job = job;
        s_morph_stale = 0;
        /* the new picture as the job reads it is the one read in line (all of it read at the first chance either way: the
         * same frame's dust, the same look), change after change from the same start */
        morph_rig_t *ra = morph_rig(NULL, r, 0), *rb = morph_rig(NULL, r, job);
        static const struct { const char *name; int kind, scene; uint32_t look; } SAME[] = {
            { "boot-cat", MK_CAT, 0, FLUID_LOOK_KEEP | 1 }, { "cat-chart", MK_SCENE, 1, FLUID_LOOK_KEEP | 100 }, { "chart-leo", MK_SCENE, 2, FLUID_LOOK_KEEP | 101 },
            { "leo-chart", MK_SCENE, 1, FLUID_LOOK_KEEP | 102 }, { "chart-cat", MK_CAT, 0, FLUID_LOOK_KEEP | 1 } };
        fluid_draw_morph_bounds(0, 0, -1);
        morph_run(ra, MK_WORD, NULL, FLUID_LOOK_GLASS, 0, 0, "start", &a);
        morph_run(rb, MK_WORD, NULL, FLUID_LOOK_GLASS, 0, 0, "start", &b);
        int same = 0, whole = 0;
        for (int q = 0; q < (int)(sizeof(SAME) / sizeof(SAME[0])); q++) {
            scene_t *sc = SAME[q].scene == 1 ? chart : SAME[q].scene == 2 ? up : NULL;
            morph_run(ra, SAME[q].kind, sc, SAME[q].look, 0, 0, SAME[q].name, &a);
            morph_run(rb, SAME[q].kind, sc, SAME[q].look, 0, 0, SAME[q].name, &b);
            same += a.read && a.read == b.read && a.nb == b.nb;
            whole += a.diff == 0 && b.diff == 0 && b.unsent == 0 && b.phase == 0;
            if (a.read != b.read) printf("    %s: read in line %08x (%d grains), on the job %08x (%d)\n", SAME[q].name, a.read, a.nb, b.read, b.nb);
        }
        fluid_draw_morph_bounds(0, 0, 0);
        CHECK(same == 5 && whole == 5, "the new picture read on the job is the one read in line (%d of 5 the same, %d whole)", same, whole);
        morph_rig_free(ra); morph_rig_free(rb);
        s_morph_stale = 0;
    }
    if (!job) { /* a reading over a layout a tap just opened (Leo's own, at that layout's subtitle place), driven as the device
         * drives it: after each word no pixel of an earlier word is left on the glass, and what is round the word is
         * the scene, dimmed, and nothing else (EXPERIMENT: on the dust kind it was made and checked on, 8cc8414's) */
        fluid_fine_dust_kind(FINE_DUST_DOTS_ILACE);
        morph_run(r, MK_SCENE, up, FLUID_LOOK_KEEP | 300, 0, 0, "read-leo", &res);
        const int leo = spot_named(chart, "leo");
        char text[PKG_SAY_MAX + 1];
        snprintf(text, sizeof(text), "%.*s", (int)chart->items[leo].nstr, chart->say + chart->items[leo].str);
        struct { int off, len, orp; } tok[PKG_SAY_MAX / 2 + 1];
        int nt = 0;
        for (int i = 0, n = (int)strlen(text); i < n;) {
            while (i < n && text[i] == ' ') i++;
            const int st = i;
            while (i < n && text[i] != ' ') i++;
            if (i > st) { tok[nt].off = st; tok[nt].len = i - st > FLUID_WORD_LETTERS ? FLUID_WORD_LETTERS : i - st; tok[nt].orp = gc_rsvp_compute_orp(text + st, tok[nt].len).focal; nt++; }
        }
        float ub[4] = { 0 };                           /* the backdrop: one box for the whole reading (mole_fluid.c) */
        for (int w = 0; w < nt; w++) {
            float b[4];
            const scene_caption_t c = { text + tok[w].off, tok[w].len, tok[w].orp };
            scene_caption_box(&c, up->caption_y, S, b);
            if (!w) memcpy(ub, b, sizeof(b));
            ub[0] = fminf(ub[0], b[0]); ub[1] = fmaxf(ub[1], b[1]); ub[2] = fminf(ub[2], b[2]); ub[3] = fmaxf(ub[3], b[3]);
        }
        scene_shapes_t hd = { r->shp, 0, r->keep, r->still };
        uint16_t *plain = calloc((size_t)S * S, 2);    /* the scene with no word, drawn whole (its drawing, undimmed) */
        int nl = 0;
        scene_fill(r->s, up, 1.f, NULL, r->L, &nl, r->dots, &hd);
        { static int16_t pb[FLUID_MAX_SHAPES][4];
          for (int q = 0; q < hd.n; q++) fluid_shape_box(&r->shp[q], 0, 0, pb[q]);
          for (int y = 0; y < S; y++) fluid_shapes_row(r->shp, hd.n, (const int16_t (*)[4])pb, 0, 0, FLUID_PAL_PASTEL, NULL, 0.f, plain + (size_t)y * S, 0, y, 0, S); }
        int left = 0, wrong = 0, shown = 0;
        double rworst = 0.;
        int rover = 0;
        for (int w = 0; w <= nt; w++) {
            const scene_caption_t cap = w < nt ? (scene_caption_t){ text + tok[w].off, tok[w].len, tok[w].orp } : (scene_caption_t){ NULL, 0, -1 };
            const int frames = w < nt ? (text[tok[w].off + tok[w].len - 1] == '.' ? 16 : text[tok[w].off + tok[w].len - 1] == ',' ? 12 : 8) : 40;
            for (int f = 0; f < frames; f++, r->frame++) {
                r->now += 1.f / 30.f;
                scene_fill(r->s, up, 1.f, &cap, r->L, &nl, r->dots, &hd);
                if (!f && cap.n) for (int q = 0; q < nl; q++) if (!r->L[q].mask && !r->L[q].rgb && r->L[q].ch != ' ')
                    fluid_sim_impulse(r->s, (r->L[q].x + r->L[q].w * 0.5f) / S * 2.f - 1.f, (r->L[q].y + r->L[q].h * 0.5f) / S * 2.f - 1.f, SCENE_CAPTION_RIPPLE, SCENE_CAPTION_RIPPLE_R);
                fluid_sim_step(r->s, 1.f / 30.f);
                fluid_draw_morph_look(r->d, FLUID_LOOK_KEEP | 300, r->snap, true);
                {
                    fluid_sim_snapshot(r->s, r->fr, false);
                    memcpy(r->ddots, r->dots, (size_t)N); memcpy(r->dkeep, r->keep, (size_t)N); memcpy(r->dstill, r->still, (size_t)N);
                    memcpy(r->dshp, r->shp, sizeof(fluid_shape_t) * (size_t)(hd.n > 0 ? hd.n : 0));
                    fluid_fine_swap(r->fine);
                    fluid_fine_caption_box(r->fine, w < nt ? ub : NULL);
                    r->o.nshapes = hd.n; r->o.cap_word = cap.n ? (unsigned)(w + 1) : 0; r->o.time = r->now;
                }
                r->g.c = (cost_t){ 0, 0, 0, 0 };
                fluid_draw_cells(r->d, r->fr, &r->o);
                fluid_draw_send(r->d, &r->o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, false, r->wbuf, 4096, send_glass, &r->g);
                const double ms = panel_ms(&r->g.c) + CELLS_MS;
                rworst = fmax(rworst, ms); rover += ms > 33.0;
                if (getenv("FINE_TRACE")) printf("    read w%d f%d: %.1f ms (%d windows, %ld px)\n", w, f, ms, r->g.c.wins, r->g.c.px);
                if (getenv("READ_DUMP") && atoi(getenv("READ_DUMP")) == w && f == frames - 1) dump_ppm(getenv("MORPH_OUT"), r->g.px, S);
                if (f != frames - 1 || (w == nt && f < 39)) continue;
                /* the word's own letters' boxes; round them, on the backdrop: a lit pixel is the scene's own drawing
                 * (there in `plain` too) or a remnant */
                int here = 0;
                for (int y = (int)ub[2] - 14; y < (int)ub[3] + 14; y++) for (int x = (int)ub[0] - 14; x < (int)ub[1] + 14; x++) {
                    if (x < 0 || y < 0 || x >= S || y >= S) continue;
                    bool in_word = false;
                    for (int q = 0; q < hd.n && !in_word; q++) {
                        const fluid_shape_t *sh = &r->shp[q];
                        if (sh->tag != FLUID_TAG_CAPTION) continue;
                        in_word = fabsf(x + 0.5f - sh->x) <= sh->w * 0.5f + 1.5f && fabsf(y + 0.5f - sh->y) <= sh->h * 0.5f + 1.5f;
                    }
                    const int g = lum(r->g.px[y * S + x]), p0 = lum(plain[y * S + x]);
                    if (in_word) { here += g > 300; continue; }   /* (its accented letter is orange: dimmer than white) */
                    left += g > 420 && p0 < 150;                 /* bright, where the scene has nothing: an earlier word's */
                    wrong += p0 > 500 && g > p0;                 /* the scene's drawing under the backdrop: never brighter than it is */
                }
                shown += here > 12 * cap.n || w == nt;
            }
        }
        printf("  a reading over Leo just opened (panel model): worst %.1f ms a frame, %d over 33\n", rworst, rover);
        CHECK(shown == nt + 1, "the reading over an opened layout: each word shows (%d of %d)", shown, nt + 1);
        CHECK(left == 0 && wrong == 0, "... and after each word nothing of an earlier one is left (%d px), the scene round it only dimmed (%d px brighter)", left, wrong);
        CHECK(rworst < 33.0 && rover == 0, "... inside the frame (worst %.1f ms, %d over)", rworst, rover);
        free(plain);
        fluid_fine_dust_kind(FINE_DUST_SPECKS);
    }
    morph_rig_free(r);
    cpk_destroy(cat);
    scene_free(up); scene_free(chart);
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
    CHECK(fluid_fine_dust_kind_get() == FINE_DUST_SPECKS && fluid_fine_hard_density_get() == FINE_HARD_PCT,
          "scenes' dust: the fine specks by default (the Lab's switch; kind 3's hard dots at %d %%)", FINE_HARD_PCT);
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
    test_scene_dust(argv[1]);
    test_ride(argv[1]);
    test_split(argv[1]);
    test_retime(argv[1]);
    test_tx();
    test_caption_kept(argv[1]);
    test_fine_hard(argv[1]);
    test_open(argv[1]);
    test_morph(argv[1], argc > 3 ? argv[3] : NULL, 0);
    test_morph(argv[1], argc > 3 ? argv[3] : NULL, 15000);   /* (the device's: its sim task reads, ~15 ms a frame) */
    bench(argc > 2 ? argv[2] : "");
    CHECK(P->refs == 1, "every scene gave its package reference back (%d)", P->refs);
    pkg_unref(P);
    printf("scene_test: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
