/*
 * fluid_test.c — host test for the "fluid" creature core (fluid_sim/target/draw.c).
 *
 * Checks: U+2800 braille bit order, dither/contour mapping, sim stability (no NaN,
 * bounded height/energy, ink in [0,1], ink converges toward the target) for every
 * state at every preset grid, the renderer's dirty-cell contract, and a host timing
 * figure per step/frame (informative only — the device number comes from frame_ms).
 *
 * Build + run: test/host/fluid_build.sh
 */
#include "fluid_sim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int s_fail = 0, s_pass = 0;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const uint8_t *stub_font(char c) {
    static const uint8_t full[7] = { 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f }, blank[7] = { 0 };
    return c == ' ' ? blank : full;
}

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ── braille ─────────────────────────────────────────────────────────────── */

static void test_braille(void) {
    /* dot (col,row) → U+2800 bit */
    static const int expect[2][4] = { { 0, 1, 2, 6 }, { 3, 4, 5, 7 } };
    for (int c = 0; c < 2; c++) for (int r = 0; r < 4; r++)
        CHECK(fluid_braille_bit(c, r) == expect[c][r], "bit(%d,%d)=%d want %d", c, r, fluid_braille_bit(c, r), expect[c][r]);

    /* contour with exactly one bright sub-sample lights exactly that dot */
    for (int q = 0; q < 8; q++) {
        float sub[8] = { 0 }; sub[q] = 1.f;
        uint8_t m = fluid_braille_contour(sub);
        CHECK(m == (1u << expect[q >> 2][q & 3]), "contour q=%d → 0x%02x", q, m);
    }
    /* whole glyphs: left column ⡇ U+2847, right column ⢸ U+28B8, top row ⠉ U+2809 */
    float left[8] = { 1, 1, 1, 1, 0, 0, 0, 0 }, right[8] = { 0, 0, 0, 0, 1, 1, 1, 1 }, top[8] = { 1, 0, 0, 0, 1, 0, 0, 0 };
    CHECK(0x2800 + fluid_braille_contour(left) == 0x2847, "left col U+%04X", 0x2800 + fluid_braille_contour(left));
    CHECK(0x2800 + fluid_braille_contour(right) == 0x28B8, "right col U+%04X", 0x2800 + fluid_braille_contour(right));
    CHECK(0x2800 + fluid_braille_contour(top) == 0x2809, "top row U+%04X", 0x2800 + fluid_braille_contour(top));

    /* dither: 0 → no dots, 1 → all 8 (U+28FF), and dot count rises with the level */
    fluid_draw_t *d = fluid_draw_create(52, 32, FLUID_STAGE_REF, FLUID_STAGE_REF);   /* builds the gamma LUT */
    float z[8] = { 0 }, o[8]; for (int q = 0; q < 8; q++) o[q] = 1.f;
    CHECK(fluid_braille_dither(z, 3, 5, 0.f, 0) == 0x00, "dither(0)");
    CHECK(fluid_braille_dither(o, 3, 5, 0.f, 0) == 0xFF, "dither(1)");
    int prev = -1, mono = 1;
    for (int lv = 0; lv <= 16; lv++) {
        float sub[8]; for (int q = 0; q < 8; q++) sub[q] = lv / 16.f;
        int tot = 0;
        for (int sj = 0; sj < 4; sj++) for (int si = 0; si < 4; si++) tot += __builtin_popcount(fluid_braille_dither(sub, si, sj, 0.f, 0));
        if (tot < prev) mono = 0;
        prev = tot;
    }
    CHECK(mono, "dither dot count not monotonic in level");
    fluid_draw_destroy(d);
}

/* ── sim ─────────────────────────────────────────────────────────────────── */

/* int16 storage saturates instead of going NaN/inf: "finite" = never pinned at the rail */
static int unsaturated(const int16_t *a, int n) { for (int k = 0; k < n; k++) if (a[k] >= 32767 || a[k] <= -32767) return 0; return 1; }

