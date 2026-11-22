/*
 * SPDX-License-Identifier: MIT
 *
 * weather_test.c: the weather on the glass alone (main/creature/weather.c). Nothing told: nothing made and no random
 * number drawn. Told: it eases there through drizzle, holds its lifetime, eases back to clear and the floor dries to
 * nothing, then it is exactly as before. Each kind: streaks, rings, drops on the glass and a wet floor appear, within
 * their bounds a frame and a second; none of the glass's drops on the face; the moonlit rule against the moon's lit
 * fraction; the setting off: nothing; the same seed: the same weather.
 */
#include "weather.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_pass, s_fail;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define W 59
#define H 36
#define DT (1.f / 30.f)

static int dots(const sky_cell_t *c, int n) { int s = 0; for (int q = 0; q < n; q++) s += __builtin_popcount(c[q].bits); return s; }
static int luma(uint32_t v) { return (int)(0.3f * (v >> 16 & 255) + 0.59f * (v >> 8 & 255) + 0.11f * (v & 255) + 0.5f); }
static weather_t *fresh(uint32_t seed) { static weather_t w; weather_init(&w, seed); return &w; }
static void run(weather_t *w, float s, bool show, float lit) { for (int f = 0; f < (int)(s * 30.f + 0.5f); f++) weather_frame(w, W, H, DT, show, lit); }

static void nothing_told(void) {
    weather_t *w = fresh(7);
    const uint32_t rng = w->rng;
    run(w, 30.f, true, 1.f);
    CHECK(w->rng == rng, "nothing told: a random number was drawn");
    CHECK(!w->show && !w->nback && !w->nfront && !w->nglass && !w->nbump && w->wet == 0.f && w->level == 0.f, "nothing told: something to draw");
    CHECK(!weather_drawn(w), "nothing told: drawn");
    weather_tell(w, WX_CLEAR, 100.f);
    run(w, 5.f, true, 1.f);
    CHECK(w->rng == rng && !w->show, "clear told: something made");
}

/* told: through drizzle to rain, its lifetime, back through drizzle to clear, then the floor dries to exactly nothing */
static void lifetime(void) {
    weather_t *w = fresh(11);
    weather_tell(w, WX_RAIN, 60.f);
    CHECK(w->kind == WX_RAIN && fabsf(w->left - 60.f) < 1e-3f, "told rain for 60 s: kind %d left %.1f", w->kind, w->left);
    float prev = 0.f; bool jumped = false, at1 = false;
    for (int f = 0; f < 30 * 15; f++) {
        weather_frame(w, W, H, DT, true, 0.f);
        if (w->level - prev > 2.f * DT / WX_EASE_S) jumped = true;
        if (fabsf(w->level - 1.f) < 0.01f) at1 = true;
        prev = w->level;
    }
    CHECK(!jumped, "the level jumped");
    CHECK(at1, "rain came without passing drizzle");
    CHECK(w->level == 2.f, "rain not reached in 15 s: %.2f", w->level);
    run(w, 46.f, true, 0.f);
    CHECK(w->kind == WX_CLEAR && w->left == 0.f, "61 s on: still told %d (%.1f s left)", w->kind, w->left);
    CHECK(w->wet > 0.6f, "a minute of rain: the floor only %.2f wet", w->wet);
    bool at1b = false;
    for (int f = 0; f < 30 * 15; f++) { weather_frame(w, W, H, DT, true, 0.f); if (fabsf(w->level - 1.f) < 0.01f) at1b = true; }
    CHECK(at1b && w->level == 0.f, "back to clear not through drizzle (%.2f)", w->level);
    CHECK(w->wet > 0.f && w->show, "the floor dry at once after the rain");
    run(w, 4.f * 60.f, true, 0.f);
    CHECK(w->wet > 0.f && w->wet < 0.9f, "four minutes on: wet %.2f", w->wet);
    int wet_cells = 0; for (int k = 0; k < W * H; k++) wet_cells += weather_wet_at(w, k) > 0;
    weather_frame(w, W, H, WX_DRY_S, true, 0.f);                  /* (the screen off a quarter of an hour: the clock counts) */
    run(w, 80.f, true, 0.f);                                      /* (the drops on the glass run their time out) */
    int left = 0; for (int k = 0; k < W * H; k++) left += weather_wet_at(w, k) > 0;
    CHECK(wet_cells > 0 && left == 0, "dried: %d wet cells, then %d", wet_cells, left);
    CHECK(!w->show && !weather_drawn(w) && !w->nback && !w->nfront && !w->nglass && !w->nbead && !w->nstreak && !w->nring, "dry and clear: something still drawn");
    const uint32_t rng = w->rng;
    run(w, 10.f, true, 0.f);
    CHECK(w->rng == rng, "dry and clear: random numbers still drawn");
    /* a lifetime is held to its bounds; none given: an hour */
    weather_tell(w, WX_DRIZZLE, 0.f); CHECK(w->left == WX_LIFE_DEF_S, "no lifetime: %.0f", w->left);
    weather_tell(w, WX_DRIZZLE, 1e6f); CHECK(w->left == WX_LIFE_MAX_S, "a long lifetime: %.0f", w->left);
    weather_tell(w, 9, 100.f); CHECK(w->kind == WX_DRIZZLE, "a kind that is none was taken");
    CHECK(weather_kind("downpour") == WX_DOWNPOUR && weather_kind("snow") < 0 && weather_kind(NULL) < 0, "kinds by name");
}

