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
#include "fluid_ask.h"
#include "fluid_ring.h"
#include "glyph_font.h"

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

/* the status mark: nothing for NONE; otherwise a few pixels at the bottom of the stage,
 * inside the round panel, and nowhere else */
static void test_mark(int fb_w, int fb_h) {
    fluid_draw_t *d = fluid_draw_create(52, 32, fb_w, fb_h);
    uint16_t *fb = calloc((size_t)fb_w * fb_h, sizeof(uint16_t));
    CHECK(d && fb, "mark alloc");
    if (!d || !fb) { free(fb); fluid_draw_destroy(d); return; }
    CHECK(fluid_draw_mark(d, fb, FLUID_PAL_PASTEL, FLUID_MARK_NONE) == 0, "NONE draws");
    for (int m = FLUID_MARK_ARC; m < FLUID_MARK_COUNT; m++) {
        memset(fb, 0, (size_t)fb_w * fb_h * sizeof(uint16_t));
        int n = fluid_draw_mark(d, fb, FLUID_PAL_GREEN, m), lit = 0, stray = 0;
        float cx = d->ox + d->stage / 2.f, cy = d->oy + d->stage / 2.f, R = d->stage / 2.f;
        for (int y = 0; y < fb_h; y++) for (int x = 0; x < fb_w; x++) {
            if (!fb[y * fb_w + x]) continue;
            lit++;
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (dy < R * 0.8f || dx * dx + dy * dy > R * R || fabsf(dx) > R * 0.2f) stray++;
        }
        CHECK(n == lit && n >= 9 && n <= 600, "%dx%d mark %d: %d px (%d lit)", fb_w, fb_h, m, n, lit);
        CHECK(stray == 0, "%dx%d mark %d: %d px off the bottom edge", fb_w, fb_h, m, stray);
    }
    free(fb);
    fluid_draw_destroy(d);
}

/* a question's labels: every letter in order, centred on its spot; only shown labels
 * pool ink; the accent follows acc */
static void test_labels(void) {
    fluid_sim_t *s = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    fluid_text_t *tx = fluid_text_create(s);
    const char *txt[5] = { "ABCDEFGH", "IJKLMNOP", "QRSTUVWX", "YZ012345", "6789 #@!" };
    int lens[5] = { 8, 8, 8, 8, 8 };
    float cx[5], cy[5], fs = fluid_ask_layout(5, lens, FLUID_STAGE_REF, cx, cy);
    fluid_label_t lb[5];
    for (int i = 0; i < 5; i++) lb[i] = (fluid_label_t){ txt[i], 8, cx[i], cy[i], i != 2, i == 3, 0 };
    fluid_text_set_labels(tx, s, lb, 5, fs);
    CHECK(tx->nletters == 40, "5×8 labels: %d letters", tx->nletters);
    const float cw = (float)FLUID_STAGE_REF / s->W, ch = (float)FLUID_STAGE_REF / s->H;
    for (int i = 0; i < 5; i++) {
        const fluid_letter_t *a = &tx->letters[i * 8], *z = &tx->letters[i * 8 + 7];
        CHECK(a->ch == txt[i][0] && z->ch == txt[i][7] && a->melt == 0.f, "label %d letters", i);
        CHECK(fabsf((a->x + z->x + z->w) / 2.f - cx[i]) < 0.01f && fabsf(a->y + a->h / 2.f - cy[i]) < 0.01f, "label %d off its spot", i);
        CHECK(a->acc == (i == 3), "label %d accent %d", i, a->acc);
        /* ink under the label's middle letter (D/E boundary: take letter 3's centre) */
        const fluid_letter_t *m = &tx->letters[i * 8 + 3];
        int k = (int)((m->y + m->h / 2.f) / ch) * s->W + (int)((m->x + m->w / 2.f) / cw);
        CHECK((tx->cov[k] > 0.f) == (i != 2), "label %d cov %.2f (shown %d)", i, tx->cov[k], i != 2);
        CHECK((tx->orp[k] > 0.f) == (i == 3), "label %d accent ink %.2f", i, tx->orp[k]);
    }
    fluid_text_destroy(tx); fluid_sim_destroy(s);
}

/* overlays: the dividers on their rays; dirtying the last overlay before a draw restores
 * exactly what it covered, and a repeated overlay over freshly redrawn cells does not
 * compound */
