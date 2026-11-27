/*
 * SPDX-License-Identifier: MIT
 *
 * waves_test.c: the waves package's scope on the firmware's own screen (the web module's native build: web_molty.c
 * included whole, its statics are this file's), with the molty-waves skill's own chord kinds and packages/waves.
 *   readout    hits a second, then hertz and the note with its octave (A0 .. A4), spelt in the chord's key
 *   package    the waves package parses; a wave and a beat in one layout, a bad `sets` refused
 *   levels     wave.play stopped: the chord kinds on the rim (each previewed, heard), its dots at rest joined as its
 *              shape, the play mark, its name, the note up top; the note goes up to the twelve notes (beads in their
 *              colours), the centre comes back down and plays; playing: the pitch a semitone a click, turning fast the
 *              chosen note's next octave with the longer tick; a tap on a whole number mutes its voice; the centre
 *              stops it on the note it stopped at; the `ring`, `tap` and `wave` events
 *   picture    the circle and its dots for every chord kind slow (each voice's dots on the circle, its shape inscribed
 *              in it), their outline for every kind fast; never more shapes than the screen holds with the hold ring,
 *              a still outline at a note in whole numbers (nothing drawn again frame after frame)
 *   speaker    one player at a time: a rhythm stops the chord and the chord the rhythm; no circle on screen stops it
 *   close      the reject button and a finger held a second close it, and the chord stops
 *   glass      device mode (only the windows the device sends reach the panel): over ten states the panel is never more
 *              than WAVE_GLASS_LAG frames behind the drawer's whole picture, and held still it is that picture exactly
 *   cost       the device's frame (the 1.75C's model) against the creature's, playing and turning
 *   renders    with a third argument, the screen at a few states as PNG there (slow, 16 a second, a note, a fifth, a
 *              major chord, the chooser, the notes)
 *
 *   waves_test <repo root> <chords.json> [render dir]
 */
#include "../../web/web_molty.c"

#include <time.h>

static int s_pass, s_fail;
#define WAVE_GLASS_LAG 3               /* frames (a tenth of a second) */
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define DT 16667u
#define S 466
static void frames(int n) { for (int i = 0; i < n; i++) wm_frame(DT); }
static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "waves_test: cannot read %s\n", path); exit(2); }
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
/* a drag round the rim from `from` degrees, `deg` clockwise, `per` frames a degree */
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
static char CHORDS[8192];
static int kind(const char *id) {
    cJSON *a = cJSON_Parse(CHORDS), *c;
    int i = 0, at = -1;
    cJSON_ArrayForEach(c, a) { if (!strcmp(str(cJSON_GetObjectItem(c, "id")), id)) at = i; i++; }
    cJSON_Delete(a);
    if (at < 0) { fprintf(stderr, "waves_test: no chord %s\n", id); exit(2); }
    return at;
}
static int play(const char *id, const char *more) {
    static char b[sizeof CHORDS + 256];
    snprintf(b, sizeof b, "{\"chords\":%s,\"pick\":%d%s%s}", CHORDS, kind(id), more[0] ? "," : "", more);
    return wm_rhythm("wave.play", b);
}
static void show(void) { wm_scene_op("scope", "{}", 600, 0); }
static wave_screen_t now(void) { wave_screen_t s; wave_now(&s); return s; }
/* the live dots on the circle (r about the item's) and the polylines (a shape a voice, the outline) */
static int on_circle(const scene_t *sc) {
    int n = 0;
    for (int q = sc->nfixed; q < sc->nitems; q++) { const scene_item_t *it = &sc->items[q]; n += it->live && it->k == PK_DISC && fabsf(hypotf(it->x, it->y) - 0.5f) < 0.16f; }
    return n;
}
static int marks(const scene_t *sc, uint32_t rgb) {
    int n = 0;
    for (int q = sc->nfixed; q < sc->nitems; q++) { const scene_item_t *it = &sc->items[q]; n += it->live && it->k == PK_DISC && it->rgb == rgb && hypotf(it->x, it->y - 0.15f) < 0.1f; }
    return n;
}
static bool has_text(const scene_t *sc, const char *s, float y) {
    for (int q = sc->nfixed; q < sc->nitems; q++) {
        const scene_item_t *it = &sc->items[q];
        if (it->k == PK_TEXT && it->nstr == strlen(s) && !memcmp(sc->chars + it->str, s, it->nstr) && fabsf(it->y - y) < 0.02f) return true;
    }
    return false;
}

