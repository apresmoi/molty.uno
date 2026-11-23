/*
 * SPDX-License-Identifier: MIT
 *
 * sky_test.c: the sky behind the creature (main/creature/sky.c). The moon's phase and distance against published dates,
 * its drawn size (a supermoon at the largest, a micromoon at the smallest), where its dots sit and which side is lit,
 * a new moon showing nothing; the falling star's wait, run, path and bound; the setting and no time showing nothing.
 *
 * The dates: Fred Espenak, "Phases of the Moon: 2001 to 2100" and "Moon at Perigee and Apogee: 2001 to 2100"
 * (astropixels.com/ephemeris/phasescat/phases2001.html, astropixels.com/ephemeris/moon/moonperap2001.html), times UT;
 * the 2000-01-06 new moon is Meeus's own example epoch (Astronomical Algorithms, ch. 49).
 */
#include "sky.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int s_pass, s_fail;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double utc(int y, int mo, int d, int h, int mi) {
    /* days from civil (H. Hinnant), no time zone */
    y -= mo <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400, doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long days = (long)era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return (double)days * 86400.0 + h * 3600.0 + mi * 60.0;
}
static float angle_to(double el, double want) { double d = fmod(el - want + 540.0, 360.0) - 180.0; return (float)fabs(d); }

#define W 59                                     /* the 466 px panel's grid (fluid_grid_for_stage) */
#define H 36

static void phases(void) {
    static const struct { int y, mo, d, h, mi; double el; } P[] = {
        { 2000, 1, 6, 18, 14, 0 },               /* new (Meeus's epoch) */
        { 2017, 8, 21, 18, 30, 0 },              /* new: total solar eclipse */
        { 2024, 4, 8, 18, 21, 0 },               /* new: total solar eclipse */
        { 2016, 11, 14, 13, 52, 180 },           /* full: the supermoon */
        { 2019, 9, 14, 4, 33, 180 },             /* full: the micromoon */
        { 2022, 11, 8, 11, 2, 180 },             /* full: total lunar eclipse */
        { 2025, 3, 14, 6, 55, 180 },             /* full: total lunar eclipse */
    };
    for (size_t q = 0; q < sizeof(P) / sizeof(P[0]); q++) {
        const sky_moon_t m = sky_moon_at(utc(P[q].y, P[q].mo, P[q].d, P[q].h, P[q].mi));
        /* 0.5 deg of elongation: the moon gains 12 deg a day, so the moment within an hour */
        CHECK(angle_to(m.elong, P[q].el) < 0.5f, "%04d-%02d-%02d: elongation %.2f, want %.0f", P[q].y, P[q].mo, P[q].d, m.elong, P[q].el);
        CHECK(P[q].el ? m.lit > 0.999f : m.lit < 0.001f, "%04d-%02d-%02d: lit %.4f", P[q].y, P[q].mo, P[q].d, m.lit);
        /* a day either side: waxing before the full moon and after the new one, waning the other way */
        const double t = utc(P[q].y, P[q].mo, P[q].d, P[q].h, P[q].mi);
        const sky_moon_t before = sky_moon_at(t - 86400.0), after = sky_moon_at(t + 86400.0);
        CHECK(P[q].el ? before.waxing && !after.waxing : !before.waxing && after.waxing, "%04d-%02d-%02d: waxing before %d after %d",
              P[q].y, P[q].mo, P[q].d, before.waxing, after.waxing);
        CHECK(fabsf(before.lit - after.lit) < 0.02f && (P[q].el ? before.lit > 0.95f : before.lit < 0.05f), "a day either side alike: %.3f %.3f", before.lit, after.lit);
    }
    /* first quarter 2024-04-15 19:13 UT: half lit, waxing */
    const sky_moon_t fq = sky_moon_at(utc(2024, 4, 15, 19, 13));
    CHECK(fabsf(fq.lit - 0.5f) < 0.01f && fq.waxing, "first quarter: lit %.3f waxing %d", fq.lit, fq.waxing);
    /* last quarter 2024-05-01 11:27 UT: half lit, waning */
    const sky_moon_t lq = sky_moon_at(utc(2024, 5, 1, 11, 27));
    CHECK(fabsf(lq.lit - 0.5f) < 0.01f && !lq.waxing, "last quarter: lit %.3f waxing %d", lq.lit, lq.waxing);
}