static void test_overlay(void) {
    const int P = FLUID_STAGE_REF;
    fluid_sim_t *s = fluid_sim_create(52, 32, P);
    fluid_draw_t *d = fluid_draw_create(52, 32, P, P), *ref = fluid_draw_create(52, 32, P, P);
    fluid_frame_t *fr = fluid_frame_create(52, 32);
    uint16_t *fb = calloc((size_t)P * P, 2), *fb_ref = calloc((size_t)P * P, 2), *fb1 = calloc((size_t)P * P, 2);
    for (int f = 0; f < 60; f++) { fluid_target_blob(s, 1.f, 1.f, 0, 0); fluid_sim_background(s, 1.f, 0.5f, true); fluid_sim_swirl_field(s); fluid_sim_step(s, 1.f / 60); fluid_sim_color(s, 1.f / 60); }
    fluid_sim_view(s);
    fluid_sim_snapshot(s, fr, false);
    fluid_draw_opts_t o = { FLUID_PAL_PASTEL, FLUID_EDGE_ALL, 0.12f, 2.f, 0.f, NULL, 0, " .:-=+*#", stub_font };
    int y0, y1, n;
    long changed, stray;
    fluid_draw(ref, fr, fb_ref, &o, &y0, &y1);                       /* the fluid alone */
    fluid_draw(d, fr, fb, &o, &y0, &y1);
    CHECK(!memcmp(fb, fb_ref, (size_t)P * P * 2), "same snapshot, different pixels");
    fluid_overlay_t none = { 0 }, divs = { 5, 1.f };
    CHECK(!fluid_overlay_on(&none) && fluid_overlay_on(&divs), "overlay_on");
    CHECK(fluid_draw_overlay(d, fb, o.palette, &none) == 0, "empty overlay drew");
    /* dividers: on the 5 rays only, and off every label box of a 5-option layout */
    fluid_draw_overlay_dirty(d, &divs);
    fluid_draw(d, fr, fb, &o, &y0, &y1);
    n = fluid_draw_overlay(d, fb, o.palette, &divs);
    int lens[5] = { 8, 8, 8, 8, 8 }; float cx[5], cy[5], fs = fluid_ask_layout(5, lens, P, cx, cy);
    changed = stray = 0; long in_label = 0;
    for (int y = 0; y < P; y++) for (int x = 0; x < P; x++) {
        if (fb[y * P + x] == fb_ref[y * P + x]) continue;
        changed++;
        float dx = x + 0.5f - P / 2.f, dy = y + 0.5f - P / 2.f, best = 1e9f;
        for (int q = 0; q < 5; q++) {
            float a = 6.2831853f * q / 5, ux = sinf(a), uy = -cosf(a), t = dx * ux + dy * uy;
            float px = dx - t * ux, py = dy - t * uy, dd = t > 0.f ? sqrtf(px * px + py * py) : 1e9f;
            if (dd < best) best = dd;
        }
        if (best > 1.6f) stray++;
        for (int i = 0; i < 5; i++)
            if (fabsf(x + 0.5f - cx[i]) < 8 * 0.6f * fs / 2 && fabsf(y + 0.5f - cy[i]) < 1.05f * fs / 2) in_label++;
    }
    CHECK(n > 500 && changed > 300 && stray == 0 && in_label == 0, "dividers: %d px, %ld changed, %ld off the rays, %ld in a label", n, changed, stray, in_label);
    fluid_draw_overlay_dirty(d, &divs);
    fluid_draw(d, fr, fb, &o, &y0, &y1);
    CHECK(!memcmp(fb, fb_ref, (size_t)P * P * 2), "the dividers left pixels behind");
    /* the same dividers again next frame: dirty + redraw + overlay = the same pixels */
    fluid_draw_overlay_dirty(d, &none); fluid_draw_overlay_dirty(d, &divs);
    fluid_draw(d, fr, fb, &o, &y0, &y1);
    fluid_draw_overlay(d, fb, o.palette, &divs);
    memcpy(fb1, fb, (size_t)P * P * 2);
    fluid_draw_overlay_dirty(d, &divs); fluid_draw_overlay_dirty(d, &divs);
    fluid_draw(d, fr, fb, &o, &y0, &y1);
    fluid_draw_overlay(d, fb, o.palette, &divs);
    CHECK(!memcmp(fb, fb1, (size_t)P * P * 2), "repeated dividers compounded");
    free(fb); free(fb_ref); free(fb1); fluid_frame_destroy(fr); fluid_draw_destroy(d); fluid_draw_destroy(ref); fluid_sim_destroy(s);
}


/* ── rings of the fluid: the listening ring and a question's coloured arcs ───── */

static void rgb_of(uint32_t c, float *r, float *g, float *b) { *r = (c >> 16 & 255) / 255.f; *g = (c >> 8 & 255) / 255.f; *b = (c & 255) / 255.f; }
/* the view (what the screen shows) and the carried colour at stage point (x, y) */
static int cell_of(const fluid_sim_t *s, float x, float y) {
    int i = (int)((x + 1.f) * 0.5f * s->W), j = (int)((y + 1.f) * 0.5f * s->H);
    i = i < 0 ? 0 : i >= s->W ? s->W - 1 : i; j = j < 0 ? 0 : j >= s->H ? s->H - 1 : j;
    return j * s->W + i;
}
/* run the sim f frames: the blob (the creature's stand-in) in the middle, then `arcs` */
typedef void (*arcs_fn)(fluid_sim_t *s, float t);
static void run_arcs(fluid_sim_t *s, int frames, arcs_fn arcs) {
    s->p.relax_up = 2.6f; s->p.relax_down = 3.4f;            /* mole_fluid's creature rates, */
    s->p.hold = 1.f; s->p.tint_rate = 12.f;                  /* settled (fd_solidity / the ask's ramp) */
    for (int f = 0; f < frames; f++) {
        float t = f / 30.f;
        fluid_target_blob(s, t, 1.f, 0.f, 0.f);
        if (arcs) arcs(s, t);
        fluid_sim_background(s, t, 0.5f, true); fluid_sim_swirl_field(s);
        fluid_sim_step(s, 1.f / 60); fluid_sim_step(s, 1.f / 60); fluid_sim_color(s, 2.f / 60);
    }
    fluid_sim_view(s);
}
static void ring_violet(fluid_sim_t *s, float t) { (void)t; fluid_target_arc(s, 0.875f, 0.02f, 0.f, 6.2831853f, 1.f, 0xb197fc, 0.f); }