/* ── the readout ────────────────────────────────────────────────────────────────────────────────────────────────── */
static void test_readout(void) {
    static const struct { float hz, midi; int key; const char *big, *unit; } R[] = {
        { 1.02f, -36.f, 0, "1.0", "A SECOND" }, { 6.9f, 0.f, 0, "6.9", "A SECOND" }, { 16.f, 0.f, 0, "16", "A SECOND" }, { 19.4f, 15.f, 0, "19", "A SECOND" },
        { 27.5f, 21.f, 0, "28", "HZ A0" }, { 55.f, 33.f, 0, "55", "HZ A1" }, { 110.f, 45.f, 0, "110", "HZ A2" }, { 220.f, 57.f, 0, "220", "HZ A3" },
        { 440.f, 69.f, 0, "440", "HZ A4" }, { 1046.5f, 84.f, 0, "1.05K", "HZ C6" }, { 1100.f, 85.f, 0, "1.1K", "HZ Db6" }, { 5587.7f, 113.f, 0, "5.59K", "HZ F8" },
        { 277.18f, 61.f, 0, "277", "HZ Db4" }, { 277.18f, 61.f, 1, "277", "HZ C#4" }, { 2000.f, 95.f, 0, "2.0K", "HZ B6" },
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof R / sizeof R[0]; i++) {
        char big[32], unit[32];
        wave_readout(R[i].hz, R[i].midi, R[i].key, big, unit);
        if (strcmp(big, R[i].big) || strcmp(unit, R[i].unit)) { bad++; printf("    %.2f Hz: %s %s, wanted %s %s\n", R[i].hz, big, unit, R[i].big, R[i].unit); }
    }
    CHECK(!bad, "the readout: hits a second, then hertz and the note with its octave, spelt in the chord's key (%d off)", bad);
}

/* ── the package ────────────────────────────────────────────────────────────────────────────────────────────────── */
static void test_package(const char *json, size_t len) {
    const char *err = NULL;
    pkg_t *p = pkg_parse(json, len, &err);
    CHECK(p && p->nlayouts == 1 && p->layouts[0].items[0].k == PK_WAVE, "the waves package parses (%s)", err ? err : "");
    pkg_unref(p);
    static const char *const BAD[] = {
        "{\"v\":\"molty-pkg/1\",\"id\":\"x\",\"name\":\"X\",\"version\":\"1.0.0\",\"kind\":\"scenes\",\"layouts\":{\"a\":{\"dur\":0,\"items\":[{\"k\":\"wave\",\"r\":0.5},{\"k\":\"beat\",\"r\":0.5}]}}}",
        "{\"v\":\"molty-pkg/1\",\"id\":\"x\",\"name\":\"X\",\"version\":\"1.0.0\",\"kind\":\"scenes\",\"layouts\":{\"a\":{\"dur\":0,\"items\":[{\"k\":\"wave\",\"r\":0.5},{\"k\":\"dial\",\"id\":\"p\",\"sets\":\"volume\"}]}}}",
        "{\"v\":\"molty-pkg/1\",\"id\":\"x\",\"name\":\"X\",\"version\":\"1.0.0\",\"kind\":\"scenes\",\"layouts\":{\"a\":{\"dur\":0,\"items\":[{\"k\":\"wave\"}]}}}",
    };
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) { pkg_t *b = pkg_parse(BAD[i], strlen(BAD[i]), &err); CHECK(!b, "bad package %zu refused", i); pkg_unref(b); }
}