/* each kind, settled: what it shows and its bounds; a downpour's shake */
static void kinds(void) {
    static const int MAX_BACK[WX_KINDS] = { 0, 160, 500, 900 };   /* streak, splash and ring dots a frame, at most */
    float landed[WX_KINDS] = { 0 };
    for (int kind = WX_DRIZZLE; kind <= WX_DOWNPOUR; kind++) {
        weather_t *w = fresh(100 + (uint32_t)kind);
        weather_tell(w, kind, 3600.f);
        run(w, 100.f, true, 0.f);
        int back = 0, front = 0, glass = 0, bumps = 0, rings = 0, mx_back = 0, mx_bump = 0, bright = 0, face = 0;
        const int N = 30 * 20;
        for (int f = 0; f < N; f++) {
            weather_frame(w, W, H, DT, true, 0.f);
            const int b = dots(w->back_c, w->nback);
            back += b; front += dots(w->front_c, w->nfront); glass += dots(w->glass_c, w->nglass);
            bumps += w->nbump; rings += w->nring;
            if (b > mx_back) mx_back = b;
            if (w->nbump > mx_bump) mx_bump = w->nbump;
            for (int c = 0; c < w->nback; c++) if (luma(w->back_c[c].rgb) > bright) bright = luma(w->back_c[c].rgb);
            for (int c = 0; c < w->nfront; c++) if (luma(w->front_c[c].rgb) > bright) bright = luma(w->front_c[c].rgb);
            for (int q = 0; q < w->nbead; q++) {
                const float a = (w->bead[q].x - 0.5f) / 0.21f, e = (w->bead[q].y - 0.29f) / 0.17f;
                if (a * a + e * e < 1.f) face++;
            }
        }
        landed[kind] = (float)bumps / 20.f;
        printf("  %-9s a frame: %d streak/ring dots (max %d), %d in front, %d on the glass, %.1f rings; %.1f bumps a second (max %d a frame)\n",
               WX_NAMES[kind], back / N, mx_back, front / N, glass / N, (float)rings / N, landed[kind], mx_bump);
        CHECK(back / N > 10 && rings > 0 && glass > 0, "%s: no rain (%d dots, %d rings, %d on the glass)", WX_NAMES[kind], back / N, rings, glass);
        CHECK(mx_back <= MAX_BACK[kind], "%s: %d dots a frame", WX_NAMES[kind], mx_back);
        CHECK(mx_bump <= WX_BUMPS, "%s: %d bumps a frame", WX_NAMES[kind], mx_bump);
        CHECK(bright <= 160, "%s: rain as bright as %d (the eyes ~205)", WX_NAMES[kind], bright);
        CHECK(face == 0, "%s: a drop on the glass over the face", WX_NAMES[kind]);
        CHECK(kind == WX_DOWNPOUR ? front > 0 : front == 0, "%s: rain in front %d", WX_NAMES[kind], front);
        CHECK(w->wet == 1.f, "%s: two minutes of it and the floor %.2f wet", WX_NAMES[kind], w->wet);
        if (kind == WX_DOWNPOUR) {
            CHECK(w->shake, "two minutes of downpour: no shake due");
            weather_shook(w);
            CHECK(!w->shake && w->shake_wait >= WX_SHAKE_GAP_MIN_S, "after a shake the next one at once (%.0f s)", w->shake_wait);
        } else CHECK(!w->shake, "%s: a shake", WX_NAMES[kind]);
    }
    CHECK(landed[WX_DRIZZLE] < landed[WX_RAIN] && landed[WX_RAIN] <= 20.f && landed[WX_DOWNPOUR] <= 20.f,   /* (the downpour thinned: 60 % bump) */
          "bumps a second: %.1f %.1f %.1f", landed[WX_DRIZZLE], landed[WX_RAIN], landed[WX_DOWNPOUR]);
}