static void test_fluid_ring(void) {
    fluid_sim_t *s = fluid_sim_create(52, 32, FLUID_STAGE_REF), *z = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    run_arcs(s, 45, ring_violet);
    run_arcs(z, 45, NULL);
    /* the ring condenses all the way round, just inside the rim, and the creature stays */
    int lit = 0, tinted = 0; float ink_min = 1.f, cr, cg, cb;
    rgb_of(0xb197fc, &cr, &cg, &cb);
    for (int q = 0; q < 24; q++) {
        float a = 6.2831853f * q / 24, x = 0.875f * sinf(a), y = -0.875f * cosf(a);
        int k = cell_of(s, x, y);
        float v = s->view[k] / FQ_UNIT, v0 = z->view[k] / FQ_UNIT;
        if (v < ink_min) ink_min = v;
        lit += v > 0.35f && v > v0 + 0.2f;
        const fluid_rgba16_t *c = &s->col[k];
        float r = c->r / 65535.f, g = c->g / 65535.f, b = c->b / 65535.f;
        tinted += fabsf(r - cr) < 0.12f && fabsf(g - cg) < 0.12f && fabsf(b - cb) < 0.12f;
    }
    CHECK(lit >= 22, "ring: %d/24 points condensed (min ink %.2f)", lit, ink_min);
    CHECK(tinted >= 22, "ring: %d/24 points carry its colour", tinted);
    int kc = cell_of(s, 0.f, 0.f);
    CHECK(s->view[kc] / FQ_UNIT > 0.3f && abs(s->view[kc] - z->view[kc]) / FQ_UNIT < 0.15f, "the creature in the middle stays (%.2f vs %.2f)", s->view[kc] / FQ_UNIT, z->view[kc] / FQ_UNIT);
    /* between the creature and the ring, the fluid is dim: a ring, not a filled disc */
    int kb = cell_of(s, 0.f, 0.62f);
    CHECK(s->view[kb] / FQ_UNIT < 0.3f, "between creature and ring: ink %.2f", s->view[kb] / FQ_UNIT);
    /* amt 0 or no width adds nothing */
    fluid_sim_t *e = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    fluid_target_blob(e, 0.f, 1.f, 0.f, 0.f);
    size_t N = (size_t)e->N;
    int16_t *T0 = malloc(N * 2); memcpy(T0, e->T, N * 2);
    fluid_target_arc(e, 0.875f, 0.02f, 0.f, 6.2831853f, 0.f, 0xffffff, 0.f);
    fluid_target_arc(e, 0.875f, 0.f, 0.f, 6.2831853f, 1.f, 0xffffff, 0.f);
    CHECK(!memcmp(T0, e->T, N * 2), "an empty ring changed the target");
    /* wider and stronger = more ink (the pulse and the mic swell) */
    fluid_target_arc(e, 0.875f, 0.016f, 0.f, 6.2831853f, 0.6f, 0xffffff, 0.f);
    double sum1 = 0; for (size_t k = 0; k < N; k++) sum1 += e->T[k];
    memcpy(e->T, T0, N * 2);
    fluid_target_arc(e, 0.875f, 0.036f, 0.f, 6.2831853f, 1.f, 0xffffff, 0.f);
    double sum2 = 0; for (size_t k = 0; k < N; k++) sum2 += e->T[k];
    CHECK(sum2 > sum1 * 1.3, "a wider, louder ring pools more ink (%.0f vs %.0f)", sum2, sum1);
    free(T0); fluid_sim_destroy(e);
    /* colour resolution: the listen's colour > the device default > the palette's accent */
    CHECK(fluid_ring_rgb(0x123456, 0xabcdef, FLUID_PAL_PASTEL) == 0x123456, "arg wins");
    CHECK(fluid_ring_rgb(-1, 0xabcdef, FLUID_PAL_PASTEL) == 0xabcdef, "then the default");
    CHECK(fluid_ring_rgb(-1, -1, FLUID_PAL_PASTEL) == 0xff8c3c && fluid_ring_rgb(-1, -1, FLUID_PAL_GREEN) == 0xffd65a, "then the accent (%06x)", (unsigned)fluid_ring_rgb(-1, -1, FLUID_PAL_PASTEL));
    CHECK(fluid_ring_rgb(0, 0xabcdef, FLUID_PAL_PASTEL) == 0, "black is a colour, not 'unset'");
    fluid_sim_destroy(s); fluid_sim_destroy(z);
    /* a listen right after `say`: text → creature restarts the hold ramp (0 for 0.7 s), and
     * the ring still shows within 0.3 s (its own local condensing) */
    fluid_sim_t *h = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    h->p.relax_up = 2.6f; h->p.relax_down = 3.4f; h->p.hold = 0.f; h->p.tint_rate = 3.f;
    for (int f = 0; f < 9; f++) {
        fluid_target_blob(h, f / 30.f, 1.f, 0.f, 0.f);
        fluid_target_arc(h, 0.875f, 0.018f, 0.f, 6.2831853f, 0.8f, 0xff8c3c, 0.f);
        fluid_sim_background(h, f / 30.f, 0.5f, true); fluid_sim_swirl_field(h);
        fluid_sim_step(h, 1.f / 60); fluid_sim_step(h, 1.f / 60); fluid_sim_color(h, 2.f / 60);
    }
    fluid_sim_view(h);
    double m = 0;
    for (int q = 0; q < 24; q++) { float a = 6.2831853f * q / 24; m += h->view[cell_of(h, 0.875f * sinf(a), -0.875f * cosf(a))] / FQ_UNIT; }
    CHECK(m / 24 > 0.22, "hold 0: the ring only reached ink %.2f in 0.3 s", m / 24);
    fluid_sim_destroy(h);
}

