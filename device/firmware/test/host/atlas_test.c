/*
 * SPDX-License-Identifier: MIT
 *
 * atlas_test.c: the sky atlas on the firmware's own screen (the web module's native build: web_molty.c included whole,
 * its statics are this file's), fed what the molty-atlas skill sends for Berlin on 2026-11-27 at 22:00 (and with no
 * place), with packages/atlas.
 *   models     the prototype's models: the same dots every time, their counts, inside the drawing, a galaxy's long axis
 *              at its catalogue angle and its tilt
 *   package    the atlas package parses; an atlas is the beat's sibling (one a layout, a centre needs one)
 *   load       every kind loads (316 objects), a bad load is refused whole and leaves what was there
 *   levels     the kinds on the rim (30 degrees), the middle opens one, its objects (15 degrees, five turned fast, a
 *              firmer tick passing the first), the kind above goes back up, the middle turns a card and back; `ring`
 *              events name the object; no place: no where face
 *   cards      every object, both faces: its name, its numbers and what shows it on the glass, never more shapes than
 *              the screen holds with the hold ring, a picture's or a model's dots on the lattice
 *   close      the reject button and a held finger close it; with the screen gone its store is freed
 *   glass      device mode (only the windows the device sends reach the panel): turning through the cards, no pixel of
 *              the glass wrong more than ATLAS_GLASS_LAG frames running, and still, exact
 *   cost       the 1.75C's frame model on a card and turning
 *   renders    with a fourth argument, the screens as PNG there
 *
 *   atlas_test <repo root> <berlin.jsonl> <noplace.jsonl> [render dir]
 */
#include "../../web/web_molty.c"

#include <time.h>

static int s_pass, s_fail;
#define ATLAS_GLASS_LAG 3              /* frames (a tenth of a second) */
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define DT 16667u
#define S 466
static void frames(int n) { for (int i = 0; i < n; i++) wm_frame(DT); }
static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "atlas_test: cannot read %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    b[n] = 0; *len = (size_t)n;
    fclose(f);
    return b;
}
static void touch_at(int down, float x, float y) { wm_touch(down, (int)lrintf((x + 1.f) * 0.5f * S), (int)lrintf((1.f - y) * 0.5f * S)); }
static void lift(void) { touch_at(0, 0.f, 0.f); frames(2); }
static void tap(float x, float y) { touch_at(1, x, y); frames(2); lift(); }
static void drag(float from, int deg, int per) {
    touch_at(1, 0.93f * cosf(from * 0.017453293f), 0.93f * sinf(from * 0.017453293f)); frames(1);
    for (int q = 1; q <= deg; q++) { const float an = (from - q) * 0.017453293f; touch_at(1, 0.93f * cosf(an), 0.93f * sinf(an)); frames(per); }
}
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
/* the skill's commands, a JSON line each: atlas.load and atlas.show to the atlas, scene.play to the screen */
static int send_all(const char *path, int *nload) {
    size_t len;
    char *b = slurp(path, &len), *line = b, *nl;
    int bad = 0;
    *nload = 0;
    for (; line && *line; line = nl ? nl + 1 : NULL) {
        nl = strchr(line, '\n');
        if (nl) *nl = 0;
        cJSON *c = cJSON_Parse(line);
        const char *name = str(cJSON_GetObjectItem(c, "name"));
        char *args = cJSON_PrintUnformatted(cJSON_GetObjectItem(c, "args"));
        if (!strncmp(name, "atlas.", 6)) { bad += wm_atlas(name, args) != 0; *nload += !strcmp(name, "atlas.load"); }
        else if (!strcmp(name, "scene.play")) bad += wm_scene_op("atlas", "{}", 600, 0) != 0;
        free(args); cJSON_Delete(c);
    }
    free(b);
    return bad;
}
static int where_level(void) { int l, k, i, f; atlas_where(&l, &k, &i, &f); return l; }
static int where_index(void) { int l, k, i, f; atlas_where(&l, &k, &i, &f); return i; }
static int where_kind(void) { int l, k, i, f; atlas_where(&l, &k, &i, &f); return k; }
static int where_face(void) { int l, k, i, f; atlas_where(&l, &k, &i, &f); return f; }
static bool has_text(const scene_t *sc, const char *s) {
    for (int q = sc->nfixed; q < sc->nitems; q++) { const scene_item_t *it = &sc->items[q]; if (it->k == PK_TEXT && it->nstr == strlen(s) && !memcmp(sc->chars + it->str, s, it->nstr)) return true; }
    return false;
}
static int count_kind(const scene_t *sc, int k, int *pts) {
    int n = 0;
    if (pts) *pts = 0;
    for (int q = sc->nfixed; q < sc->nitems; q++) if (sc->items[q].k == k) { n++; if (pts) *pts += sc->items[q].npts; }
    return n;
}