enum { ST_BLOB, ST_RIPPLE, ST_VORTEX, ST_WAVE, ST_TEXT, ST_N };
static const char *ST_NAME[ST_N] = { "blob", "effect:ripple", "effect:vortex", "effect:wave", "text" };

static void run_state(int W, int H, int st, double *step_ms, double *frame_ms) {
    fluid_sim_t *s = fluid_sim_create(W, H, FLUID_STAGE_REF);
    fluid_text_t *tx = fluid_text_create(s);
    fluid_draw_t *d = fluid_draw_create(W, H, FLUID_STAGE_REF, FLUID_STAGE_REF);
    fluid_frame_t *fr = fluid_frame_create(W, H);
    uint16_t *fb = calloc(FLUID_STAGE_REF * FLUID_STAGE_REF, 2);
    CHECK(s && tx && d && fr && fb, "alloc %dx%d", W, H);
    if (!s || !tx || !d || !fr || !fb) return;
    s->p.tilt_x = 6.f; s->p.tilt_y = -3.f;                 /* exercise the IMU bias */
    s->vibe.amp = 0.9f; s->vibe.freq = 9.7f;               /* and the thinking shimmer */
    const char *word = "molty.";
    fluid_text_set_sentence(tx, 4.5f);
    fluid_text_set_word(tx, s, word, (int)strlen(word), 1);
    int fx = st - ST_RIPPLE; float fxs = 0.f;
    if (st >= ST_RIPPLE && st <= ST_WAVE) fluid_effect_enter(s, fx);
    fluid_sim_impulse(s, 0.2f, -0.1f, 1.6f, 0.09f);
    fluid_sim_push(s, -0.3f, 0.2f, 40.f, -20.f, 0.16f);
    const float dt = 1.f / 60.f;
    double tstep = 0, tframe = 0; int nstep = 0, nframe = 0;
    float hmax = 0.f, hmid = 0.f, hlate = 0.f, err_end = 1.f;
    fluid_draw_opts_t o = { FLUID_PAL_PASTEL, FLUID_EDGE_ALL, 0.12f, 0.f, 0.f, tx->letters, tx->nletters, " .,:;!+=*xX#@", stub_font };
    for (int f = 0; f < 300; f++) {                         /* 10 s at 30 fps, 2 steps/frame */
        float t = f / 30.f, breath = 0.5f - 0.5f * cosf(t / 4.f * 6.2832f), swell = 1.f + 0.05f * (breath * 2.f - 1.f);
        s->p.hold = fminf(1.f, t / 1.5f);
        double t0 = now_ms();
        switch (st) {
        case ST_BLOB: fluid_target_blob(s, t, swell, 0.02f, 0.f); break;
        case ST_TEXT: fluid_text_fill(tx, s, t); break;
        default: fluid_target_effect(s, fx, t, swell, 0.f, 0.f); fluid_effect_tick(s, fx, 1.f / 30.f, &fxs); break;
        }
        fluid_sim_background(s, t, breath, (f & 1) == 0);
        fluid_sim_swirl_field(s);
        double t1 = now_ms();
        fluid_sim_step(s, dt); fluid_sim_step(s, dt);
        fluid_sim_color(s, 2.f * dt);
        double t2 = now_ms();
        fluid_sim_view(s);
        int y0, y1; o.time = t;
        fluid_sim_snapshot(s, fr, f % 2);
        fluid_draw(d, fr, fb, &o, &y0, &y1);
        double t3 = now_ms();
        tstep += (t2 - t1) / 2; nstep++;
        tframe += t3 - t0; nframe++;
        if (f % 60 == 30) fluid_sim_impulse(s, 0.f, 0.f, 1.4f, 0.12f);
        for (int k = 0; k < s->N; k++) {
            float a = fabsf(s->h[k] / FQ_H);
            if (a > hmax) hmax = a;
            if (f >= 90 && f < 180 && a > hmid) hmid = a;     /* 3-6 s */
            if (f >= 210 && a > hlate) hlate = a;             /* 7-10 s */
        }
        err_end = s->err;
    }
    int ok = unsaturated(s->h, s->N) && unsaturated(s->u, s->N) && unsaturated(s->v, s->N)
          && unsaturated(s->phi, s->N) && isfinite(s->energy) && isfinite(s->err);
    CHECK(ok, "%dx%d %s: field saturated / non-finite", W, H, ST_NAME[st]);
    float imin = 1.f, imax = 0.f;
    for (int k = 0; k < s->N; k++) { float v = s->ink[k] / FQ_UNIT; if (v < imin) imin = v; if (v > imax) imax = v; }
    CHECK(imin >= 0.f && imax <= 1.f, "%dx%d %s: ink out of range [%f,%f]", W, H, ST_NAME[st], imin, imax);
    /* bounded = no growth between the 3-6 s and 7-10 s windows (forced sheets settle
     * at a steady amplitude; an instability grows without limit) */
    CHECK(hmax < 30.f && hlate <= hmid * 1.25f + 0.5f, "%dx%d %s: height growing (mid %f late %f max %f)",
          W, H, ST_NAME[st], hmid, hlate, hmax);
    CHECK(s->energy >= 0.f && s->energy <= 1.f, "%dx%d %s: energy %f", W, H, ST_NAME[st], s->energy);
    CHECK(err_end < 0.12f, "%dx%d %s: ink did not converge (err %f)", W, H, ST_NAME[st], err_end);
    printf("  %dx%d %-14s err %.3f  |h|max %.2f  energy %.2f  step %.3f ms  frame %.3f ms (host)\n",
           W, H, ST_NAME[st], err_end, hmax, s->energy, tstep / nstep, tframe / nframe);
    if (step_ms) *step_ms = tstep / nstep;
    if (frame_ms) *frame_ms = tframe / nframe;
    free(fb); fluid_frame_destroy(fr); fluid_draw_destroy(d); fluid_text_destroy(tx); fluid_sim_destroy(s);
}