/* the listening ring (fluid_ring.h): thin and soft, breathes (in/out, never around), more with the voice */
static fluid_ring_t g_rg;
static float g_level;
static void ring_tick_fill(fluid_sim_t *s, float t) {
    (void)t;
    fluid_ring_tick(&g_rg, 1.f / 30, true, g_level);
    fluid_target_ring(s, &g_rg, 0xb197fc, 0.f);
}
static void ring_old(fluid_sim_t *s, float t) {        /* the ring before: a breathing band */
    (void)t;
    fluid_target_arc(s, 0.875f + 0.006f, 0.022f, 0.f, 6.2831853f, 0.8f, 0xb197fc, 0.f);
}
/* ink the ring adds over the plain sim, summed over the band 0.7..1 (the view and T) */
static void ring_excess(const fluid_sim_t *s, const fluid_sim_t *z, double *view, double *tgt) {
    *view = *tgt = 0;
    for (int j = 0; j < s->H; j++) for (int i = 0; i < s->W; i++) {
        int k = j * s->W + i;
        float r = sqrtf(s->xs[i] * s->xs[i] + s->ys[j] * s->ys[j]);
        if (!s->mask[k] || r < 0.7f) continue;
        *view += fmax(0, (s->view[k] - z->view[k]) / FQ_UNIT);
        *tgt  += fmax(0, (s->T[k] - z->T[k]) / FQ_UNIT);
    }
}
static void test_ring_ripples(void) {
    fluid_sim_t *s = fluid_sim_create(52, 32, FLUID_STAGE_REF), *z = fluid_sim_create(52, 32, FLUID_STAGE_REF),
                *o = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    g_rg = (fluid_ring_t){ 0 }; g_level = 0.f;
    run_arcs(s, 45, ring_tick_fill);
    run_arcs(z, 45, NULL);
    run_arcs(o, 45, ring_old);
    /* still a whole ring of the fluid in its colour, the creature untouched in the middle */
    int lit = 0, tinted = 0; float cr, cg, cb;
    rgb_of(0xb197fc, &cr, &cg, &cb);
    for (int q = 0; q < 24; q++) {
        float a = 6.2831853f * q / 24;
        int best = -1; float bv = -1.f;                    /* the ring's cell on this ray */
        for (float rr = 0.80f; rr <= 0.95f; rr += 0.01f) {
            int k = cell_of(s, rr * sinf(a), -rr * cosf(a));
            float v = (s->view[k] - z->view[k]) / FQ_UNIT;
            if (v > bv) { bv = v; best = k; }
        }
        lit += bv > 0.08f;
        const fluid_rgba16_t *c = &s->col[best];
        tinted += fabsf(c->r / 65535.f - cr) < 0.15f && fabsf(c->g / 65535.f - cg) < 0.15f && fabsf(c->b / 65535.f - cb) < 0.15f;
    }
    CHECK(lit >= 22, "calm ring: %d/24 rays show it", lit);
    CHECK(tinted >= 20, "calm ring: %d/24 rays carry its colour", tinted);
    int kc = cell_of(s, 0.f, 0.f);
    CHECK(abs(s->view[kc] - z->view[kc]) / FQ_UNIT < 0.15f, "the creature in the middle stays");
    /* thinner and softer than the old band: far less ink pooled at the edge */
    double vs, ts, vo, to;
    ring_excess(s, z, &vs, &ts); ring_excess(o, z, &vo, &to);
    if (getenv("RING_STATS")) printf("  ring excess: target %.1f (old %.1f), view %.1f (old %.1f)\n", ts, to, vs, vo);
    CHECK(ts < 0.55 * to && vs < 0.6 * vo, "calm ring not thin/soft: target %.1f vs old %.1f, view %.1f vs old %.1f", ts, to, vs, vo);
    fluid_sim_destroy(s); fluid_sim_destroy(z); fluid_sim_destroy(o);

    /* quiet: it breathes — the radius goes in and out, slowly and shallowly */
    fluid_ring_t r = { 0 };
    const float dt = 1.f / 30;
    float lo = 1.f, hi = -1.f; int pq = 0, sign_flips = 0; float prev = 0.f;
    for (int f = 0; f < 900; f++) {                          /* 30 s */
        pq += fluid_ring_tick(&r, dt, true, 0.f);
        if (f > 30) { lo = fminf(lo, r.dr); hi = fmaxf(hi, r.dr); }
        if (f > 30 && (r.dr > 0.f) != (prev > 0.f)) sign_flips++;
        prev = r.dr;
    }
    const float amp_idle = 0.5f * (hi - lo);
    CHECK(amp_idle > 0.8f * RING_BREATH_AMP_IDLE && amp_idle < 1.2f * RING_BREATH_AMP_IDLE, "quiet breath amplitude %.4f", amp_idle);
    CHECK(sign_flips >= 12 && sign_flips <= 16, "quiet: %d in/out turns in ~29 s (calm, slow)", sign_flips);
    CHECK(pq >= 1 && pq <= 2 + (int)(30 * RING_PING_IDLE), "quiet: %d pings in 30 s", pq);
    const float ink_q = r.ink;

    /* speaking (level updates at 10 Hz): further out, deeper, quicker, more and stronger pings */
    int ps = 0, flips_s = 0; float lo_s = 1.f, hi_s = -1.f, ink_max_s = 0.f, ink_max_q = ink_q * (1.f + RING_INK_BREATH);
    for (int f = 0; f < 900; f++) {                          /* 30 s of voice */
        const float lv = 0.55f + 0.25f * ((f / 3) % 2);     /* a voice: steps every 0.1 s */
        ps += fluid_ring_tick(&r, dt, true, lv);
        if (f > 10) { lo_s = fminf(lo_s, r.dr); hi_s = fmaxf(hi_s, r.dr); ink_max_s = fmaxf(ink_max_s, r.ink); }
        if (f > 10 && (r.dr > RING_SWELL * r.lvl) != (prev > RING_SWELL * r.lvl)) flips_s++;
        prev = r.dr;
    }
    CHECK(0.5f * (hi_s - lo_s) > 2.f * amp_idle, "voice: breath %.4f vs quiet %.4f", 0.5f * (hi_s - lo_s), amp_idle);
    CHECK(0.5f * (hi_s + lo_s) > 0.6f * RING_SWELL, "voice: the ring sits further out (%.4f)", 0.5f * (hi_s + lo_s));
    CHECK(flips_s >= 3 * sign_flips, "voice: %d in/out turns vs quiet %d (a quicker pulse)", flips_s, sign_flips);
    CHECK(ps >= 2 * pq + 2 && r.ping_s > 2.f * RING_PING_S_IDLE, "voice: %d pings (quiet %d), strength %.2f", ps, pq, r.ping_s);
    CHECK(ink_max_s > 1.2f * ink_max_q, "voice: brighter (%.2f vs %.2f)", ink_max_s, ink_max_q);

    /* nothing travels around it: the ring's ink is the same at every angle, frame after frame */
    {
        fluid_sim_t *a = fluid_sim_create(52, 32, FLUID_STAGE_REF);
        fluid_ring_t rr = { 0 };
        double worst = 0;
        for (int f = 0; f < 20; f++) {
            fluid_ring_tick(&rr, dt, true, f < 10 ? 0.f : 0.9f);
            memset(a->T, 0, (size_t)a->N * 2);
            fluid_target_ring(a, &rr, 0xffffff, 0.f);
            /* the 4-fold symmetric cells of the grid carry exactly the same target */
            for (int j = 0; j < a->H / 2; j++) for (int i = 0; i < a->W / 2; i++) {
                const int k = j * a->W + i, k2 = (a->H - 1 - j) * a->W + (a->W - 1 - i), k3 = j * a->W + (a->W - 1 - i);
                worst = fmax(worst, fabs((double)a->T[k] - a->T[k2]) + fabs((double)a->T[k] - a->T[k3]));
            }
        }
        CHECK(worst <= 4, "the ring is not symmetric (|ΔT| %.0f): something moves around it", worst);
        /* a ping is a symmetric bump of the surface along the whole rim, none inside */
        memset(a->h, 0, (size_t)a->N * 2);
        rr.ping_s = 0.3f; fluid_ring_ping(a, &rr);
        int bumped = 0; double asym = 0;
        for (int j = 0; j < a->H; j++) for (int i = 0; i < a->W; i++) {
            const int k = j * a->W + i, k2 = (a->H - 1 - j) * a->W + (a->W - 1 - i);
            bumped += a->h[k] > 0; asym = fmax(asym, abs(a->h[k] - a->h[k2]));
        }
        int kc = cell_of(a, 0.f, 0.f), kt = cell_of(a, 0.f, -(RING_R + rr.dr)), kr = cell_of(a, RING_R + rr.dr, 0.f);
        CHECK(bumped > 100 && asym < 2 && a->h[kc] == 0 && a->h[kt] > 0 && a->h[kr] > 0, "ping: %d cells, asym %.0f, centre %d, top %d, right %d", bumped, asym, a->h[kc], a->h[kt], a->h[kr]);
        fluid_sim_destroy(a);
    }

    /* silence: it settles back to the calm breath */
    for (int f = 0; f < 45; f++) fluid_ring_tick(&r, dt, true, 0.f);
    CHECK(r.lvl < 0.05f && fabsf(r.dr) < 1.3f * RING_BREATH_AMP_IDLE + RING_SWELL * r.lvl, "silence: level %.3f, dr %.4f", r.lvl, r.dr);

    /* smooth: a level jumping 0 ↔ 1 every 0.1 s never steps the breath */
    float maxd = 0.f, maxkink = 0.f, d1 = r.dr, d2 = r.dr;
    for (int f = 0; f < 60; f++) {
        const float l0 = r.lvl;
        fluid_ring_tick(&r, dt, true, (f / 3) % 2 ? 1.f : 0.f);
        maxd = fmaxf(maxd, fabsf(r.lvl - l0));
        if (f >= 2) maxkink = fmaxf(maxkink, fabsf(r.dr - 2.f * d1 + d2));   /* a step = a kink */
        d2 = d1; d1 = r.dr;
    }
    CHECK(maxd < 0.4f, "level stepped %.2f in one frame", maxd);
    CHECK(maxkink < 0.02f, "the radius kinked %.4f in one frame: the breath steps", maxkink);

    /* off: melts out within the fade, no pings, and a faded ring writes nothing */
    int poff = 0;
    for (int f = 0; f * dt < RING_FADE_S + dt; f++) poff += fluid_ring_tick(&r, dt, false, 1.f);
    CHECK(r.on == 0.f, "off: still %.2f after the fade", r.on);
    for (int f = 0; f < 30; f++) poff += fluid_ring_tick(&r, dt, false, 1.f);
    CHECK(poff <= 1 && r.lvl < 0.05f, "off: %d pings, level %.2f", poff, r.lvl);
    fluid_sim_t *e = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    fluid_target_blob(e, 0.f, 1.f, 0.f, 0.f);
    int16_t *T0 = malloc((size_t)e->N * 2); memcpy(T0, e->T, (size_t)e->N * 2);
    fluid_target_ring(e, &r, 0xffffff, 0.f);
    CHECK(!memcmp(T0, e->T, (size_t)e->N * 2), "a faded ring changed the target");
    free(T0); fluid_sim_destroy(e);

    /* a listen right after `say` (hold 0): the thin ring still shows within 0.3 s */
    fluid_sim_t *h = fluid_sim_create(52, 32, FLUID_STAGE_REF), *h0 = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    fluid_ring_t rh = { 0 };
    for (int pass = 0; pass < 2; pass++) {
        fluid_sim_t *x = pass ? h0 : h;
        x->p.relax_up = 2.6f; x->p.relax_down = 3.4f; x->p.hold = 0.f; x->p.tint_rate = 3.f;
        for (int f = 0; f < 9; f++) {
            fluid_target_blob(x, f / 30.f, 1.f, 0.f, 0.f);
            if (!pass) { fluid_ring_tick(&rh, dt, true, 0.f); fluid_target_ring(x, &rh, 0xff8c3c, 0.f); }
            fluid_sim_background(x, f / 30.f, 0.5f, true); fluid_sim_swirl_field(x);
            fluid_sim_step(x, 1.f / 60); fluid_sim_step(x, 1.f / 60); fluid_sim_color(x, 2.f / 60);
        }
        fluid_sim_view(x);
    }
    int shown = 0;
    for (int q = 0; q < 24; q++) {
        float a = 6.2831853f * q / 24, bv = 0.f;
        for (float rr = 0.80f; rr <= 0.95f; rr += 0.01f) {
            int k = cell_of(h, rr * sinf(a), -rr * cosf(a));
            bv = fmaxf(bv, (h->view[k] - h0->view[k]) / FQ_UNIT);
        }
        shown += bv > 0.06f;
    }
    CHECK(shown >= 20, "hold 0: the ring shows on only %d/24 rays within 0.3 s", shown);
    fluid_sim_destroy(h); fluid_sim_destroy(h0);
}