/* ── the models ─────────────────────────────────────────────────────────────────────────────────────────────────── */
static void test_models(void) {
    static atlas_pt_t a[ATLAS_DOTS], b[ATLAS_DOTS];
    const int n1 = atlas_model(AM_GALAXY, 0, 0.392f, 35.f, 177.8f, atlas_seed("m31"), a, ATLAS_DOTS);
    const int n2 = atlas_model(AM_GALAXY, 0, 0.392f, 35.f, 177.8f, atlas_seed("m31"), b, ATLAS_DOTS);
    CHECK(n1 == 420 && n1 == n2 && !memcmp(a, b, sizeof(atlas_pt_t) * (size_t)n1), "a galaxy's model: 420 dots, the same every time");
    CHECK(atlas_seed("m31") == 52772u && atlas_seed("ngc5139") != atlas_seed("m13"), "the prototype's seeds (m31: %u)", atlas_seed("m31"));
    /* its long axis: the dots' spread is widest along north through east by its angle (35 degrees: up and to the left) */
    double sxx = 0, syy = 0, sxy = 0;
    for (int q = 0; q < n1; q++) { sxx += a[q].x * a[q].x; syy += a[q].y * a[q].y; sxy += a[q].x * a[q].y; }
    const double th = 0.5 * atan2(2 * sxy, sxx - syy), pa = fmod(atan2(-cos(th), sin(th)) * 57.29578 + 360., 180.);   /* from north (up) through east (left) */
    const double l1 = 0.5 * (sxx + syy) + sqrt(0.25 * (sxx - syy) * (sxx - syy) + sxy * sxy), l2 = 0.5 * (sxx + syy) - sqrt(0.25 * (sxx - syy) * (sxx - syy) + sxy * sxy);
    CHECK(fabs(pa - 35.) < 8., "its long axis at its angle, 35 degrees from north through east (%.1f)", pa);
    CHECK(sqrt(l2 / l1) < 0.6, "tilted: the short axis %.2f of the long", sqrt(l2 / l1));
    static const struct { int type, n; float ratio, size; const char *id; } M[] = {
        { AM_GLOBULAR, 360, 0.f, 0.f, "m13" }, { AM_NEBULA, 420, 0.67f, 0.f, "m8" }, { AM_PLANETARY, 301, 1.f, 0.f, "m27" }, { AM_OPEN, 90, 0.f, 95.f, "m44" }, { AM_GALAXY, 420, 0.f, 0.f, "m87" },
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++) {
        const int n = atlas_model(M[i].type, 1, M[i].ratio, 0.f, M[i].size, atlas_seed(M[i].id), a, ATLAS_DOTS);
        float far = 0.f;
        for (int q = 0; q < n; q++) far = fmaxf(far, hypotf(a[q].x, a[q].y));
        if (n != M[i].n || far > ATLAS_FIT * 1.001f) { bad++; printf("    %s: %d dots (%d), out to %.3f\n", M[i].id, n, M[i].n, far); }
    }
    CHECK(!bad, "a globular, a nebula, a planetary, an open cluster, an elliptical: their counts, inside the drawing");
    printf("  models  a galaxy at its angle and tilt; the counts of every model, inside the drawing\n");
}

/* ── the package ────────────────────────────────────────────────────────────────────────────────────────────────── */
static void test_package(const char *json, size_t len) {
    const char *err = NULL;
    pkg_t *p = pkg_parse(json, len, &err);
    CHECK(p && p->nlayouts == 1 && p->layouts[0].items[0].k == PK_ATLAS, "the atlas package parses (%s)", err ? err : "");
    pkg_unref(p);
    static const char *const BAD[] = {
        "{\"v\":\"molty-pkg/1\",\"id\":\"x\",\"name\":\"X\",\"version\":\"1.0.0\",\"kind\":\"scenes\",\"layouts\":{\"a\":{\"dur\":0,\"items\":[{\"k\":\"atlas\",\"r\":1},{\"k\":\"wave\",\"r\":0.5}]}}}",
        "{\"v\":\"molty-pkg/1\",\"id\":\"x\",\"name\":\"X\",\"version\":\"1.0.0\",\"kind\":\"scenes\",\"layouts\":{\"a\":{\"dur\":0,\"items\":[{\"k\":\"atlas\"}]}}}",
        "{\"v\":\"molty-pkg/1\",\"id\":\"x\",\"name\":\"X\",\"version\":\"1.0.0\",\"kind\":\"scenes\",\"layouts\":{\"a\":{\"dur\":0,\"items\":[{\"k\":\"field\",\"r\":1}]}}}",
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) { pkg_t *b = pkg_parse(BAD[i], strlen(BAD[i]), &err); CHECK(!b, "bad package %zu refused", i); pkg_unref(b); }
}