/* a quiescent field re-drawn at the same time stamp must touch nothing */
static void test_dirty(void) {
    fluid_sim_t *s = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    fluid_draw_t *d = fluid_draw_create(52, 32, FLUID_STAGE_REF, FLUID_STAGE_REF);
    fluid_frame_t *fr = fluid_frame_create(52, 32);
    uint16_t *fb = calloc(FLUID_STAGE_REF * FLUID_STAGE_REF, 2);
    for (int f = 0; f < 60; f++) { fluid_target_blob(s, 1.f, 1.f, 0, 0); fluid_sim_background(s, 1.f, 0.5f, true); fluid_sim_swirl_field(s); fluid_sim_step(s, 1.f / 60); fluid_sim_color(s, 1.f / 60); }
    fluid_sim_view(s);
    fluid_sim_snapshot(s, fr, true);
    fluid_draw_opts_t o = { FLUID_PAL_GREEN, FLUID_EDGE_CONTOURS, 0.f, 2.f, 0.f, NULL, 0, " .:-=+*#", stub_font };
    int y0, y1;
    int n1 = fluid_draw(d, fr, fb, &o, &y0, &y1);
    CHECK(n1 > 50 && y1 > y0, "first draw drew %d cells, band [%d,%d)", n1, y0, y1);
    long lit = 0; for (int p = 0; p < FLUID_STAGE_REF * FLUID_STAGE_REF; p++) lit += fb[p] != 0;
    CHECK(lit > 1000, "first draw lit only %ld px", lit);
    fluid_draw(d, fr, fb, &o, &y0, &y1);
    CHECK(y0 == 0 && y1 == 0, "unchanged frame dirtied band [%d,%d)", y0, y1);
    /* corner pixel (outside the disc) stays black */
    CHECK(fb[0] == 0 && fb[FLUID_STAGE_REF * FLUID_STAGE_REF - 1] == 0, "corner pixel drawn");
    o.edge_mode = FLUID_EDGE_OFF;                           /* ASCII path */
    int n3 = fluid_draw(d, fr, fb, &o, &y0, &y1);
    CHECK(n3 > 50, "ascii draw drew %d", n3);
    free(fb); fluid_frame_destroy(fr); fluid_draw_destroy(d); fluid_sim_destroy(s);
}