/* a question's arcs: each sector's border in its option's colour, gaps at the dividers */
static const uint32_t ARC_RGB[3] = { 0x3ccf96, 0xff6b6b, 0x4dabf7 };
static void arcs3(fluid_sim_t *s, float t) {
    (void)t;
    fluid_arc_t a[3];
    for (int i = 0; i < 3; i++) { fluid_ask_arc(3, i, FLUID_STAGE_REF, &a[i].a0, &a[i].a1); a[i].hw = FLUID_ASK_ARC_HW; a[i].amt = 1.f; a[i].rgb = ARC_RGB[i]; }
    fluid_target_arcs(s, FLUID_ASK_ARC_R, a, 3, 0.f);
}
static void test_fluid_arcs(void) {
    fluid_sim_t *s = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    run_arcs(s, 45, arcs3);
    int sector[FLUID_ASK_MAX]; fluid_ask_order(3, sector);
    for (int i = 0; i < 3; i++) {
        float a = ((float)sector[i] + 0.5f) * 6.2831853f / 3, R = FLUID_ASK_ARC_R;
        int k = cell_of(s, R * sinf(a), -R * cosf(a));
        float r, g, b; rgb_of(ARC_RGB[i], &r, &g, &b);
        const fluid_rgba16_t *c = &s->col[k];
        CHECK(s->view[k] / FQ_UNIT > 0.35f, "arc %d mid: ink %.2f", i, s->view[k] / FQ_UNIT);
        CHECK(fabsf(c->r / 65535.f - r) < 0.12f && fabsf(c->g / 65535.f - g) < 0.12f && fabsf(c->b / 65535.f - b) < 0.12f,
              "arc %d mid colour %.2f %.2f %.2f, want %.2f %.2f %.2f", i, c->r / 65535.f, c->g / 65535.f, c->b / 65535.f, r, g, b);
    }
    /* the targets alone (no flow): a divider is a gap, the arc's middle is full */
    fluid_sim_t *e = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    memset(e->T, 0, (size_t)e->N * 2);
    arcs3(e, 0.f);
    for (int q = 0; q < 3; q++) {
        float a = q * 6.2831853f / 3, R = FLUID_ASK_ARC_R;
        float mid = e->T[cell_of(e, R * sinf(a + 1.047f), -R * cosf(a + 1.047f))] / FQ_UNIT;
        float gap = e->T[cell_of(e, R * sinf(a), -R * cosf(a))] / FQ_UNIT;
        CHECK(mid > 0.5f && gap < 0.5f * mid, "divider %d: gap %.2f vs arc %.2f", q, gap, mid);
    }
    /* one pass = the arcs one by one (disjoint arcs: the same target and colour) */
    fluid_sim_t *o = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    fluid_sim_destroy(e); e = fluid_sim_create(52, 32, FLUID_STAGE_REF);   /* fresh T and tcol on both */
    memset(e->T, 0, (size_t)e->N * 2); memset(o->T, 0, (size_t)o->N * 2);
    arcs3(e, 0.f);
    for (int i = 0; i < 3; i++) { float a0, a1; fluid_ask_arc(3, i, FLUID_STAGE_REF, &a0, &a1); fluid_target_arc(o, FLUID_ASK_ARC_R, FLUID_ASK_ARC_HW, a0, a1, 1.f, ARC_RGB[i], 0.f); }
    CHECK(!memcmp(e->T, o->T, (size_t)e->N * 2) && !memcmp(e->tcol, o->tcol, (size_t)e->N * 4), "one pass differs from the arcs one by one");
    fluid_sim_destroy(o);
    fluid_sim_destroy(e); fluid_sim_destroy(s);
}