/* ── loads ──────────────────────────────────────────────────────────────────────────────────────────────────────── */
static void test_load(const char *berlin) {
    int loads;
    CHECK(send_all(berlin, &loads) == 0 && loads == 10, "the skill's commands for Berlin: ten loads, the screen, where to start (%d loads)", loads);
    frames(60);
    CHECK(s_sp.cur && atlas_item(s_sp.cur) >= 0 && atlas_active(), "the atlas is up");
    int total = 0;
    static const char *const K[] = { "planet", "constellation", "star", "double", "open", "globular", "nebula", "planetary", "galaxy", "shower" };
    for (int k = 0; k < 10; k++) total += atlas_kind_index(K[k]) >= 0;
    CHECK(total == 10, "every kind loaded");
    const size_t held = atlas_bytes();
    CHECK(held > 20000 && held < 200000, "in PSRAM: %zu bytes", held);
    /* bad loads, refused whole: what was there stays */
    static const char *const BAD[] = {
        "{\"kind\":\"galaxy\",\"objects\":[{\"id\":\"m31\",\"name\":\"M31\",\"facts\":\"\",\"see\":\"eye\",\"d\":{\"k\":\"dots\",\"p\":\"@@@@\"}}]}",
        "{\"kind\":\"galaxy\",\"objects\":[{\"id\":\"m31\",\"name\":\"M31\",\"facts\":\"\",\"see\":\"glasses\",\"d\":{\"k\":\"model\",\"m\":\"galaxy\"}}]}",
        "{\"kind\":\"nowhere\",\"objects\":[{\"id\":\"m31\",\"name\":\"M31\",\"facts\":\"\",\"see\":\"eye\",\"d\":{\"k\":\"model\",\"m\":\"galaxy\"}}]}",
        "{\"kind\":\"galaxy\",\"objects\":[{\"id\":\"m31\",\"name\":\"A NAME FAR TOO LONG FOR THE GLASS\",\"facts\":\"\",\"see\":\"eye\",\"d\":{\"k\":\"model\",\"m\":\"galaxy\"}}]}",
        "{\"kind\":\"galaxy\",\"objects\":[{\"id\":\"m31\",\"name\":\"M31\",\"facts\":\"\",\"see\":\"eye\",\"d\":{\"k\":\"stars\",\"p\":\"AAAA\",\"l\":\"AAU=\"}}]}",
        "{\"kinds\":[]}", "{}",
    };
    int refused = 0;
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) refused += wm_atlas("atlas.load", BAD[i]) != 0;
    CHECK(refused == (int)(sizeof BAD / sizeof BAD[0]) && atlas_bytes() == held, "%d bad loads refused, the atlas as it was", refused);
    CHECK(wm_atlas("atlas.show", "{\"kind\":\"galaxy\",\"index\":999,\"level\":\"objects\"}") != 0 && wm_atlas("atlas.show", "{\"kind\":\"comets\"}") != 0, "a show of nothing refused");
    printf("  load    ten kinds, %zu bytes in PSRAM; bad loads refused whole\n", held);
}

