/*
 * askview_test.c — a question over a moving scene, as device/creatures/ask-options.html shows it (main/fluid/
 * fluid_askview.c with the real sim and drawer): it forms where the centre tap's crest passes; standing, it overlays
 * everything (its words on cleared cells, its lines over the scene) while the scene behind, dimmed and thinned, keeps
 * moving; the whole name shows; answered, a wave leaves the chosen line and everything breaks into dust where it
 * stands (no word moves as a shape), carried off, gone by FLUID_ASK_GONE_S; declined, the rim lets go first. And what it
 * costs: the sim's part a frame, the panel's (a model: 0.25 ms a window, 0.25 us a pixel, as scene_test.c).
 *
 * Build + run: test/host/askview_build.sh (ASKVIEW_DUMP=<dir>: frames as PPM)
 */
#include "fluid_askview.h"
#include "glyph_font.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int s_fail = 0, s_pass = 0;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int P = 412, W = 52, H = 32;   /* the stage and its grid: the 1.46 (412) or the 1.75 (466) */
#define N (W * H)
#define NMAX (64 * 40)
#define DT (1.f / 30.f)
static const uint32_t CAT = 0xFF8C3C, WORLD = 0x8090A0;

static double now_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e6 + ts.tv_nsec * 1e-3; }
static uint32_t hash2(uint32_t a, uint32_t b) { uint32_t h = a * 374761393u + b * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return h ^ (h >> 16); }
static uint16_t rgb565_be(int r, int g, int b) { const uint16_t v = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); return (uint16_t)(v >> 8 | v << 8); }

typedef struct { long wins, px; } cost_t;
static int send_cost(void *ctx, int x, int y, int w, int h, const uint16_t *px) { cost_t *c = ctx; (void)x; (void)y; (void)px; c->wins++; c->px += (long)w * h; return 0; }

/* the world: a creature (a disc, every dot, its own) moving, and a static dust round it */
typedef struct {
    fluid_sim_t *s; fluid_draw_t *d; fluid_frame_t *fr; fluid_askview_t *v;
    fluid_ask_screen_t scr; fluid_ask_layout_t L;
    uint8_t bg[NMAX], own[NMAX], dots[NMAX]; uint32_t bgc[NMAX];
    fluid_letter_t lt[FLUID_MAX_LETTERS]; int nl;
    uint16_t *fb, *wbuf;
    float t, cat_x;
    bool moving;
    double sim_us, panel_ms; long px;
} world_t;

static void scene(world_t *w) {
    if (w->moving) w->cat_x = 0.35f * sinf(w->t * 2.f);
    for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
        const int k = j * W + i;
        const float x = w->s->xs[i], y = w->s->ys[j], dx = x - w->cat_x, dy = y - 0.15f;
        w->own[k] = dx * dx + dy * dy < 0.3f * 0.3f;
        w->bg[k] = w->own[k] ? 0xFF : (uint8_t)(hash2((uint32_t)k, 9u) & hash2((uint32_t)k, 10u) & 0xFF);
        w->bgc[k] = w->own[k] ? CAT : WORLD;
        if (!w->s->mask[k]) w->bg[k] = 0;
    }
}