static void distances(void) {
    static const struct { int y, mo, d, h, mi; float km; } D[] = {
        { 2016, 11, 14, 11, 23, 356512.f },      /* perigee, the year's nearest */
        { 2019, 1, 21, 19, 58, 357345.f },       /* perigee */
        { 2019, 9, 13, 13, 32, 406378.f },       /* apogee */
    };
    for (size_t q = 0; q < sizeof(D) / sizeof(D[0]); q++) {
        const sky_moon_t m = sky_moon_at(utc(D[q].y, D[q].mo, D[q].d, D[q].h, D[q].mi));
        CHECK(fabsf(m.km - D[q].km) < 300.f, "%04d-%02d-%02d: %.0f km, want %.0f", D[q].y, D[q].mo, D[q].d, m.km, D[q].km);
    }
    /* the size: held at the ends, 0.75 on an average night, rounded to whole dots */
    CHECK(sky_size_k(350000.f) == 1.f && sky_size_k(410000.f) == 0.7f && fabsf(sky_size_k(384400.f) - 0.75f) < 1e-4f, "the size table");
    CHECK(sky_moon_rdots(356500.f, W) == 10 && sky_moon_rdots(384400.f, W) == 8 && sky_moon_rdots(406500.f, W) == 7, "radius in dots: %d %d %d",
          sky_moon_rdots(356500.f, W), sky_moon_rdots(384400.f, W), sky_moon_rdots(406500.f, W));
    /* a supermoon (full near perigee) at the largest and full; a micromoon (full near apogee) at the smallest */
    const sky_moon_t sup = sky_moon_at(utc(2016, 11, 14, 12, 0)), mic = sky_moon_at(utc(2019, 9, 14, 12, 0));
    CHECK(sup.lit > 0.99f && sky_moon_rdots(sup.km, W) == 10, "supermoon 2016-11-14: lit %.3f, %d dots", sup.lit, sky_moon_rdots(sup.km, W));
    CHECK(mic.lit > 0.99f && sky_moon_rdots(mic.km, W) == 7, "micromoon 2019-09-14: lit %.3f, %d dots", mic.lit, sky_moon_rdots(mic.km, W));
}

/* the dots of cells c[0..n): their centre, extent and the lit side */
typedef struct { int n; float cx, cy, x0, x1, y0, y1; } spread_t;
static spread_t spread(const sky_cell_t *c, int n) {
    spread_t s = { 0, 0.f, 0.f, 1e9f, -1e9f, 1e9f, -1e9f };
    for (int q = 0; q < n; q++) for (int b = 0; b < 8; b++) {
        const int col = b < 6 ? b / 3 : b - 6, row = b < 6 ? b % 3 : 3;
        if (!(c[q].bits & (1 << b))) continue;
        const float x = ((c[q].k % W) * 2 + col + 0.5f) / (2 * W), y = ((c[q].k / W) * 4 + row + 0.5f) / (4 * H);
        s.n++; s.cx += x; s.cy += y;
        s.x0 = fminf(s.x0, x); s.x1 = fmaxf(s.x1, x); s.y0 = fminf(s.y0, y); s.y1 = fmaxf(s.y1, y);
    }
    if (s.n) { s.cx /= s.n; s.cy /= s.n; }
    return s;
}