/* a coloured label: its letters draw in its colour on pastel, in the palette's on mono */
static void test_letter_color(void) {
    fluid_sim_t *s = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    fluid_text_t *tx = fluid_text_create(s);
    fluid_draw_t *d = fluid_draw_create(52, 32, FLUID_STAGE_REF, FLUID_STAGE_REF);
    fluid_frame_t *fr = fluid_frame_create(52, 32);
    const size_t PX = (size_t)FLUID_STAGE_REF * FLUID_STAGE_REF;
    uint16_t *fb = calloc(PX, 2);
    fluid_label_t lb[1] = { { "YES", 3, 206.f, 206.f, true, false, FLUID_RGB | 0xff0000 } };
    fluid_text_set_labels(tx, s, lb, 1, 40.f);
    CHECK(tx->letters[0].rgb == (FLUID_RGB | 0xff0000), "label colour reaches its letters");
    s->p.relax_up = 7.f; s->p.relax_down = 2.4f;
    for (int f = 0; f < 90; f++) { fluid_text_fill(tx, s, f / 30.f); fluid_sim_background(s, 1.f, 0.5f, true); fluid_sim_swirl_field(s); fluid_sim_step(s, 1.f / 60); fluid_sim_color(s, 1.f / 60); }
    fluid_sim_view(s); fluid_sim_snapshot(s, fr, false);
    for (int pal = 0; pal < FLUID_PAL_COUNT; pal++) {
        fluid_draw_opts_t o = { pal, FLUID_EDGE_ALL, 0.f, 1.f, 0.f, tx->letters, tx->nletters, " .:-=+*#", stub_font };
        memset(fb, 0, PX * 2); d->force = true; int y0, y1;
        fluid_draw(d, fr, fb, &o, &y0, &y1);
        long red = 0;
        for (int li = 0; li < tx->nletters; li++) {
            const fluid_letter_t *L = &tx->letters[li];
            for (int y = (int)L->y; y < (int)(L->y + L->h); y++) for (int x = (int)L->x; x < (int)(L->x + L->w); x++) {
                uint16_t be = fb[(size_t)y * FLUID_STAGE_REF + x], le = (uint16_t)(be >> 8 | be << 8);
                int r = (le >> 11) & 31, g = (le >> 5) & 63, b = le & 31;
                red += r > 20 && g < 8 && b < 4;
            }
        }
        if (pal == FLUID_PAL_PASTEL) CHECK(red > 60, "pastel: %ld red letter px", red);
        else CHECK(red == 0, "palette %d: %ld red px (mono stays mono)", pal, red);
    }
    free(fb); fluid_frame_destroy(fr); fluid_draw_destroy(d); fluid_text_destroy(tx); fluid_sim_destroy(s);
}