/* ── the levels ─────────────────────────────────────────────────────────────────────────────────────────────────── */
static void test_levels(void) {
    wm_atlas("atlas.show", "{\"kind\":\"constellation\",\"level\":\"kinds\"}");
    frames(30);
    CHECK(where_level() == ATLAS_AT_KINDS && where_kind() == atlas_kind_index("constellation"), "at the kinds, on the constellations");
    events_named("x", NULL);
    drag(90.f, 34, 2); lift();
    cJSON *last = NULL;
    CHECK(where_kind() == atlas_kind_index("star") && events_named("ring", &last) == 1 && !strcmp(str(cJSON_GetObjectItem(cJSON_GetObjectItem(last, "data"), "spot")), "kind")
          && !strcmp(str(cJSON_GetObjectItem(cJSON_GetObjectItem(last, "data"), "id")), "star"), "30 degrees: the next kind, bright stars; ring {spot: kind, id: star}");
    touch_at(1, 0.93f * cosf(56.f * 0.017453293f), 0.93f * sinf(56.f * 0.017453293f)); frames(1);
    for (int q = 1; q <= 34; q++) { const float an = (56.f + q) * 0.017453293f; touch_at(1, 0.93f * cosf(an), 0.93f * sinf(an)); frames(2); }
    lift();
    CHECK(where_kind() == atlas_kind_index("constellation"), "back: the constellations");
    tap(0.f, 0.f); frames(4);
    CHECK(where_level() == ATLAS_AT_OBJECTS && where_index() == 0, "the middle: its objects, the best known first");
    scene_t *sc = s_sp.cur;
    CHECK(sc && has_text(sc, "ORION") && has_text(sc, "CONSTELLATIONS") && count_kind(sc, PK_DISC, NULL) > 20 && count_kind(sc, PK_LINE, NULL) > 15, "Orion's card: its stars, its lines, its name, its kind above");
    events_named("x", NULL);
    drag(90.f, 17, 2); lift();
    CHECK(where_index() == 1 && events_named("ring", &last) == 1 && !strcmp(str(cJSON_GetObjectItem(cJSON_GetObjectItem(last, "data"), "spot")), "object"), "15 degrees: the next object, ring {spot: object}");
    wheel_turn(1, true);
    CHECK(where_index() == 6, "a fast click: five on (%d)", where_index());
    wheel_turn(-1, true); wheel_turn(-1, false);
    CHECK(where_index() == 0 && atomic_load(&s_tick_plan) == WAVE_LAND_TICK, "back to the first: the firmer tick");
    wheel_turn(-1, false);
    CHECK(where_index() == 87, "one more back: round to the last (%d)", where_index());
    wheel_turn(1, false);
    CHECK(where_index() == 0 && atomic_load(&s_tick_plan) == WAVE_LAND_TICK, "and on past it to the first: the firmer tick");
    frames(10);
    tap(0.f, 0.1f); frames(4);
    CHECK(where_face() == ATLAS_WHERE, "the middle: the card turns to where it is");
    sc = s_sp.cur;
    CHECK(sc && has_text(sc, "SOUTH-EAST 30 UP") && has_text(sc, "HIGHEST 01:10") && has_text(sc, "N"), "where: south-east, 30 up, highest at 01:10, the dome's north");
    wm_button(MOLTY_BTN_ACCEPT, 1); frames(3); wm_button(MOLTY_BTN_ACCEPT, 0); frames(4);
    CHECK(where_face() == ATLAS_DRAWING, "the accept button is the middle: the drawing again");
    tap(0.f, ATLAS_TOP_Y); frames(4);
    CHECK(where_level() == ATLAS_AT_KINDS, "the kind above: back up to the kinds");
    cJSON_Delete(last);
    printf("  levels  the kinds, a kind's objects (a click, five fast, round past the first), a card's two faces, back up; ring events\n");
}