static void frame(world_t *w, cost_t *c) {
    w->t += DT;
    for (int q = 0; q < 2; q++) fluid_sim_step(w->s, DT / 2.f);
    if (w->scr.phase != FLUID_ASK_NONE) { w->scr.t += DT; if (w->scr.phase != FLUID_ASK_SHOW) w->scr.out_t += DT; }
    if ((w->scr.phase == FLUID_ASK_PICK || w->scr.phase == FLUID_ASK_OUT) && w->scr.out_t >= FLUID_ASK_GONE_S) w->scr.phase = FLUID_ASK_NONE;
    const double t0 = now_us();
    fluid_askview_frame(w->v, w->s, &w->scr, w->t);
    w->sim_us += now_us() - t0;
    scene(w);
    fluid_sim_snapshot(w->s, w->fr, false);
    memcpy(w->dots, w->bg, N);
    for (int k = 0; k < N; k++) w->fr->col[k] = (fluid_rgba16_t){ (uint16_t)((w->bgc[k] >> 16 & 255) * 257), (uint16_t)((w->bgc[k] >> 8 & 255) * 257), (uint16_t)((w->bgc[k] & 255) * 257), 0 };
    fluid_askview_compose(w->v, w->dots, w->own, w->fr->col);
    w->nl = fluid_askview_letters(w->v, w->lt, FLUID_MAX_LETTERS);
    /* the options as mole_fluid.c has them for a rigged creature (its crisp dots; the frame's melt long past a text
     * state's: seconds), then the question's */
    fluid_draw_opts_t o = { .palette = FLUID_PAL_PASTEL, .edge_mode = FLUID_EDGE_CONTOURS, .time = w->t, .melt = 5.f + w->t,
                            .font = gc_font5x7, .dots = w->bg, .scene = true, .ramp = " .,:;!+=*xX#@" };
    fluid_askview_opts(w->v, &o, w->dots, w->lt, w->nl);
    cost_t cc = { 0, 0 };
    fluid_draw_cells(w->d, w->fr, &o);
    fluid_draw_send(w->d, &o, &(fluid_overlay_t){ 0 }, FLUID_MARK_NONE, false, w->wbuf, 4096, send_cost, &cc);
    fluid_draw_rect(w->d, &o, &(fluid_rect_t){ w->fb, P, 0, 0, P, P }, false);
    const double ms = cc.wins * 0.25 + cc.px * 0.25e-3;
    w->panel_ms = ms; w->px = cc.px;
    if (c) { c->wins += cc.wins; c->px += cc.px; }
}