/* a letter's own melt hides it like the frame's melt does */
static void test_letter_melt(void) {
    fluid_sim_t *s = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    fluid_text_t *tx = fluid_text_create(s);
    fluid_draw_t *d = fluid_draw_create(52, 32, FLUID_STAGE_REF, FLUID_STAGE_REF);
    fluid_frame_t *fr = fluid_frame_create(52, 32);
    uint16_t *fb = calloc(FLUID_STAGE_REF * FLUID_STAGE_REF, 2);
    fluid_text_set_sentence(tx, 2.f);
    fluid_text_set_word(tx, s, "OK", 2, -1);
    s->p.relax_up = 7.f; s->p.relax_down = 2.4f;               /* mole_fluid's text rates */
    for (int f = 0; f < 90; f++) { fluid_text_fill(tx, s, f / 30.f); fluid_sim_background(s, 1.f, 0.5f, true); fluid_sim_swirl_field(s); fluid_sim_step(s, 1.f / 60); fluid_sim_color(s, 1.f / 60); }
    fluid_sim_view(s); fluid_sim_snapshot(s, fr, false);
    fluid_letter_t L[2]; memcpy(L, tx->letters, sizeof(L));
    fluid_draw_opts_t o = { FLUID_PAL_GREEN, FLUID_EDGE_ALL, 0.f, 1.f, 0.f, L, 2, " .:-=+*#", stub_font };
    const size_t PX = (size_t)FLUID_STAGE_REF * FLUID_STAGE_REF;
    uint16_t *img[3] = { calloc(PX, 2), calloc(PX, 2), calloc(PX, 2) };
    for (int v = 0; v < 3; v++) {          /* both shown · letter 1 melted · the frame melted */
        L[1].melt = v == 1 ? 0.8f : 0.f; o.melt = v == 2 ? 0.8f : 0.f;
        d->force = true; int y0, y1;
        fluid_draw(d, fr, img[v], &o, &y0, &y1);
    }
    /* compare each letter's box: letter 0 as when shown, letter 1 as when melted */
    long same0 = 0, same1 = 0, diff0 = 0, diff1 = 0;
    for (int li = 0; li < 2; li++)
        for (int y = (int)L[li].y; y < (int)(L[li].y + L[li].h); y++)
            for (int x = (int)L[li].x + 1; x < (int)(L[li].x + L[li].w) - 1; x++) {
                size_t p = (size_t)y * FLUID_STAGE_REF + x;
                if (li == 0) { same0 += img[1][p] == img[0][p]; diff0 += img[2][p] != img[0][p]; }
                else { same1 += img[1][p] == img[2][p]; diff1 += img[1][p] != img[0][p]; }
            }
    long area0 = (long)((int)(L[0].y + L[0].h) - (int)L[0].y) * ((int)(L[0].x + L[0].w) - 1 - ((int)L[0].x + 1));
    CHECK(same0 == area0 && diff0 > 20, "letter 0 must stay (%ld/%ld same, %ld differ when melted)", same0, area0, diff0);
    CHECK(diff1 > 20 && same1 > area0 * 8 / 10, "letter 1's own melt hid it (%ld differ, %ld like the melted frame)", diff1, same1);
    for (int v = 0; v < 3; v++) free(img[v]);
    free(fb); fluid_frame_destroy(fr); fluid_draw_destroy(d); fluid_text_destroy(tx); fluid_sim_destroy(s);
}