/* ── every card, both faces ─────────────────────────────────────────────────────────────────────────────────────── */
static void test_cards(void) {
    static const char *const K[] = { "planet", "constellation", "star", "double", "open", "globular", "nebula", "planetary", "galaxy", "shower" };
    int worst = 0, cards = 0, no_words = 0, fields = 0, field_pts = 0;
    char worst_at[64] = "";
    for (int k = 0; k < 10; k++) {
        for (int i = 0; i < ATLAS_OBJECTS; i++) {
            char a[160];
            snprintf(a, sizeof a, "{\"kind\":\"%s\",\"index\":%d,\"level\":\"objects\",\"face\":\"%s\"}", K[k], i, "drawing");
            if (wm_atlas("atlas.show", a)) break;
            for (int face = 0; face < 2; face++) {
                if (face) { snprintf(a, sizeof a, "{\"kind\":\"%s\",\"index\":%d,\"level\":\"objects\",\"face\":\"where\"}", K[k], i); wm_atlas("atlas.show", a); }
                frames(3);
                const scene_t *sc = s_sp.cur;
                cards++;
                const int texts = count_kind(sc, PK_TEXT, NULL);
                if (texts < (face ? 7 : 4)) { no_words++; if (no_words < 5) printf("    %s %d face %d: %d words\n", K[k], i, face, texts); }
                int pts;
                const int f = count_kind(sc, PK_FIELD, &pts);
                if (f) { fields++; field_pts += pts; }
                if (s_nshp < 0 || s_nshp > worst) { worst = s_nshp < 0 ? 999 : s_nshp; snprintf(worst_at, sizeof worst_at, "%s %d face %d", K[k], i, face); }
            }
        }
    }
    CHECK(cards == 316 * 2, "every object's card, both faces (%d)", cards);
    CHECK(!no_words, "each its kind, name, numbers and what shows it (where: direction and height, when, the dome's N E S W)");
    CHECK(worst >= 0 && worst + SCENE_HOLD_DOTS <= FLUID_MAX_SHAPES, "never more shapes than the screen holds with the hold ring: at most %d (%s)", worst, worst_at);
    CHECK(fields > 100, "pictures and models as dots on the lattice: %d cards, %d dots", fields, field_pts);
    printf("  cards   %d cards: at most %d shapes (%s); %d of them dots on the lattice\n", cards, worst, worst_at, fields);
}

/* ── no place: no where face ────────────────────────────────────────────────────────────────────────────────────── */
static void test_noplace(const char *noplace) {
    int loads;
    CHECK(send_all(noplace, &loads) == 0 && loads == 10, "the skill's commands with no place");
    frames(30);
    wm_atlas("atlas.show", "{\"kind\":\"constellation\",\"index\":0,\"level\":\"objects\",\"face\":\"where\"}");
    frames(4);
    CHECK(where_face() == ATLAS_DRAWING, "no place: asked for where, the drawing");
    tap(0.f, 0.1f); frames(4);
    CHECK(where_face() == ATLAS_DRAWING && s_sp.cur && has_text(s_sp.cur, "EYE"), "the middle turns nothing; what shows it, with no up or down");
}

/* ── closing ────────────────────────────────────────────────────────────────────────────────────────────────────── */
static void test_close(const char *berlin) {
    int loads;
    events_named("x", NULL);
    wm_button(MOLTY_BTN_REJECT, 1); frames(3); wm_button(MOLTY_BTN_REJECT, 0); frames(10);
    CHECK(!s_sp.cur && events_named("scene", NULL) == 1, "the reject button closes it");
    frames((int)(ATLAS_ORPHAN_S * 60) + 30);
    CHECK(atlas_bytes() == 0 && !atlas_active(), "the screen gone: its store freed");
    send_all(berlin, &loads); frames(30);
    touch_at(1, -0.3f, -0.85f * 0.f + 0.3f); frames(70); lift(); frames(10);
    CHECK(!s_sp.cur, "a finger held a second: the same");
    send_all(berlin, &loads); frames(60);
}

/* ── the glass ──────────────────────────────────────────────────────────────────────────────────────────────────── */
static int glass_lag(int nframes, void (*each)(int f)) {
    static uint16_t ref[S * S], age[S * S];
    memset(age, 0, sizeof age);
    int oldest = 0;
    for (int f = 0; f < nframes; f++) {
        if (each) each(f);
        frames(1);
        if (fluid_draw_morphing(s_draw)) continue;
        const fluid_rect_t t = { ref, S, 0, 0, S, S };
        fluid_draw_rect(s_draw, &s_dopts, &t, true);
        fluid_draw_overlay(s_draw, &t, s_dopts.palette, &s_kept.ov);
        for (int q = 0; q < S * S; q++) { age[q] = s_rgba_of[ref[q]] != s_rgba[q] ? age[q] + 1 : 0; if (age[q] > oldest) oldest = age[q]; }
    }
    return oldest;
}
static void click_every_12(int f) { if (f % 12 == 0) wheel_turn(1, false); }
static void click_every_4(int f) { if (f % 4 == 0) wheel_turn(1, false); }
static void face_every_30(int f) { if (f % 30 == 0) atlas_local_centre(); }
static void test_glass(void) {
    wm_device(1);
    wm_atlas("atlas.show", "{\"kind\":\"constellation\",\"index\":0,\"level\":\"objects\"}");
    frames(90);
    const int a = glass_lag(240, click_every_12);
    wm_atlas("atlas.show", "{\"kind\":\"galaxy\",\"index\":0,\"level\":\"objects\"}"); frames(60);
    const int b = glass_lag(240, click_every_4);
    wm_atlas("atlas.show", "{\"kind\":\"nebula\",\"index\":0,\"level\":\"objects\"}"); frames(60);
    const int c = glass_lag(240, face_every_30);
    frames(90);
    const int still = wm_glass_off();
    wm_device(0);
    const int worst = a > b ? (a > c ? a : c) : (b > c ? b : c);
    if (worst > ATLAS_GLASS_LAG) { const int *at = wm_glass_off_at(); printf("    worst %d frames (constellations %d, galaxies fast %d, faces %d), at %d,%d %d,%d\n", worst, a, b, c, at[0], at[1], at[2], at[3]); }
    CHECK(worst <= ATLAS_GLASS_LAG, "turning through cards, slow and fast, and turning a card: no pixel wrong more than %d frames running (at most %d)", ATLAS_GLASS_LAG, worst);
    CHECK(still == 0, "held still: the glass is the whole picture (%d px off)", still);
    printf("  glass   cards turned slow, fast, faces turned in device mode: the panel at most %d frames behind, none once still\n", worst);
}