/* ── the levels: the chords, the notes, playing ─────────────────────────────────────────────────────────────────── */
static void test_levels(void) {
    show();
    CHECK(play("major", "\"note\":0,\"paused\":true") == 0, "wave.play, stopped, on C major");
    frames(40);
    scene_t *sc = s_sp.cur;
    CHECK(sc && sc->wave >= 0, "the scope has its wave");
    if (!sc) return;
    wave_screen_t w = now();
    CHECK(w.level == WAVE_AT_CHORDS && s_wh.w.face == 1 && s_wh.w.nlabels == 29 && s_wh.w.index == kind("major"), "stopped: the chord kinds on the rim, major under the notch");
    CHECK(wm_rhythm_pcm_n() > 0, "the chord heard once, plucked");
    CHECK(on_circle(sc) == 15 && marks(sc, SCENE_NOTCH_RGB) == 10, "its 4 + 5 + 6 dots on the circle (%d), the play mark (%d)", on_circle(sc), marks(sc, SCENE_NOTCH_RGB));
    CHECK(has_text(sc, "MAJOR", -0.02f) && has_text(sc, "C", SCENE_WAVE_TOP_Y), "its name in the middle, the note up top");
    int polys = 0;
    for (int q = sc->nfixed; q < sc->nitems; q++) polys += sc->items[q].k == PK_LINE && (sc->items[q].npts == 5 || sc->items[q].npts == 6 || sc->items[q].npts == 7);
    CHECK(polys == 3, "joined as a square, a pentagon and a hexagon (%d)", polys);
    events_named("x", NULL);
    drag(90.f, 34, 2);
    lift();
    cJSON *last = NULL;
    w = now();
    CHECK(w.index == kind("minor") && s_wh.w.index == kind("minor"), "30 degrees: the next kind, minor");
    CHECK(events_named("ring", &last) == 1 && !strcmp(str(cJSON_GetObjectItem(cJSON_GetObjectItem(last, "data"), "spot")), "chord") &&
          !strcmp(str(cJSON_GetObjectItem(cJSON_GetObjectItem(last, "data"), "id")), "minor"), "ring {spot: chord, id: minor}");
    tap(0.f, SCENE_WAVE_TOP_Y);
    frames(4);
    w = now();
    CHECK(w.level == WAVE_AT_NOTES && s_wh.w.level && s_wh.w.nlabels == 12 && s_wh.w.tints[1] != s_wh.w.tints[2], "the note up top: the twelve notes, a bead in each one's colour");
    drag(90.f, 34, 2); lift();
    w = now();
    CHECK(w.index == 1 && !strcmp(w.name, "C#/Db"), "a click: C#/Db (%s)", w.name);
    CHECK(events_named("ring", &last) == 1 && !strcmp(str(cJSON_GetObjectItem(cJSON_GetObjectItem(last, "data"), "spot")), "note"), "ring {spot: note}");
    tap(0.02f, 0.02f); frames(4);
    w = now();
    CHECK(w.level == WAVE_AT_CHORDS && !strcmp(w.top, "C#"), "the centre: down to the chords on it (minor spells it C#: %s)", w.top);
    tap(0.02f, 0.02f); frames(30);
    w = now();
    CHECK(w.level == WAVE_AT_PLAYING && s_wh.w.face == 0 && fabsf(w.v.f[0] - wav_hz(61.f)) < 1.f, "again: it plays, at the pitch just heard (%.1f Hz)", w.v.f[0]);
    CHECK(events_named("wave", &last) == 1 && !strcmp(str(cJSON_GetObjectItem(cJSON_GetObjectItem(last, "data"), "state")), "playing"), "event wave {state: playing}");
    CHECK(!strcmp(w.big, "277") && !strcmp(w.unit, "HZ C#4"), "the readout: %s %s", w.big, w.unit);
    /* the pitch: a semitone a click; fast, straight to the chosen note's next octave, the longer tick */
    wm_wheel_ticks(NULL, NULL);
    drag(90.f, 14, 3); lift();
    float midi = 0.f, land;
    int level, index;
    wave_where(&level, &index, &midi, &land);
    CHECK(midi == 62.f, "12 degrees: a semitone up (%.0f)", midi);
    CHECK(events_named("ring", &last) == 1 && !strcmp(str(cJSON_GetObjectItem(cJSON_GetObjectItem(last, "data"), "spot")), "pitch") &&
          cJSON_GetObjectItem(cJSON_GetObjectItem(last, "data"), "value")->valuedouble == 62., "ring {spot: pitch, value: 62}");
    wheel_turn(1, true);                                /* (a click the ring input calls fast: over 200 degrees a second) */
    wave_where(&level, &index, &midi, &land);
    const int plan = atomic_load(&s_tick_plan);
    CHECK(midi == 73.f && land > 0.5f && plan == WAVE_LAND_TICK, "a fast click: C#5, the chosen note's next octave (%.0f), the longer tick (%d ms at %d)", midi, plan >> 8, plan & 255);
    wheel_turn(-1, false);
    wave_where(&level, &index, &midi, &land);
    CHECK(midi == 72.f && atomic_load(&s_tick_plan) != WAVE_LAND_TICK, "a slow click back: a semitone, the wheel's own tick");
    wheel_turn(1, false);
    frames(4);
    /* a tap on a whole number up top mutes its voice */
    frames(70);
    int num = -1;
    for (int q = sc->nfixed; q < sc->nitems; q++) if (sc->items[q].k == PK_TEXT && sc->items[q].layer == 1) num = q;
    CHECK(num >= 0, "the whole numbers up top, each its voice's");
    if (num >= 0) {
        tap(sc->items[num].x, sc->items[num].y); frames(4);
        CHECK(now().v.mute == 2 && events_named("tap", &last) == 1 && !strcmp(str(cJSON_GetObjectItem(cJSON_GetObjectItem(last, "data"), "spot")), "voice-2"), "a tap on 12: its voice muted, tap {spot: voice-2}");
        tap(sc->items[num].x, sc->items[num].y); frames(4);
        CHECK(now().v.mute == 0, "again: back");
    }
    tap(0.02f, 0.02f); frames(10);
    w = now();
    CHECK(w.level == WAVE_AT_CHORDS && w.index == kind("minor") && !strcmp(w.top, "C#") && events_named("wave", &last) == 1, "the centre: stopped, choosing, on the note it stopped at");
    cJSON_Delete(last);
    printf("  levels  the chords (previewed), the notes, playing (a semitone a click, an octave fast), mutes, back; ring, tap and wave events\n");
}