static void dump(const world_t *w, const char *name) {
    const char *dir = getenv("ASKVIEW_DUMP");
    if (!dir) return;
    char path[512]; snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6 %d %d 255\n", P, P);
    for (int q = 0; q < P * P; q++) {
        const uint16_t v = (uint16_t)(w->fb[q] >> 8 | w->fb[q] << 8);
        const unsigned char px[3] = { (unsigned char)((v >> 11) << 3), (unsigned char)((v >> 5 & 63) << 2), (unsigned char)((v & 31) << 3) };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

static void world_open(world_t *w, const fluid_ask_q_t *q) {
    memset(w, 0, sizeof(*w));
    fluid_grid_for_stage(P, &W, &H);
    w->s = fluid_sim_create(W, H, P); w->d = fluid_draw_create(W, H, P, P); w->fr = fluid_frame_create(W, H);
    w->fb = calloc((size_t)P * P, 2); w->wbuf = calloc(4096, 2);
    w->moving = true;
    w->v = fluid_askview_create(w->s, q);
    w->L = *fluid_askview_layout(w->v);
    scene(w);
    for (int f = 0; f < 4; f++) frame(w, NULL);           /* the scene alone first */
    fluid_ask_follow(&w->scr, true, 1, -1, q->n);
    fluid_askview_start(w->v, w->s);
}
static void world_close(world_t *w) {
    fluid_askview_destroy(w->v); fluid_frame_destroy(w->fr); fluid_draw_destroy(w->d); fluid_sim_destroy(w->s); free(w->fb); free(w->wbuf);
}
static void run(world_t *w, float secs, cost_t *c, double *worst) { for (int f = 0; f < (int)(secs / DT + 0.5f); f++) { frame(w, c); if (worst && w->panel_ms > *worst) *worst = w->panel_ms; } }

static int lit(const uint8_t *m, int bits) { int n = 0; for (int b = 0; b < bits; b++) n += m[b >> 3] >> (7 - (b & 7)) & 1; return n; }
static int glyph_bits(const char *s) { int n = 0; for (; *s; s++) { const uint8_t *g = gc_font5x7(*s); for (int r = 0; r < 7; r++) n += __builtin_popcount(g[r] & 31); } return n; }
static float lum(const fluid_rgba16_t *c) { return (c->r + c->g + c->b) / (3.f * 65535.f); }
/* the cells of option opt's line (its arc's middle third) */
static int line_cells(const world_t *w, int opt, int *cells, int max) {
    int n = 0;
    for (int t = 0; t < w->L.ntargets; t++) {
        const fluid_ask_target_t *g = &w->L.tg[t];
        if (g->opt != opt || g->kind != FLUID_ASK_ARC) continue;
        for (int q = 0; q < 24 && n < max; q++) {
            const float a = g->b + (g->c - g->b) * (0.33f + 0.33f * q / 23.f), x = 0.962f * sinf(a), y = -0.962f * cosf(a);
            const int i = (int)((x + 1.f) * 0.5f * W), j = (int)((y + 1.f) * 0.5f * H), k = j * W + i;
            bool dup = false;
            for (int m = 0; m < n; m++) dup |= cells[m] == k;
            if (!dup) cells[n++] = k;
        }
    }
    return n;
}

static fluid_ask_q_t install_q(void) {
    fluid_ask_q_t q;
    memset(&q, 0, sizeof(q));
    strcpy(q.title, "install astrology?"); strcpy(q.detail, "a new package for molty");
    q.n = 2; strcpy(q.label[0], "install"); strcpy(q.label[1], "not now");
    q.rgb[0] = 0x3ccf96; q.rgb[1] = 0xff6b6b;
    return q;
}

/* forming, standing over the moving scene, the whole name; then answered */
static void test_accept(void) {
    world_t w;
    const fluid_ask_q_t q = install_q();
    world_open(&w, &q);
    cost_t form = { 0, 0 }; double worst = 0.;
    /* the crest forms it: at 0.3 s the lines are not all there yet, the title's middle (the crest's first) is */
    run(&w, 0.3f, &form, &worst);
    int cells[64], nc = line_cells(&w, 0, cells, 64), on = 0;
    for (int m = 0; m < nc; m++) on += fluid_askview_dots(w.v)[cells[m]] != 0;
    CHECK(on < nc, "at 0.3 s the rim has not formed yet (%d of %d cells)", on, nc);
    run(&w, FLUID_ASK_FORM_S + 0.6f - 0.3f, &form, &worst);
    dump(&w, "accept-standing");
    /* standing: each answer's line in its colour, bright, over the scene */
    for (int opt = 0; opt < 2; opt++) {
        nc = line_cells(&w, opt, cells, 64); on = 0;
        int right = 0;
        for (int m = 0; m < nc; m++) {
            const int k = cells[m];
            if (!fluid_askview_dots(w.v)[k]) continue;
            on++;
            const fluid_rgba16_t *c = &w.fr->col[k];
            const uint32_t want = q.rgb[opt];
            right += fabsf(c->r / 257.f - (want >> 16 & 255)) < 60.f && fabsf(c->g / 257.f - (want >> 8 & 255)) < 60.f && fabsf(c->b / 257.f - (want & 255)) < 60.f;
        }
        CHECK(on >= nc - 1 && right >= on * 3 / 4, "answer %d: its rim line stands in its colour (%d of %d cells, %d right)", opt, on, nc, right);
    }
    /* the scene behind: the creature dimmed to under a third, the world's dust thinned to about half */
    int own = 0, dim = 0, world0 = 0, world1 = 0;
    for (int k = 0; k < N; k++) {
        if (fluid_askview_dots(w.v)[k] || !w.s->mask[k]) continue;
        if (w.own[k]) { own++; dim += lum(&w.fr->col[k]) < 0.3f * (0xFF + 0x8C + 0x3C) / (3.f * 255.f); }
        else { world0 += __builtin_popcount(w.bg[k]); world1 += __builtin_popcount(w.dots[k]); }
    }
    CHECK(own > 20 && dim == own, "the creature dims under the question (%d of %d cells)", dim, own);
    CHECK(world1 > 0.3f * world0 && world1 < 0.6f * world0, "the world's dust thins to %.2f", (float)world1 / world0);
    /* the whole name, crisp, over cleared cells; nothing of the creature shows through its letters */
    CHECK(w.nl == 4, "title, detail, two answers: %d lines of letters", w.nl);
    const fluid_letter_t *tl = &w.lt[0];
    CHECK(tl->mask && tl->mw == 18 * 6 - 1 && lit(tl->mask, tl->mw * 7) == glyph_bits("INSTALL ASTROLOGY?"), "\"INSTALL ASTROLOGY?\" whole (%d of %d px)",
          tl->mask ? lit(tl->mask, tl->mw * 7) : 0, glyph_bits("INSTALL ASTROLOGY?"));
    CHECK(w.lt[2].mw == 7 * 6 - 1 && w.lt[3].mw == 7 * 6 - 1, "INSTALL and NOT NOW whole");
    {
        const uint16_t title = rgb565_be(244, 240, 250);
        long in = 0, through = 0;                           /* the title's box: its letters, else dark */
        const fluid_letter_t *L = &w.lt[0];
        for (int y = (int)L->y + 1; y < (int)(L->y + L->h) - 1; y++) for (int x = (int)L->x + 1; x < (int)(L->x + L->w) - 1; x++) {
            const uint16_t c = w.fb[y * P + x];
            in += c == title; through += c && c != title;
        }
        CHECK(through == 0, "nothing shows through the title's letters (%ld px)", through);
        CHECK(in > glyph_bits("INSTALL ASTROLOGY?") * 4, "the title drawn on the panel (%ld px)", in);
    }
    /* the scene keeps moving under it: cells change from frame to frame outside the question */
    uint8_t before[NMAX]; memcpy(before, w.dots, N);
    uint8_t standing[NMAX]; memcpy(standing, fluid_askview_dots(w.v), N);
    run(&w, 0.3f, NULL, NULL);
    int moved = 0;
    for (int k = 0; k < N; k++) moved += !fluid_askview_dots(w.v)[k] && before[k] != w.dots[k];
    CHECK(moved > 10, "the scene moves under the question (%d cells changed)", moved);
    /* answered with "install": a wave from its line; it lingers a beat, brighter; everything breaks into dust in place */
    const fluid_letter_t full = w.lt[0];
    float full_box[4][4];
    for (int n = 0; n < 4; n++) { full_box[n][0] = w.lt[n].x; full_box[n][1] = w.lt[n].x + w.lt[n].w; full_box[n][2] = w.lt[n].y; full_box[n][3] = w.lt[n].y + w.lt[n].h; }
    uint8_t fullmask[256]; memcpy(fullmask, full.mask, (size_t)(full.mw * 7 + 7) / 8);
    const int16_t h0 = w.s->h[(int)((w.L.tg[0].cy + 1.f) * 0.5f * H) * W + (int)((w.L.tg[0].cx + 1.f) * 0.5f * W)];
    fluid_ask_follow(&w.scr, true, 1, 0, 2);
    fluid_askview_answer(w.v, w.s, 0);
    const int16_t h1 = w.s->h[(int)((w.L.tg[0].cy + 1.f) * 0.5f * H) * W + (int)((w.L.tg[0].cx + 1.f) * 0.5f * W)];
    CHECK(h1 > h0 + 0.5f * FQ_H, "a real wave starts from the chosen line (%.2f -> %.2f)", h0 / FQ_H, h1 / FQ_H);
    run(&w, 0.1f, NULL, NULL);
    int lv[2] = { 0, 0 };
    for (int opt = 0; opt < 2; opt++) {
        nc = line_cells(&w, opt, cells, 64);
        for (int m = 0; m < nc; m++) lv[opt] += (int)(lum(&w.fr->col[cells[m]]) * 100.f);
    }
    CHECK(lv[0] > lv[1], "the chosen line lingers brighter (%d vs %d)", lv[0], lv[1]);
    run(&w, 0.25f, NULL, NULL);
    dump(&w, "accept-dust");
    bool subset = true; int left = 0, away = 0;
    for (int n = 0; n < w.nl; n++) if (w.lt[n].x == full.x && w.lt[n].y == full.y) {
        for (int b = 0; b < (full.mw * 7 + 7) / 8; b++) subset &= (w.lt[n].mask[b] & ~fullmask[b]) == 0;
        left = lit(w.lt[n].mask, full.mw * 7);
    }
    for (int k = 0; k < N; k++) {                           /* dust where nothing of the question stood */
        const int i = k % W, j = k / W;
        bool word = false;
        for (int n = 0; n < 4; n++) word |= i * (float)P / W >= full_box[n][0] - 8.f && i * (float)P / W <= full_box[n][1] && j * (float)P / H >= full_box[n][2] - 13.f && j * (float)P / H <= full_box[n][3];
        away += fluid_askview_dots(w.v)[k] && !standing[k] && !word;
    }
    CHECK(subset && left < glyph_bits("INSTALL ASTROLOGY?") * 2 / 3, "the title breaks where it stands, never moved (%d px left)", left);
    {   /* the chosen answer lingers whole on the panel, though its own wave runs under it */
        long on = 0;
        for (int y = (int)full_box[2][2]; y < (int)full_box[2][3]; y++) for (int x = (int)full_box[2][0]; x < (int)full_box[2][1]; x++) {
            const uint16_t v = (uint16_t)(w.fb[y * P + x] >> 8 | w.fb[y * P + x] << 8);
            on += v && abs(((v >> 11) << 3) - 0x3c) < 40 && abs(((v >> 5 & 63) << 2) - 0xcf) < 40 && abs(((v & 31) << 3) - 0x96) < 40;
        }
        CHECK(on > glyph_bits("INSTALL") * 3, "INSTALL still drawn while its wave starts (%ld px)", on);
    }
    CHECK(away > 3, "its dust is carried off by the water (%d cells)", away);
    run(&w, FLUID_ASK_GONE_S, NULL, NULL);
    int rest = 0; for (int k = 0; k < N; k++) rest += fluid_askview_dots(w.v)[k] != 0;
    CHECK(w.scr.phase == FLUID_ASK_NONE && rest == 0 && w.nl == 0, "gone by %.1f s (%d cells, %d lines)", FLUID_ASK_GONE_S, rest, w.nl);
    printf("  forming (panel model): %.1f ms a frame on average, worst %.1f\n", (form.wins * 0.25 + form.px * 0.25e-3) / ((FLUID_ASK_FORM_S + 0.6f) / DT), worst);
    world_close(&w);
    /* against a finger's touch at the centre of the same scene, no question */
    world_open(&w, &q);
    w.scr.phase = FLUID_ASK_NONE;
    cost_t touch = { 0, 0 }; double tworst = 0.;
    run(&w, FLUID_ASK_FORM_S + 0.6f, &touch, &tworst);
    printf("  a touch's ripple alone (panel model): %.1f ms a frame on average, worst %.1f\n", (touch.wins * 0.25 + touch.px * 0.25e-3) / ((FLUID_ASK_FORM_S + 0.6f) / DT), tworst);
    CHECK(worst < tworst + 8.0, "forming costs about what a touch does (worst %.1f against %.1f ms)", worst, tworst);
    world_close(&w);
}

/* declined (or timed out): the rim lets go as the recording ring's stop, then the same dust */
static void test_decline(void) {
    world_t w;
    const fluid_ask_q_t q = install_q();
    world_open(&w, &q);
    run(&w, 2.f, NULL, NULL);
    float rim0 = 0.f, rim1 = 0.f, mid0 = 0.f, mid1 = 0.f;
    for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
        const float r = sqrtf(w.s->xs[i] * w.s->xs[i] + w.s->ys[j] * w.s->ys[j]);
        if (fabsf(r - 0.86f) < 0.04f) rim0 += w.s->h[j * W + i];
        if (r < 0.4f) mid0 += w.s->h[j * W + i];
    }
    fluid_ask_follow(&w.scr, false, 1, -1, 2);
    fluid_askview_answer(w.v, w.s, -1);
    for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
        const float r = sqrtf(w.s->xs[i] * w.s->xs[i] + w.s->ys[j] * w.s->ys[j]);
        if (fabsf(r - 0.86f) < 0.04f) rim1 += w.s->h[j * W + i];
        if (r < 0.4f) mid1 += w.s->h[j * W + i];
    }
    CHECK(w.scr.phase == FLUID_ASK_OUT && rim1 - rim0 > 20.f * FQ_H * 0.1f && fabsf(mid1 - mid0) < 1.f, "the rim lets go a ring of water (%.1f at the rim, %.1f inside)", (rim1 - rim0) / FQ_H, (mid1 - mid0) / FQ_H);
    run(&w, 0.15f, NULL, NULL);
    int dots = 0; for (int k = 0; k < N; k++) dots += fluid_askview_dots(w.v)[k] != 0;
    CHECK(dots > 0 && w.nl > 0, "breaking up, not gone at once (%d cells, %d lines)", dots, w.nl);
    run(&w, FLUID_ASK_GONE_S, NULL, NULL);
    dots = 0; for (int k = 0; k < N; k++) dots += fluid_askview_dots(w.v)[k] != 0;
    CHECK(w.scr.phase == FLUID_ASK_NONE && dots == 0 && w.nl == 0, "then gone");
    world_close(&w);
}