/* ── cost ───────────────────────────────────────────────────────────────────────────────────────────────────────── */
static double device_ms(bool turn, double *worst) {
    wm_device(1);
    double sum = 0.;
    int n = 0;
    *worst = 0.;
    for (int q = 0; q < 240; q++) {
        const float an = (90.f - q * 1.f) * 0.017453293f;
        if (turn) touch_at(1, 0.93f * cosf(an), 0.93f * sinf(an));
        wm_frame(DT);
        const float ms = wm_frame_info()[FI_FRAME_MS];
        if (q >= 30) { sum += ms; n++; if (ms > *worst) *worst = ms; }
    }
    if (turn) touch_at(0, 0.f, 0.f);
    wm_device(0);
    return sum / n;
}
static void test_cost(void) {
    double w0, w1, w2, w3;
    wm_atlas("atlas.show", "{\"kind\":\"constellation\",\"index\":0,\"level\":\"objects\"}"); frames(60);
    const double orion = device_ms(false, &w0);
    wm_atlas("atlas.show", "{\"kind\":\"nebula\",\"index\":0,\"level\":\"objects\"}"); frames(60);
    const double m42 = device_ms(false, &w1);
    wm_atlas("atlas.show", "{\"kind\":\"galaxy\",\"index\":0,\"level\":\"objects\"}"); frames(60);
    const double turn = device_ms(true, &w2);
    wm_atlas("atlas.show", "{\"kind\":\"constellation\",\"index\":0,\"level\":\"objects\",\"face\":\"where\"}"); frames(60);
    const double where = device_ms(false, &w3);
    if (getenv("ATLAS_COST")) {   /* (a closer look: a click's frames, then turning through each kind) */
        wm_device(1);
        for (int c = 0; c < 3; c++) {
            wheel_turn(1, false);
            for (int f = 0; f < 8; f++) { wm_frame(DT); const float *fi = wm_frame_info(); printf("    click %d frame %d: wins %.0f px %.0f comp %.0f wave %.0f cells %.1f send %.1f frame %.1f\n", c, f, fi[FI_WINS], fi[FI_PX], fi[FI_COMP], fi[FI_WAVE], fi[FI_CELLS_MS], fi[FI_SEND_MS], fi[FI_FRAME_MS]); }
        }
        wm_device(0);
        static const char *const K[] = { "planet", "constellation", "star", "double", "open", "globular", "nebula", "planetary", "galaxy", "shower" };
        for (int k = 0; k < 10; k++) { char a[96]; snprintf(a, sizeof a, "{\"kind\":\"%s\",\"index\":0,\"level\":\"objects\"}", K[k]); wm_atlas("atlas.show", a); frames(60); double w; const double t = device_ms(true, &w); printf("    turning %s: %.1f (%.1f)\n", K[k], t, w); }
    }
    printf("  device  the 1.75C's model, ms a frame (worst): Orion %.1f (%.1f), the Orion Nebula %.1f (%.1f), the dome %.1f (%.1f); galaxies turning %.1f (%.1f)\n", orion, w0, m42, w1, where, w3, turn, w2);
    /* the fluid task's own share: a card's items each frame and a model made, timed here, times the device's slowness */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    static atlas_pt_t pts[ATLAS_DOTS];
    for (int q = 0; q < 200; q++) atlas_model(AM_GALAXY, 0, 0.4f, 35.f, 100.f, (uint32_t)q, pts, ATLAS_DOTS);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double model_us = ((t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) * 1e-3) / 200.;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int q = 0; q < 200; q++) { scene_beat(s_sp.cur, &(scene_beat_t){ 0 }); atlas_frame(s_sp.cur, 0.f, false); }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double card_us = ((t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) * 1e-3) / 200.;
    printf("  sim     a card's items %.0f us a frame here, about %.2f ms on the device; a galaxy's model %.0f us, about %.1f ms (once a card)\n", card_us, card_us * DEV_CPU_X / 1000., model_us, model_us * DEV_CPU_X / 1000.);
    const double floor_ms = 1000. / 30.;
    CHECK(orion <= floor_ms && m42 <= floor_ms && where <= floor_ms && turn <= floor_ms, "30 frames a second at least: %.1f, %.1f, %.1f, %.1f ms", orion, m42, where, turn);
    CHECK(card_us * DEV_CPU_X / 1000. < 3., "a card's items each frame: under 3 ms on the device");
}