/* ── the picture: every kind, slow and fast; the shapes it makes; a still outline ───────────────────────────────── */
static void test_picture(void) {
    show();
    play("major", "\"note\":0,\"midi\":60");
    frames(30);
    int worst = 0, bad_dots = 0, bad_fast = 0;
    char worst_at[WAVE_ID_MAX + 1] = "";
    cJSON *a = cJSON_Parse(CHORDS), *c;
    cJSON_ArrayForEach(c, a) {
        const char *id = str(cJSON_GetObjectItem(c, "id"));
        int dots = 0;
        const cJSON *r;
        cJSON_ArrayForEach(r, cJSON_GetObjectItem(c, "ratio")) dots += r->valueint;
        for (int speed = 0; speed < 3; speed++) {
            char set[96];
            snprintf(set, sizeof set, "{\"pick\":%d,%s}", kind(id), speed == 0 ? "\"midi\":-36" : speed == 1 ? "\"count\":true" : "\"midi\":60");
            wm_rhythm("wave.set", set);
            frames(speed == 2 ? 20 : 12);
            const scene_t *sc = s_sp.cur;
            if (s_nshp < 0 || s_nshp > worst) { worst = s_nshp < 0 ? 999 : s_nshp; snprintf(worst_at, sizeof worst_at, "%s", id); }
            if (speed == 0 && on_circle(sc) < (dots <= 40 ? dots : 20)) { bad_dots++; printf("    %s slow: %d dots of %d\n", id, on_circle(sc), dots); }
            int segs = 0;
            for (int q = sc->nfixed; q < sc->nitems; q++) if (sc->items[q].k == PK_LINE && sc->items[q].npts > 30) segs = sc->items[q].npts - 1;
            if (speed == 2 && (segs < SCENE_WAVE_MIN_SEGS || on_circle(sc) > 0)) { bad_fast++; printf("    %s fast: %d strokes, %d dots\n", id, segs, on_circle(sc)); }
        }
    }
    cJSON_Delete(a);
    CHECK(!bad_dots, "slow: each voice's dots on the circle, every kind (the densest give way past 40)");
    CHECK(!bad_fast, "fast: the outline alone, at least %d strokes, every kind", SCENE_WAVE_MIN_SEGS);
    CHECK(worst >= 0 && worst + SCENE_HOLD_DOTS <= FLUID_MAX_SHAPES, "never more shapes than the screen holds with the hold ring: at most %d (%s)", worst, worst_at);
    /* a held finger's ring fits over the busiest */
    wm_rhythm("wave.set", "{\"pick\":0,\"midi\":-36}");
    play("7-sharp-9", "\"midi\":-12");
    frames(30);
    touch_at(1, -0.3f, -0.45f); frames(40);
    CHECK(s_nshp >= SCENE_HOLD_DOTS, "the hold ring over a slow seven sharp nine (%d shapes)", s_nshp);
    lift();
    /* a note in whole numbers: the outline still, nothing drawn again */
    play("major", "\"midi\":60");
    frames(90);
    wm_device(1);
    frames(30);
    long px = 0;
    for (int f = 0; f < 60; f++) { wm_frame(DT); px += s_draw->ndrawn; }
    wm_device(0);
    CHECK(px / 60 < 2000, "C major in whole numbers: a still outline, %ld px drawn a frame", px / 60);
    printf("  picture every kind slow, counted and fast: at most %d shapes (%s); a still major chord draws %ld px a frame\n", worst, worst_at, px / 60);
}