/* the moonlit look: drizzle and rain under a bright moon; plain at a thin moon, with no moon, in a downpour */
static void moonlit(void) {
    static const struct { int kind; float lit, lo, hi; } C[] = {
        { WX_RAIN, 1.f, 0.99f, 1.f }, { WX_DRIZZLE, 0.97f, 0.99f, 1.f }, { WX_RAIN, 0.6f, 0.3f, 0.7f },
        { WX_RAIN, 0.2f, 0.f, 0.f }, { WX_RAIN, 0.f, 0.f, 0.f }, { WX_DOWNPOUR, 1.f, 0.f, 0.f },
    };
    for (size_t c = 0; c < sizeof(C) / sizeof(C[0]); c++) {
        weather_t *w = fresh(5);
        weather_tell(w, C[c].kind, 3600.f);
        run(w, 25.f, true, C[c].lit);
        CHECK(w->moonlit >= C[c].lo && w->moonlit <= C[c].hi, "%s at lit %.2f: moonlit %.2f", WX_NAMES[C[c].kind], C[c].lit, w->moonlit);
    }
    /* moonlit rain shows where the moon lights it: under the moon, not on the far left */
    weather_t *w = fresh(5);
    weather_tell(w, WX_RAIN, 3600.f);
    run(w, 25.f, true, 1.f);
    int left = 0, under = 0;
    for (int f = 0; f < 300; f++) {
        weather_frame(w, W, H, DT, true, 1.f);
        for (int c = 0; c < w->nback; c++) {
            const int i = w->back_c[c].k % W, j = w->back_c[c].k / W;
            if (j * 4 >= (int)(0.49f * 4 * H)) continue;          /* (the sky only: the floor's rings stay) */
            if (i < W / 3) left += __builtin_popcount(w->back_c[c].bits);
            else if (i > W / 2) under += __builtin_popcount(w->back_c[c].bits);
        }
    }
    CHECK(under > 4 * left, "moonlit: %d dots under the moon, %d on the left", under, left);
}

static void setting_and_hidden(void) {
    weather_t *w = fresh(3);
    weather_tell(w, WX_RAIN, 3600.f);
    run(w, 20.f, true, 0.f);
    CHECK(w->show, "rain: nothing shown");
    weather_set(w, false);
    const uint32_t rng = w->rng;
    run(w, 5.f, true, 0.f);
    CHECK(!w->show && !w->nback && !w->nbump && w->rng == rng && w->wet == 0.f, "the setting off: something made");
    weather_tell(w, WX_RAIN, 3600.f);
    weather_set(w, true);
    run(w, 1.f, true, 0.f);
    CHECK(!w->show && w->kind == WX_CLEAR, "the setting on again: rain told while off kept");
    /* off the glass (a scene, a question, words): nothing made and no bump; the clock runs */
    weather_tell(w, WX_DOWNPOUR, 100.f);
    run(w, 20.f, false, 0.f);
    CHECK(!w->show && !w->nback && !w->nfront && !w->nglass && !w->nbump && !w->nstreak, "off the glass: something made");
    CHECK(w->left < 81.f && w->level == 3.f, "off the glass: the clocks stopped (%.0f left, level %.1f)", w->left, w->level);
    run(w, 1.f, true, 0.f);
    CHECK(w->show && w->nstreak > 0, "back on the glass: no rain");
}

static void repeat(void) {
    weather_t a, b;
    weather_init(&a, 42); weather_init(&b, 42);
    weather_tell(&a, WX_RAIN, 600.f); weather_tell(&b, WX_RAIN, 600.f);
    bool same = true;
    for (int f = 0; f < 900 && same; f++) {
        weather_frame(&a, W, H, DT, true, 0.5f); weather_frame(&b, W, H, DT, true, 0.5f);
        same = a.nback == b.nback && !memcmp(a.back_c, b.back_c, sizeof(sky_cell_t) * (size_t)a.nback) && a.nbump == b.nbump;
    }
    CHECK(same, "the same seed, another weather");
    weather_init(&b, 43); weather_tell(&b, WX_RAIN, 600.f);
    weather_init(&a, 42); weather_tell(&a, WX_RAIN, 600.f);
    run(&a, 20.f, true, 0.f); run(&b, 20.f, true, 0.f);
    CHECK(a.nback != b.nback || memcmp(a.back_c, b.back_c, sizeof(sky_cell_t) * (size_t)a.nback), "another seed, the same weather");
}

int main(void) {
    nothing_told();
    lifetime();
    kinds();
    moonlit();
    setting_and_hidden();
    repeat();
    printf("weather_test: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