/* standing over a still scene: little goes to the panel (the words are not sent again); over the moving one, the
 * scene's own changes; the sim's part a frame */
static void test_cost(void) {
    world_t w;
    const fluid_ask_q_t q = install_q();
    world_open(&w, &q);
    run(&w, 3.6f, NULL, NULL);                               /* formed and settled */
    w.moving = false;
    run(&w, 0.2f, NULL, NULL);
    cost_t still = { 0, 0 }; double worst = 0.;
    w.sim_us = 0.;
    run(&w, 2.f, &still, &worst);
    const double mean = (still.wins * 0.25 + still.px * 0.25e-3) / (2.f / DT);
    printf("  standing over a still scene (panel model): %.2f ms a frame, worst %.2f; the question's own frame %.0f us on the host\n", mean, worst, w.sim_us / (2.f / DT));
    CHECK(mean < 3.0 && worst < 8.0, "a still question costs the panel little (%.2f ms, worst %.2f): its words are not sent again", mean, worst);
    w.moving = true;
    cost_t mv = { 0, 0 }; worst = 0.;
    run(&w, 2.f, &mv, &worst);
    printf("  standing over the moving scene (panel model): %.1f ms a frame, worst %.1f\n", (mv.wins * 0.25 + mv.px * 0.25e-3) / (2.f / DT), worst);
    CHECK(worst < 30.0, "under a frame (worst %.1f ms)", worst);
    world_close(&w);
}