/* orp < 0 = static label (the pairing code): centred, no accent letter */
static void test_static_label(void) {
    fluid_sim_t *s = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    fluid_text_t *tx = fluid_text_create(s);
    fluid_text_set_sentence(tx, 2.f);
    fluid_text_set_word(tx, s, "4821", 4, -1);
    int acc = 0; for (int i = 0; i < tx->nletters; i++) acc += tx->letters[i].acc;
    const fluid_letter_t *a = &tx->letters[0], *z = &tx->letters[3];
    float mid = (a->x + z->x + z->w) / 2.f;
    CHECK(tx->nletters == 4 && acc == 0, "static label: %d letters, %d accented", tx->nletters, acc);
    CHECK(fabsf(mid - FLUID_STAGE_REF / 2.f) < 0.01f, "static label not centred (mid %f)", mid);
    fluid_text_set_word(tx, s, "4821", 4, 1);
    CHECK(tx->letters[1].acc && !tx->letters[0].acc, "RSVP word lost its ORP accent");
    fluid_text_destroy(tx); fluid_sim_destroy(s);
}

/* other panels: the board sizes the fluid at runtime. A 240×240 round panel (the
 * Waveshare 1.28) and a 320×240 rectangle (stage = the centred 240 square) must draw a
 * live fluid that stays inside the stage's disc, with the 1.46's dot geometry. */