static void moon_cells(void) {
    sky_cell_t c[SKY_MOON_CELLS];
    int rd;
    /* full, everyday size: centred where option A put it, 75 % of its radius as whole dots */
    sky_moon_t m = { 180.0, 1.f, true, 14.8f, 384400.f };
    int n = sky_moon_cells(&m, W, H, c, SKY_MOON_CELLS, &rd);
    spread_t s = spread(c, n);
    const float r = (float)rd / (2 * W);
    CHECK(n > 0 && fabsf(s.cx - SKY_MOON_X) < 0.006f && fabsf(s.cy - SKY_MOON_Y) < 0.006f, "full moon centred at (%.3f, %.3f)", s.cx, s.cy);
    CHECK(fabsf((s.x1 - s.x0) * 0.5f - r) < 1.f / (2 * W) && fabsf((s.y1 - s.y0) * 0.5f - r) < 1.f / (4 * H) + 0.004f,
          "round, radius %.4f across %.4f down, want %.4f", (s.x1 - s.x0) * 0.5f, (s.y1 - s.y0) * 0.5f, r);
    CHECK(fabsf(r - 0.75f * SKY_MOON_R) < 0.5f / (2 * W) + 0.003f, "75 %% of option A: %.4f of %.4f", r, SKY_MOON_R);
    /* the brightest dot under the cat's eyes (their luma ~205) and the moon not black anywhere it shows */
    float top = 0.f;
    for (int q = 0; q < n; q++) {
        const float l = 0.3f * (c[q].rgb >> 16 & 255) + 0.59f * (c[q].rgb >> 8 & 255) + 0.11f * (c[q].rgb & 255);
        top = fmaxf(top, l);
        CHECK(l > 15.f, "a moon cell too dark to see: %.0f", l);
    }
    CHECK(top > 80.f && top < 130.f, "quiet: its brightest at luma %.0f (option A quiet: 107)", top);
    /* the largest still clears the glass's rim (8 px of 466 at least) */
    sky_moon_t big = { 180.0, 1.f, true, 14.8f, 356000.f };
    n = sky_moon_cells(&big, W, H, c, SKY_MOON_CELLS, &rd);
    s = spread(c, n);
    float far = 0.f;
    for (int q = 0; q < n; q++) for (int b = 0; b < 8; b++) if (c[q].bits & (1 << b)) {
        const int col = b < 6 ? b / 3 : b - 6, row = b < 6 ? b % 3 : 3;
        const float x = ((c[q].k % W) * 2 + col + 0.5f) / (2 * W) - 0.5f, y = ((c[q].k / W) * 4 + row + 0.5f) / (4 * H) - 0.5f;
        far = fmaxf(far, sqrtf(x * x + y * y));
    }
    CHECK(rd == 10 && far < 0.5f - 8.f / 466.f, "the largest (%d dots) inside the rim: %.3f of 0.5", rd, far);
    /* waxing quarter: lit on the right (seen from the north); waning: on the left */
    sky_moon_t wax = { 90.0, 0.5f, true, 7.4f, 384400.f }, wane = { 270.0, 0.5f, false, 22.1f, 384400.f };
    const spread_t a = spread(c, sky_moon_cells(&wax, W, H, c, SKY_MOON_CELLS, NULL));
    const spread_t b = spread(c, sky_moon_cells(&wane, W, H, c, SKY_MOON_CELLS, NULL));
    CHECK(a.cx > SKY_MOON_X + 0.01f && b.cx < SKY_MOON_X - 0.01f, "waxing lit right (%.3f), waning left (%.3f), of %.3f", a.cx, b.cx, SKY_MOON_X);
    /* fewer dots as it thins; a new moon shows none */
    sky_moon_t thin = { 30.0, 0.067f, true, 2.5f, 384400.f }, none = { 1.0, 0.0001f, true, 0.1f, 384400.f };
    const spread_t t = spread(c, sky_moon_cells(&thin, W, H, c, SKY_MOON_CELLS, NULL));
    CHECK(t.n > 0 && t.n < a.n, "a crescent (%d dots) under a quarter (%d)", t.n, a.n);
    CHECK(sky_moon_cells(&none, W, H, c, SKY_MOON_CELLS, NULL) == 0, "a new moon: nothing");
}

static int popcount(const sky_cell_t *c, int n) { int k = 0; for (int q = 0; q < n; q++) k += __builtin_popcount(c[q].bits); return k; }