/* ── one player on the speaker; no circle, no sound; closing by hand ────────────────────────────────────────────── */
static const char *RHY = "{\"rhythms\":[{\"id\":\"clave\",\"label\":\"CLAVE\",\"steps\":4,\"per\":1,\"beats\":[1,2,3,4],\"layers\":[{\"id\":\"rim\",\"sound\":\"rim\",\"color\":\"#9fc0ff\",\"hits\":[1,3]}]}],\"pick\":0,\"bpm\":100}";
static void test_speaker(void) {
    show();
    play("fifth", "\"midi\":48");
    frames(20);
    CHECK(wave_active() && rhythm_sounding(), "a fifth plays");
    wm_rhythm("rhythm.play", RHY);
    frames(20);
    CHECK(!wave_active() && rhythm_active(), "rhythm.play: the chord stops, the rhythm has the speaker");
    play("fifth", "\"midi\":48");
    frames(20);
    CHECK(wave_active() && !rhythm_active(), "wave.play: the rhythm stops");
    wm_scene_clear();
    frames(30);
    CHECK(wave_active(), "the circle gone: it plays on a moment");
    frames(150);
    CHECK(!wave_active() && wm_rhythm("wave.set", "{\"midi\":50}") != 0, "no circle for 2 s: it stops (wave.set: not playing)");
    frames(30);
    CHECK(!rhythm_sounding(), "and is silent");
    /* closing by hand */
    show(); play("major", "\"midi\":57"); frames(30);
    events_named("x", NULL);
    wm_button(MOLTY_BTN_REJECT, 1); frames(3); wm_button(MOLTY_BTN_REJECT, 0); frames(30);
    cJSON *last = NULL;
    CHECK(!s_sp.cur && !wave_active() && events_named("wave", &last) == 1, "the reject button: the scope closes, the chord stops (wave {state: stopped})");
    show(); play("major", "\"midi\":57"); frames(30);
    touch_at(1, -0.35f, 0.2f); frames(70); lift(); frames(30);
    CHECK(!s_sp.cur && !wave_active(), "a finger held a second: the same");
    show(); play("major", "\"midi\":57,\"paused\":true"); frames(30);
    wm_button(MOLTY_BTN_ACCEPT, 1); frames(3); wm_button(MOLTY_BTN_ACCEPT, 0); frames(20);
    CHECK(now().level == WAVE_AT_PLAYING, "the accept button is the centre: it plays");
    cJSON_Delete(last);
    wm_rhythm("wave.stop", NULL); frames(60);
    printf("  speaker one player at a time; no circle for 2 s stops it; the buttons and a held finger close it\n");
}

/* ── the glass: what the panel shows against the drawer's whole picture, frame after frame (device mode: only the
 * windows the device sends reach the panel, which keeps the rest). Over a turn the screen may lag a few frames where
 * its drawer has no room (fluid_draw.c owes those cells to the next frame), but no pixel stays wrong for long, and once
 * the picture holds still the panel is the whole picture, exactly. */