/* ── renders ────────────────────────────────────────────────────────────────────────────────────────────────────── */
static void snap(const char *dir, const char *name) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const uint8_t *p = wm_pixels();
    fprintf(f, "P6\n%d %d\n255\n", S, S);
    for (int q = 0; q < S * S; q++) fwrite(p + q * 4, 1, 3, f);
    fclose(f);
}
static void renders(const char *dir, const char *noplace) {
    static const struct { const char *name, *show; } R[] = {
        { "1-kinds", "{\"kind\":\"constellation\",\"level\":\"kinds\"}" },
        { "2-orion", "{\"kind\":\"constellation\",\"index\":0,\"level\":\"objects\"}" },
        { "3-orion-where", "{\"kind\":\"constellation\",\"index\":0,\"level\":\"objects\",\"face\":\"where\"}" },
        { "4-pleiades", "{\"kind\":\"open\",\"index\":0,\"level\":\"objects\"}" },
        { "5-orion-nebula", "{\"kind\":\"nebula\",\"index\":0,\"level\":\"objects\"}" },
        { "6-andromeda", "{\"kind\":\"galaxy\",\"index\":0,\"level\":\"objects\"}" },
        { "7-saturn", "{\"kind\":\"planet\",\"index\":2,\"level\":\"objects\"}" },
        { "8-moon", "{\"kind\":\"planet\",\"index\":0,\"level\":\"objects\"}" },
        { "9-betelgeuse", "{\"kind\":\"star\",\"index\":1,\"level\":\"objects\"}" },
        { "10-ring-nebula", "{\"kind\":\"planetary\",\"index\":1,\"level\":\"objects\"}" },
        { "11-geminids", "{\"kind\":\"shower\",\"index\":2,\"level\":\"objects\"}" },
        { "12-albireo", "{\"kind\":\"double\",\"index\":6,\"level\":\"objects\"}" },
    };
    wm_device(1);
    for (size_t i = 0; i < sizeof R / sizeof R[0]; i++) { wm_atlas("atlas.show", R[i].show); frames(150); snap(dir, R[i].name); }
    int loads;
    send_all(noplace, &loads); frames(30);
    wm_atlas("atlas.show", "{\"kind\":\"constellation\",\"index\":0,\"level\":\"objects\"}"); frames(150);
    snap(dir, "13-no-place");
    wm_device(0);
    printf("  renders %zu screens into %s\n", sizeof R / sizeof R[0] + 1, dir);
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: atlas_test <repo root> <berlin.jsonl> <noplace.jsonl> [render dir]\n"); return 2; }
    test_models();
    char path[1024];
    size_t len;
    snprintf(path, sizeof path, "%s/packages/atlas/atlas.json", argv[1]);
    char *pkg = slurp(path, &len);
    test_package(pkg, len);
    if (wm_start(S, 7, 0) != 0 || wm_package(pkg, len) != 0) { fprintf(stderr, "atlas_test: no start\n"); return 2; }
    wm_sense(1);
    frames(60);
    test_load(argv[2]);
    test_levels();
    test_cards();
    test_glass();
    test_cost();
    test_close(argv[2]);
    test_noplace(argv[3]);
    if (argc > 4) { test_load(argv[2]); renders(argv[4], argv[3]); }
    free(pkg);
    printf("atlas_test: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