static void stars(void) {
    static sky_t s;
    const double now = utc(2026, 11, 22, 21, 0);
    const float dt = 1.f / 30.f;
    /* the first one: within the window, seeded (the same seed, the same wait; another seed, another) */
    sky_init(&s, 1234u);
    const float w1 = s.star.wait;
    sky_t s2; sky_init(&s2, 1234u);
    sky_t s3; sky_init(&s3, 99u);
    CHECK(w1 >= SKY_STAR_GAP_MIN_S && w1 <= SKY_STAR_GAP_MAX_S && s2.star.wait == w1 && s3.star.wait != w1, "first wait %.1f s (%.1f, %.1f)", w1, s2.star.wait, s3.star.wait);
    int f = 0, shown = 0, most = 0, last_start = -1, gaps = 0;
    float px0 = -1.f, py0 = -1.f, pux = 0.f;
    bool was = false;
    for (; f < 30 * 3600; f++) {                 /* an hour of the cat on the glass */
        sky_frame(&s, now + f * dt, W, H, dt, true, true);
        const bool on = s.star.t >= 0.f;
        if (on && !was) {
            if (last_start >= 0) {
                const float gap = (f - last_start) * dt;
                CHECK(gap >= SKY_STAR_GAP_MIN_S && gap <= SKY_STAR_GAP_MAX_S + SKY_STAR_DUR_MAX_S + SKY_STAR_FADE_S + 1.f, "a gap of %.0f s", gap);
                gaps++;
            } else CHECK(fabsf(f * dt - w1) < 2 * dt, "the first one at %.2f s, its wait %.2f s", f * dt, w1);
            CHECK(px0 < 0.f || fabsf(s.star.x0 - px0) + fabsf(s.star.y0 - py0) > 0.08f || (s.star.ux > 0.f) != (pux > 0.f), "not the last path again");
            /* the path: from the upper sky down at a slant, above the horizon, left or right of the cat, inside the glass */
            const float x1 = s.star.x0 + s.star.len * s.star.ux, y1 = s.star.y0 + s.star.len * s.star.uy;
            CHECK(s.star.uy > 0.3f && s.star.uy < 0.85f && y1 <= 0.40f && s.star.y0 < 0.3f, "a slant down: uy %.2f, ends at y %.2f", s.star.uy, y1);
            CHECK((s.star.x0 < 0.34f && x1 < 0.34f) || (s.star.x0 > 0.66f && x1 > 0.66f), "beside the cat: x %.2f to %.2f", s.star.x0, x1);
            px0 = s.star.x0; py0 = s.star.y0; pux = s.star.ux;
            last_start = f; shown++;
        }
        if (on) {
            const int life = f - last_start;
            CHECK(life * dt <= s.star.dur + SKY_STAR_FADE_S + dt, "it lasts %.2f s", life * dt);
            const int dots = popcount(s.star_c, s.nstar);
            most = dots > most ? dots : most;
            CHECK(dots <= SKY_STAR_DOTS && s.nstar <= SKY_STAR_CELLS, "%d dots in %d cells", dots, s.nstar);
            for (int q = 0; q < s.nstar; q++) {   /* never brighter than the head's level */
                const float l = 0.3f * (s.star_c[q].rgb >> 16 & 255) + 0.59f * (s.star_c[q].rgb >> 8 & 255) + 0.11f * (s.star_c[q].rgb & 255);
                CHECK(l <= SKY_STAR_HEAD * 255.f + 1.f && l < 200.f, "a star cell at luma %.0f", l);
            }
        }
        was = on;
    }
    CHECK(shown >= 3 && shown <= 16 && gaps == shown - 1, "%d stars in an hour", shown);
    CHECK(most >= 8, "a star shows (%d dots at most)", most);
    /* not allowed: one showing goes out at once, and the wait does not count */
    sky_init(&s, 7u);
    s.star.wait = 0.01f;
    for (int i = 0; i < 6; i++) sky_frame(&s, now, W, H, dt, true, true);
    CHECK(s.star.t > 0.f && s.nstar > 0, "a star runs");
    sky_frame(&s, now, W, H, dt, true, false);
    CHECK(s.star.t < 0.f && s.nstar == 0, "a question (or a listen, a say...) puts it out at once");
    const float wait = s.star.wait;
    for (int i = 0; i < 3000; i++) sky_frame(&s, now, W, H, dt, true, false);
    CHECK(s.star.wait == wait && s.star.t < 0.f, "the wait stands while none may show");
    /* no time, or the setting off: nothing at all, and no random number drawn */
    sky_init(&s, 7u);
    const uint32_t rng = s.star.rng;
    s.star.wait = 0.01f;
    for (int i = 0; i < 60; i++) sky_frame(&s, 0.0, W, H, dt, true, true);
    CHECK(!s.show && s.nmoon_shown == 0 && s.nstar == 0 && s.star.rng == rng && s.star.t < 0.f, "no time: no sky");
    sky_set(&s, false);
    for (int i = 0; i < 60; i++) sky_frame(&s, now, W, H, dt, true, true);
    CHECK(!s.show && s.nmoon_shown == 0 && s.nstar == 0 && s.star.rng == rng, "the setting off: no sky");
    sky_set(&s, true);
    sky_frame(&s, now, W, H, dt, true, false);
    CHECK(s.show && s.nmoon_shown > 0 && s.nstar == 0, "on again: the moon");
    sky_frame(&s, now, W, H, dt, false, false);
    CHECK(!s.show && s.nmoon_shown == 0, "the creature not on the glass: no moon");
    /* the moon made once a day: the same cells all day, new ones the next */
    sky_frame(&s, now - 20 * 3600.0, W, H, dt, true, false);
    const int64_t day = s.day;
    sky_frame(&s, now + 4 * 3600.0, W, H, dt, true, false);
    CHECK(s.day == day + 1, "the next day, made again");
}

int main(void) {
    phases();
    distances();
    moon_cells();
    stars();
    printf("sky_test: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