/* the hand's angle (its spoke, the only 0.03 wide), -999 none */
static float hand_at(void) {
    const scene_t *sc = s_sp.cur;
    for (int q = sc ? sc->nfixed : 0; sc && q < sc->nitems; q++) if (sc->items[q].k == PK_TICKS && sc->items[q].w == 0.03f) return sc->items[q].a0;
    return -999.f;
}
static int s_torn, s_moves;            /* frames that moved the hand and showed a pixel of it wrong (a hand in two pieces for
                                        * a frame), of the frames that moved it                                             */
/* a pixel of the hand (the core of a stroke tagged FLUID_TAG_LEAD, its drawer's mover) the panel shows wrong */
static bool hand_wrong(const uint16_t *ref) {
    for (int q = 0; q < s_dopts.nshapes; q++) {
        const fluid_shape_t *sh = &s_dopts.shapes[q];
        if (sh->tag != FLUID_TAG_LEAD || sh->k != FLUID_SHAPE_SEG) continue;
        int16_t b[4];
        fluid_shape_box(sh, s_draw->ox, s_draw->oy, b);
        const float vx = sh->x1 - sh->x, vy = sh->y1 - sh->y, l2 = vx * vx + vy * vy;
        for (int y = b[2] < 0 ? 0 : b[2]; y < b[3] && y < S; y++) for (int x = b[0] < 0 ? 0 : b[0]; x < b[1] && x < S; x++) {
            const float px = x - s_draw->ox + 0.5f - sh->x, py = y - s_draw->oy + 0.5f - sh->y;
            const float u = l2 > 0.f ? fminf(1.f, fmaxf(0.f, (px * vx + py * vy) / l2)) : 0.f, ex = px - u * vx, ey = py - u * vy;
            if (ex * ex + ey * ey <= (sh->hw - 1.f) * (sh->hw - 1.f) && s_rgba_of[ref[y * S + x]] != s_rgba[y * S + x]) return true;
        }
    }
    return false;
}
static int glass_lag(int nframes) {
    static uint16_t ref[S * S], age[S * S];
    memset(age, 0, sizeof age);
    int oldest = 0;
    float hand = hand_at();
    for (int f = 0; f < nframes; f++) {
        frames(1);
        const float now = hand_at();
        const bool moved = now != hand && now > -999.f;
        s_moves += moved;
        hand = now;
        if (fluid_draw_morphing(s_draw)) continue;
        const fluid_rect_t t = { ref, S, 0, 0, S, S };
        fluid_draw_rect(s_draw, &s_dopts, &t, true);
        fluid_draw_overlay(s_draw, &t, s_dopts.palette, &s_kept.ov);
        s_torn += moved && hand_wrong(ref);
        for (int q = 0; q < S * S; q++) { age[q] = s_rgba_of[ref[q]] != s_rgba[q] ? age[q] + 1 : 0; if (age[q] > oldest) oldest = age[q]; }
    }
    return oldest;
}
static void test_glass(void) {
    static const struct { const char *chord, *more; } R[] = {
        { "alone", "\"hz\":1" }, { "alone", "\"hz\":8" }, { "alone", "\"hz\":16" }, { "alone", "\"hz\":24" }, { "fifth", "\"hz\":2" },
        { "major", "\"hz\":4.1" }, { "major", "\"hz\":12" }, { "minor", "\"hz\":20" }, { "7-sharp-9", "\"hz\":2" }, { "major-7", "\"paused\":true" },
    };
    wm_device(1);
    frames(60);
    int worst = 0, still_off = 0;
    for (size_t i = 0; i < sizeof R / sizeof R[0]; i++) {
        show(); play(R[i].chord, R[i].more); frames(60);
        const int lag = glass_lag(150);
        if (lag > worst) worst = lag;
        if (lag > WAVE_GLASS_LAG) { const int off = wm_glass_off(); const int *at = wm_glass_off_at(); printf("    %s %s: a pixel wrong %d frames running (%d off now, at %d,%d %d,%d)\n", R[i].chord, R[i].more, lag, off, at[0], at[1], at[2], at[3]); }
    }
    play("major", "\"midi\":60"); frames(90);
    still_off = wm_glass_off();
    wm_device(0);
    CHECK(worst <= WAVE_GLASS_LAG, "every state, 150 frames each: no pixel of the glass wrong more than %d frames running (at most %d)", WAVE_GLASS_LAG, worst);
    CHECK(still_off == 0, "held still: the glass is the whole picture (%d px off)", still_off);
    CHECK(s_moves > 200 && s_torn * 50 <= s_moves, "the hand whole: at most one move in 50 leaves a piece for the next frame (%d of %d)", s_torn, s_moves);
    printf("  glass   10 states in device mode: the panel at most %d frames behind the whole picture, none once still; the hand moved %d times, %d left a piece for a frame\n", worst, s_moves, s_torn);
}