static void test_panel(int fb_w, int fb_h) {
    const int stage = fb_w < fb_h ? fb_w : fb_h, ox = (fb_w - stage) / 2, oy = (fb_h - stage) / 2;
    int W, H;
    fluid_grid_for_stage(stage, &W, &H);
    fluid_sim_t *s = fluid_sim_create(W, H, stage);
    fluid_text_t *tx = s ? fluid_text_create(s) : NULL;
    fluid_draw_t *d = fluid_draw_create(W, H, fb_w, fb_h);
    fluid_draw_t *ref = fluid_draw_create(52, 32, FLUID_STAGE_REF, FLUID_STAGE_REF);
    fluid_frame_t *fr = fluid_frame_create(W, H);
    uint16_t *fb = calloc((size_t)fb_w * fb_h, 2);
    CHECK(s && tx && d && ref && fr && fb, "%dx%d: alloc (grid %dx%d)", fb_w, fb_h, W, H);
    if (!s || !tx || !d || !ref || !fr || !fb) return;
    /* same cell size in px as the 1.46's 52×32, so braille dots keep their geometry */
    CHECK(fabsf((float)stage / W - 412.f / 52) < 0.35f && fabsf((float)stage / H - 412.f / 32) < 0.7f,
          "%dx%d: grid %dx%d cells %.2fx%.2f px", fb_w, fb_h, W, H, (float)stage / W, (float)stage / H);
    CHECK(d->dot_w == ref->dot_w && d->dot_h == ref->dot_h, "%dx%d: dots %dx%d, 1.46 %dx%d",
          fb_w, fb_h, d->dot_w, d->dot_h, ref->dot_w, ref->dot_h);
    CHECK(d->cx[0] == ox && d->cx[W] == ox + stage && d->cy[0] == oy && d->cy[H] == oy + stage,
          "%dx%d: cell edges [%d,%d]x[%d,%d]", fb_w, fb_h, d->cx[0], d->cx[W], d->cy[0], d->cy[H]);
    fluid_text_set_sentence(tx, 3.f);
    fluid_text_set_word(tx, s, "molty", 5, 1);
    float lx0 = tx->letters[0].x, lx1 = tx->letters[4].x + tx->letters[4].w;
    CHECK(lx0 > 0.f && lx1 < stage && tx->letters[0].fs >= 18.f, "%dx%d: word spans [%.1f,%.1f] of %d, fs %.1f",
          fb_w, fb_h, lx0, lx1, stage, tx->letters[0].fs);
    fluid_draw_opts_t o = { FLUID_PAL_PASTEL, FLUID_EDGE_ALL, 0.12f, 0.f, 0.f, NULL, 0, " .:-=+*#", stub_font };
    long lit = 0, outside = 0, band_bad = 0;
    for (int f = 0; f < 90; f++) {
        float t = f / 30.f;
        s->p.hold = fminf(1.f, t / 1.5f);
        if (f < 45) fluid_target_blob(s, t, 1.f, 0.f, 0.f); else fluid_text_fill(tx, s, t);
        fluid_sim_background(s, t, 0.5f, (f & 1) == 0);
        fluid_sim_swirl_field(s);
        fluid_sim_step(s, 1.f / 60); fluid_sim_step(s, 1.f / 60);
        fluid_sim_color(s, 1.f / 30);
        fluid_sim_view(s);
        fluid_sim_snapshot(s, fr, false);
        o.time = t;
        o.edge_mode = f < 60 ? FLUID_EDGE_ALL : FLUID_EDGE_OFF;
        o.letters = f >= 45 ? tx->letters : NULL; o.nletters = f >= 45 ? tx->nletters : 0;
        int y0, y1;
        fluid_draw(d, fr, fb, &o, &y0, &y1);
        if (y1 > y0 && (y0 < oy || y1 > oy + stage)) band_bad++;
    }
    /* the mask is per cell (centre inside the disc), so a rim cell's dots may poke out by
     * up to half a cell — on any panel, the 412 included; nothing may go further */
    const float r = stage / 2.f + 0.5f * stage / W + 1.f, cx = ox + stage / 2.f, cy = oy + stage / 2.f;
    for (int y = 0; y < fb_h; y++) for (int x = 0; x < fb_w; x++) {
        if (!fb[(size_t)y * fb_w + x]) continue;
        lit++;
        float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
        if (dx * dx + dy * dy > r * r) outside++;
    }
    CHECK(lit > stage * stage / 20, "%dx%d: only %ld px lit", fb_w, fb_h, lit);
    CHECK(outside == 0, "%dx%d: %ld lit px outside the stage disc", fb_w, fb_h, outside);
    CHECK(band_bad == 0, "%dx%d: dirty band left the stage %ld times", fb_w, fb_h, band_bad);
    printf("  panel %dx%d: grid %dx%d, %ld px lit, inside the disc\n", fb_w, fb_h, W, H, lit);
    free(fb); fluid_frame_destroy(fr); fluid_draw_destroy(ref); fluid_draw_destroy(d); fluid_text_destroy(tx); fluid_sim_destroy(s);
}

int main(void) {
    printf("fluid host test\n");
    test_static_label();
    test_braille();
    test_dirty();
    static const int grids[3][2] = { { 44, 27 }, { 52, 32 }, { 60, 37 } };
    for (int g = 0; g < 3; g++) for (int st = 0; st < ST_N; st++) run_state(grids[g][0], grids[g][1], st, NULL, NULL);
    CHECK(fluid_sim_create(8, 8, FLUID_STAGE_REF) == NULL, "out-of-range grid accepted");
    { int gw, gh; fluid_grid_for_stage(FLUID_STAGE_REF, &gw, &gh); CHECK(gw == 52 && gh == 32, "412 default grid %dx%d", gw, gh); }
    test_panel(240, 240);
    test_panel(320, 240);
    test_panel(FLUID_STAGE_REF, FLUID_STAGE_REF);
    printf("%d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