/* 4..5 answers: underlined rows, a faint ring round the rim; each row's line under its label */
static void test_rows(void) {
    world_t w;
    fluid_ask_q_t q;
    memset(&q, 0, sizeof(q));
    strcpy(q.title, "which playlist?");
    q.n = 5;
    static const char *const L[5] = { "focus", "jazz", "lo-fi", "classica", "silence" };
    for (int i = 0; i < 5; i++) { strcpy(q.label[i], L[i]); q.rgb[i] = fluid_ask_color(-1, i); }
    world_open(&w, &q);
    run(&w, 2.4f, NULL, NULL);
    dump(&w, "rows-standing");
    CHECK(w.L.hit.rows && w.L.tg[0].kind == FLUID_ASK_RING, "rows and a ring");
    for (int t = 1; t < w.L.ntargets; t++) {
        const fluid_ask_target_t *g = &w.L.tg[t];
        const int j = (int)((g->a + 1.f) * 0.5f * H), i = W / 2;
        int on = 0;
        for (int dj = -1; dj <= 1; dj++) on += fluid_askview_dots(w.v)[(j + dj) * W + i] != 0;
        CHECK(on > 0, "row %d underlined", t - 1);
    }
    int ring = 0;
    for (int q2 = 0; q2 < 16; q2++) {
        const float a = q2 * 6.2831853f / 16, x = 0.975f * sinf(a), y = -0.975f * cosf(a);
        const int i = (int)((x + 1.f) * 0.5f * W), j = (int)((y + 1.f) * 0.5f * H);
        int any = 0;
        for (int dj = -1; dj <= 1; dj++) for (int di = -1; di <= 1; di++) { const int ii = i + di, jj = j + dj; if (ii >= 0 && jj >= 0 && ii < W && jj < H) any |= fluid_askview_dots(w.v)[jj * W + ii]; }
        ring += any != 0;
    }
    CHECK(ring >= 14, "the faint ring round the rim (%d of 16)", ring);
    CHECK(w.nl == 6, "title and five labels (%d)", w.nl);
    for (int n = 1; n < w.nl; n++) {                          /* each underline shows on the panel, under its word's cleared cells */
        const fluid_letter_t *Lt = &w.lt[n];
        const int c = (int)(Lt->rgb & 0xFFFFFF);
        long on = 0;
        for (int y = (int)(Lt->y + Lt->h); y < (int)(Lt->y + Lt->h) + 26 && y < P; y++)
            for (int x = (int)Lt->x; x < (int)(Lt->x + Lt->w); x++) {
                const uint16_t v = (uint16_t)(w.fb[y * P + x] >> 8 | w.fb[y * P + x] << 8);
                const int r = (v >> 11) << 3, g = (v >> 5 & 63) << 2, b = (v & 31) << 3;
                on += v && abs(r - (c >> 16 & 255)) < 70 && abs(g - (c >> 8 & 255)) < 70 && abs(b - (c & 255)) < 70;
            }
        CHECK(on > 20, "row %d: its underline drawn (%ld px)", n - 1, on);
    }
    world_close(&w);
}

int main(void) {
    printf("askview host test\n");
    test_accept();
    P = 466; printf("  the 1.75 (466 px):\n"); test_accept(); P = 412;
    test_decline();
    test_cost();
    test_rows();
    printf("%d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