/* ── cost: the 1.75C's frame in the device model ───────────────────────────────────────────────────────────────── */
/* the device's time for a frame (device mode), calibrated on the 1.75C's own logs (research/fps, 2026-11-27): the model
 * counts the drawer's cells pass flat (web_device.c DEV_CELLS_MS) but on the device that pass also makes the drawing's
 * cache of every changed cell (`anim ... cache=9.0`: the cells pass 14 to 27 ms while the hand sweeps): here it is the
 * model's frame or its cells, cache and send in a row, whichever is longer. Also, in percent, the frames over 34 ms
 * (a third refresh: the device's `frames.sim` histogram past its <=34 bucket; the cache is timed by this computer's
 * clock 100 times over, DEV_CPU_X, so a hiccup of its scheduler is a frame 10 ms longer) and those held for the
 * drawer's backlog (the same picture again: a stutter) */
static double s_held, s_over;
static double device_ms(bool turn, double *worst) {
    wm_device(1);
    double sum = 0.;
    int n = 0, held = 0, was = s_wave_held;
    int over = 0;
    *worst = 0.;
    for (int q = 0; q < 240; q++) {
        const float an = (90.f - q * 2.f) * 0.017453293f;
        if (turn) touch_at(1, 0.93f * cosf(an), 0.93f * sinf(an));
        wm_frame(DT);
        const float *fi = wm_frame_info();
        const double ms = fmax(fi[FI_FRAME_MS], fi[FI_CELLS_MS] + s_draw->cache_us / 1000. + fi[FI_SEND_MS]);
        if (q >= 30) held += s_wave_held - (s_wave_held < was ? 0 : was);   /* (mole_fluid.c wave_frame: the same picture again) */
        was = s_wave_held;
        if (q >= 30) { sum += ms; n++; over += ms > 34.; if (ms > *worst) *worst = ms; }
    }
    if (turn) touch_at(0, 0.f, 0.f);
    wm_device(0);
    s_held = 100. * held / n; s_over = 100. * over / n;
    return sum / n;
}
static void test_cost(void) {
    wm_scene_clear(); frames(120);
    double w[6], h[6], o[6];
    const double creature = device_ms(false, &w[0]);
    show(); play("major", "\"midi\":60"); frames(60);
    const double note = device_ms(false, &w[1]); h[1] = s_held; o[1] = s_over;
    play("major", "\"hz\":4.1"); frames(60);
    const double slow = device_ms(false, &w[2]); h[2] = s_held; o[2] = s_over;
    play("alone", "\"midi\":31"); frames(60);   /* 49 Hz: through the hand-over while it turns */
    const double turn = device_ms(true, &w[3]); h[3] = s_held; o[3] = s_over;
    play("major", "\"midi\":24"); frames(60);   /* the zone where the hand blurs and the shivers close */
    const double zone = device_ms(false, &w[4]); h[4] = s_held; o[4] = s_over;
    play("alone", "\"hz\":1"); frames(60);
    const double beat = device_ms(false, &w[5]); h[5] = s_held; o[5] = s_over;
    printf("  device  the 1.75C, calibrated, ms a frame (worst; over 34 ms, held): the creature %.1f (%.1f); C major as a note %.1f (%.1f; %.0f%%, %.0f%%), "
           "slow %.1f (%.1f; %.0f%%, %.0f%%), in the zone %.1f (%.1f; %.0f%%, %.0f%%); one beat a second %.1f (%.1f; %.0f%%, %.0f%%); one voice turning %.1f (%.1f; %.0f%%, %.0f%%)\n",
           creature, w[0], note, w[1], o[1], h[1], slow, w[2], o[2], h[2], zone, w[4], o[4], h[4], beat, w[5], o[5], h[5], turn, w[3], o[3], h[3]);
    /* the sim task's own share (the model counts it flat): the trace and the items, timed here, times the device's
     * slowness (web_device.c DEV_CPU_X) */
    play("7-sharp-9", "\"midi\":36"); frames(60);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int q = 0; q < 200; q++) wave_frame(0.f);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double us = ((t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) * 1e-3) / 200.;
    printf("  sim     the trace and the items of a five-voice chord: %.0f us a frame here, about %.1f ms on the device (x%.0f)\n", us, us * DEV_CPU_X / 1000., DEV_CPU_X);
    CHECK(us * DEV_CPU_X / 1000. + DEV_SIM_MS < 30., "with the sim's own work, under the 30 ms frame");
    const double floor_ms = 1000. / 30.;
    CHECK(note <= floor_ms && slow <= floor_ms && turn <= floor_ms && zone <= floor_ms && beat <= floor_ms, "30 frames a second at least: %.1f, %.1f, %.1f, %.1f, %.1f ms", note, slow, zone, beat, turn);
    double late = 0.;
    for (int q = 1; q < 6; q++) late = fmax(late, o[q]);
    CHECK(late <= 3., "a third refresh at most 3 frames in 100 while it moves (the diagnosis's 97%% bound): at most %.0f%%", late);
    CHECK(h[1] + h[3] + h[5] == 0. && h[2] <= 25. && h[4] <= 25., "held for the backlog: none for a note, a beat or a turn, at most a frame in four for a slow chord (%.0f, %.0f%%)", h[2], h[4]);
    wm_rhythm("wave.stop", NULL); frames(60);
}