/* the device font draws every printable ASCII char (a question's labels may use any) */
static void test_font(void) {
    static const uint8_t full[7] = { 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f };
    int bad = 0;
    for (int c = 0x21; c <= 0x7e; c++) {
        const uint8_t *g = gc_font5x7((char)c);
        int ink = 0; for (int r = 0; r < 7; r++) ink |= g[r];
        if (!ink || !memcmp(g, full, 7)) { bad++; printf("  no glyph for '%c'\n", c); }
    }
    const uint8_t *sp = gc_font5x7(' ');
    CHECK(bad == 0 && !(sp[0] | sp[3] | sp[6]), "%d printable chars without a glyph", bad);
    CHECK(!memcmp(gc_font5x7('\t'), full, 7), "a control char is not the solid block");
}

int main(void) {
    printf("fluid host test\n");
    test_static_label();
    test_braille();
    test_dirty();
    test_labels();
    test_overlay();
    test_fluid_ring();
    test_ring_ripples();
    test_fluid_arcs();
    test_letter_color();
    test_letter_melt();
    test_font();
    static const int grids[3][2] = { { 44, 27 }, { 52, 32 }, { 60, 37 } };
    for (int g = 0; g < 3; g++) for (int st = 0; st < ST_N; st++) run_state(grids[g][0], grids[g][1], st, NULL, NULL);
    CHECK(fluid_sim_create(8, 8, FLUID_STAGE_REF) == NULL, "out-of-range grid accepted");
    { int gw, gh; fluid_grid_for_stage(FLUID_STAGE_REF, &gw, &gh); CHECK(gw == 52 && gh == 32, "412 default grid %dx%d", gw, gh); }
    test_panel(240, 240);
    test_panel(320, 240);
    test_panel(FLUID_STAGE_REF, FLUID_STAGE_REF);
    test_mark(FLUID_STAGE_REF, FLUID_STAGE_REF);
    test_mark(240, 240);
    test_mark(320, 240);
    printf("%d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