/* ── renders: the screen as PNG at a few states ─────────────────────────────────────────────────────────────────── */
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
static void renders(const char *dir) {
    static const struct { const char *name, *chord, *more; int settle; } R[] = {
        { "1-slow-beat", "alone", "\"hz\":1.5", 50 }, { "2-sixteen-a-second", "alone", "\"hz\":16", 40 }, { "3-a-note-a3", "alone", "\"midi\":57", 40 },
        { "4-a-fifth-slow", "fifth", "\"hz\":2", 50 }, { "5-a-fifth-c3", "fifth", "\"midi\":48", 40 }, { "6-c-major-slow", "major", "\"hz\":4", 50 },
        { "7-c-major-c4", "major", "\"midi\":60", 40 }, { "8-the-chooser", "major-7", "\"paused\":true", 20 },
    };
    wm_device(1);                                       /* (the panel as the device sends it: its changed windows only) */
    show(); play("alone", "\"hz\":1"); frames(150);    /* (the scope formed once; each state is a wave.play on it) */
    for (size_t i = 0; i < sizeof R / sizeof R[0]; i++) {
        play(R[i].chord, R[i].more); frames(R[i].settle + 60);
        snap(dir, R[i].name);
    }
    tap(0.f, SCENE_WAVE_TOP_Y); frames(200);
    snap(dir, "9-the-notes");
    wm_device(0);
    wm_rhythm("wave.stop", NULL); frames(60);
    printf("  renders %zu states into %s\n", sizeof R / sizeof R[0] + 1, dir);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: waves_test <repo root> <chords.json> [render dir]\n"); return 2; }
    test_readout();
    char path[1024];
    size_t len, cl;
    snprintf(path, sizeof path, "%s/packages/waves/waves.json", argv[1]);
    char *pkg = slurp(path, &len), *ch = slurp(argv[2], &cl);
    while (cl && (ch[cl - 1] == '\n' || ch[cl - 1] == ' ')) ch[--cl] = 0;
    snprintf(CHORDS, sizeof CHORDS, "%s", ch);
    free(ch);
    test_package(pkg, len);
    if (wm_start(S, 7, 0) != 0 || wm_package(pkg, len) != 0) { fprintf(stderr, "waves_test: no start\n"); return 2; }
    wm_sense(1);
    frames(60);
    test_levels();
    test_picture();
    test_speaker();
    test_glass();
    if (argc > 3) renders(argv[3]);
    test_cost();
    free(pkg);
    printf("waves_test: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
