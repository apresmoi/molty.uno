/*
 * SPDX-License-Identifier: MIT
 *
 * creature_ring.c: the rings over a rigged creature's crisp cells (creature_ring.h), as ring-options.html's disc 1 (one
 * touch; a sharp line). Start: a touch at the centre of the real water (a finger's), the water quickened while the ring
 * forms so its crest reaches the rim in ~0.6 s, not a touch's ~1.5 s; it wobbles the creature as it passes, near the rim
 * its crest carries the ring's colour, and where it arrives the ring lands row by row: a thin bright line at the edge.
 * While a listen's ring shows, the voice (cr_voice) sends small ripples from the rim inward through the same water, so
 * the line trembles with it. Stop: a ring-shaped wave let go at the line carries the colour inward, fading, the ring
 * leaving row by row. Each part of the ring follows its line's ink (a listen: all of it; a question: its arcs; the
 * progress comet: where it is).
 *
 * Made once per grid: the dots from CR_CREST_IN out (the line and where the crest may carry colour) and the cells inside
 * (the release's crest), each with its cell, bit, angle bin, radius and three hashes.
 */
#include "creature_ring.h"
#include "fluid_ring.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define CR_CALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CR_FREE(p)   heap_caps_free(p)
#else
#define CR_CALLOC(n) calloc(1, (n))
#define CR_FREE(p)   free(p)
#endif

#define CR_NB       128      /* angle bins                                                                     */
#define CR_ARC_R    0.92f    /* a question's arcs' line (fluid_ask.h FLUID_ASK_ARC_R): where its ink is read   */
#define CR_LINE_IN  0.962f   /* the line: from here to the rim                                                 */
#define CR_RIM      0.988f
#define CR_CREST_IN 0.55f    /* the forming crest carries colour from here out                                */
#define CR_PROBE_R  0.88f    /* each bin reads the wave here (inside the rim's sponge)                         */
#define CR_TAP_S    1.6f     /* the start: a touch at the centre (a finger's: 1.6, 0.09) ...                    */
#define CR_TAP_R    0.09f
#define CR_C2       0.28f    /* ... its water quicker while the ring forms or lets go (fluid_params_t c2: 0.11; a
                              * touch's ripple reaches the rim in ~1.5 s, this one in ~0.8 s; stable below ~0.4)  */
#define CR_FORM_S   0.9f     /* ... and lands there by now, the water's crest or not                           */
#define CR_LET_S    0.12f    /* the release: a ring-shaped wave let go at the line (as ring-options.html)        */
#define CR_LET_W    0.05f
#define CR_LAND_H   0.012f   /* the crest has arrived where the water is up this much (h units)                */
#define CR_LAND_S   0.35f    /* a part lands row by row over this once its crest arrives (rows scattered: a few a
                              * frame for the panel, which takes ~0.6 ms a changed row whatever is in it)       */
#define CR_CREST_R  0.8f      /* the forming crest carries the colour from here out                             */
#define CR_CALM_S   1.0f     /* landed or let go, the ring's wave settles over this (the water's height eased to 0:
                              * a moving surface makes every cell under it change, the panel's cost)            */
#define CR_DONE_S   1.0f     /* released: gone after this                                                      */
#define CR_TW       36       /* each dot row's twinkle every this many frames (rows scattered, a few a frame)   */
/* the voice on a listen's ring: small touches of the water just inside the line, at random angles, more and stronger
 * the louder (cr_voice's level, 0..1, as an envelope: up in ~50 ms, down in ~200 ms); none below CR_V_MIN */
#define CR_V_R      0.9f     /* where they touch (inside the rim's sponge, so they run inward)                  */
#define CR_V_W      0.045f   /* their size (a touch's 0.09)                                                    */
#define CR_V_S      0.35f    /* their strength at full voice (a touch's 1.6)                                   */
#define CR_V_RATE   18.f     /* touches a second at full voice                                                 */
#define CR_V_MIN    0.08f
#define CR_V_UP     0.05f
#define CR_V_DOWN   0.2f
#define CR_ILACE_IN 3        /* forming or letting go, the water's rows (and the crest's) each every this many frames
                              * (else every other, fluid_draw_opts_t.frame): the wave changes nearly every cell then */

typedef struct { uint16_t k; uint8_t bit, bin; uint16_t r; uint8_t h1, h2, h3, row, cw; } cr_dot_t;   /* r: radius x 65535; cw: the
                                                                                                    * forming crest's weight there x 255 */
enum { OFF, IN, ON, OUT };

struct cr {
    int W, H, n, nin;                             /* dots: the band's (r >= CR_CREST_IN) first, then the inner ones */
    cr_dot_t *dot;
    int16_t kr[CR_NB], ka[CR_NB], kp[CR_NB];      /* each bin's cell on the ring's line, the arcs' line, the probe  */
    float *cr2;                                   /* each cell's radius (the driven wave)                          */
    float land[CR_NB], lr[CR_NB];                 /* each bin: when its crest arrived (-1: not yet), its line's ink */
    uint32_t col[CR_NB];                          /* ... and its colour                                             */
    int32_t listen;                               /* the listen ring's colour over its line's (cr_listen), -1       */
    int16_t lq;                                   /* ... and its ink (as the target's, Q14), given not written      */
    bool lisring;                                 /* the ring showing (or letting go) is the listen's               */
    int mode; float T, red, sum, c2, calm;          /* calm: s the water is settled after the ring's wave          */                /* c2: the water's own wave speed, while the ring's is quicker   */
    uint8_t *cbits, *cb;                          /* per cell: the crest's dots and brightness, kept a frame (each
                                                   * half of the cell rows takes the crest every other frame)       */
    uint8_t ilace, im;                            /* this frame's phase and how many (2, or CR_ILACE_IN forming)     */
    float venv, vin, vacc; uint32_t vn;           /* the voice: its envelope, the level given, touches due, count   */
    /* the ring's own dots are kept per cell row and made again only when that row changes (its twinkle's turn, its
     * landing or leaving, the line's ink): a steady ring costs a row or so a frame */
    int *brow, *irow;                             /* per cell row: its band dots [brow[j], brow[j + 1]), inner ones   */
    uint8_t *rl, *rll, *rlb;                      /* per cell: the ring's dots, brightness (x 32), bin              */
    uint32_t *rowph, *rowver; uint32_t ver;       /* per row: the twinkle turn and the version it was made at       */
    float lmin, lmax;                             /* when the first and the last part landed                        */
    bool all;                                     /* ... every part has: the ring lands row by row from lmax        */
    uint16_t *cells; int ncell, nbcell;           /* the cells with ring dots in them: the band's [0, nbcell), all  */
    uint32_t lutc; uint16_t lut[64][3];           /* the cells' colours by brightness (eighths), for colour lutc    */
    uint8_t hin[128], hout[128];                  /* the crest's lift by the water's height (h, Q10, from 0) x 255  */
    uint16_t *cdot; int *crow;                    /* the band's dots the forming crest can colour, by cell row     */
    bool crest;                                   /* crest dots are held (cleared once the crest is done)            */
};

static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline float smooth(float a, float b, float x) { const float t = clampf((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t); }
static uint32_t hash2(uint32_t a, uint32_t b) { uint32_t h = a * 374761393u + b * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return h ^ (h >> 16); }
static int nearest(const float *c, int n, float v) { int b = 0; for (int i = 1; i < n; i++) if (fabsf(c[i] - v) < fabsf(c[b] - v)) b = i; return b; }

cr_t *cr_create(const fluid_sim_t *s) {
    cr_t *r = CR_CALLOC(sizeof(cr_t));
    if (!r) return NULL;
    const int W = s->W, H = s->H;
    r->W = W; r->H = H; r->listen = -1;
    for (int b = 0; b < CR_NB; b++) {
        const float a = (b + 0.5f) * (6.2831853f / CR_NB), sx = sinf(a), sy = -cosf(a);   /* clockwise from 12 */
        r->kr[b] = (int16_t)(nearest(s->ys, H, RING_R * sy) * W + nearest(s->xs, W, RING_R * sx));
        r->ka[b] = (int16_t)(nearest(s->ys, H, CR_ARC_R * sy) * W + nearest(s->xs, W, CR_ARC_R * sx));
        r->kp[b] = (int16_t)(nearest(s->ys, H, CR_PROBE_R * sy) * W + nearest(s->xs, W, CR_PROBE_R * sx));
    }
    r->cr2 = CR_CALLOC(sizeof(float) * (size_t)W * H);
    r->cbits = CR_CALLOC((size_t)W * H); r->cb = CR_CALLOC((size_t)W * H);
    r->rl = CR_CALLOC((size_t)W * H); r->rll = CR_CALLOC((size_t)W * H); r->rlb = CR_CALLOC((size_t)W * H);
    r->rowph = CR_CALLOC(sizeof(uint32_t) * (size_t)H); r->rowver = CR_CALLOC(sizeof(uint32_t) * (size_t)H);
    r->brow = CR_CALLOC(sizeof(int) * (size_t)(H + 1)); r->irow = CR_CALLOC(sizeof(int) * (size_t)(H + 1));
    if (!r->cr2 || !r->cbits || !r->cb || !r->rl || !r->rll || !r->rlb || !r->rowph || !r->rowver || !r->brow || !r->irow) { cr_destroy(r); return NULL; }
    for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) r->cr2[j * W + i] = sqrtf(s->xs[i] * s->xs[i] + s->ys[j] * s->ys[j]);
    for (int pass = 0; pass < 3; pass++) {        /* count; the band's dots; the inner ones */
        int n = pass == 2 ? r->n : 0;
        for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
            const int k = j * W + i;
            if (!s->mask[k]) continue;
            for (int q = 0; q < 8; q++) {
                const int c = q >> 2, row = q & 3;
                const float x = s->xs[i] + (c ? 0.5f : -0.5f) / W, y = s->ys[j] + ((row + 0.5f) / 4.f - 0.5f) * 2.f / H;
                const float rad = sqrtf(x * x + y * y);
                if (rad > CR_RIM || rad < 0.08f) continue;
                const bool band = rad >= CR_CREST_IN;
                if (pass == 0) { if (band) r->n++; else r->nin++; continue; }
                if (band != (pass == 1)) continue;
                float a = atan2f(x, -y); if (a < 0.f) a += 6.2831853f;
                cr_dot_t *d = &r->dot[n++];
                const int sx = 2 * i + c, sy = 4 * j + row;
                d->k = (uint16_t)k; d->bit = (uint8_t)(1 << fluid_braille_bit(c, row));
                d->bin = (uint8_t)((int)(a * (CR_NB / 6.2831853f)) % CR_NB); d->r = (uint16_t)(rad * 65535.f);
                d->h1 = (uint8_t)(hash2((uint32_t)sx, (uint32_t)sy) >> 24); d->h2 = (uint8_t)(hash2((uint32_t)sx + 911u, (uint32_t)sy) >> 24);
                d->h3 = (uint8_t)(hash2((uint32_t)sx, (uint32_t)sy + 1201u) >> 24); d->row = (uint8_t)j;
            }
        }
        if (pass == 0 && !(r->dot = CR_CALLOC(sizeof(cr_dot_t) * (size_t)(r->n + r->nin + 1)))) { cr_destroy(r); return NULL; }
    }
    {   /* the ring at the disc's visible edge, kept round: the panel shows whole cells inside the disc, so its edge is a
         * stepped octagon; the largest circle every angle still shows (the least of each angle's outermost drawn dot)
         * is the rim, CR_RIM, and every dot's radius is taken from there */
        float edge[CR_NB], vis = 1.f;
        for (int q = 0; q < CR_NB; q++) edge[q] = 0.f;
        for (int n = 0; n < r->n; n++) { const float rad = r->dot[n].r * (1.f / 65535.f); if (rad > edge[r->dot[n].bin]) edge[r->dot[n].bin] = rad; }
        for (int q = 0; q < CR_NB; q++) if (edge[q] > 0.5f && edge[q] < vis) vis = edge[q];
        for (int n = 0; n < r->n; n++) {
            cr_dot_t *d = &r->dot[n];
            const float rad = d->r * (1.f / 65535.f) + (CR_RIM - vis);
            d->r = (uint16_t)(rad < 0.f ? 0.f : rad > 1.f ? 65535.f : rad * 65535.f);
        }
    }
    for (int j = 0, n = 0; j <= H; j++) { while (n < r->n && r->dot[n].row < j) n++; r->brow[j] = n; }   /* dots are in row order */
    for (int j = 0, n = r->n; j <= H; j++) { while (n < r->n + r->nin && r->dot[n].row < j) n++; r->irow[j] = n; }
    {   /* the cells the ring's dots are in: the band's first, then the inner ones (the release's crest) */
        uint8_t *seen = CR_CALLOC((size_t)W * H);
        if (!seen || !(r->cells = CR_CALLOC(sizeof(uint16_t) * (size_t)W * H))) { CR_FREE(seen); cr_destroy(r); return NULL; }
        for (int pass = 0; pass < 2; pass++) {
            for (int n = pass ? r->n : 0; n < (pass ? r->n + r->nin : r->n); n++)
                if (!seen[r->dot[n].k]) { seen[r->dot[n].k] = 1; r->cells[r->ncell++] = r->dot[n].k; }
            if (!pass) r->nbcell = r->ncell;
        }
        CR_FREE(seen);
    }
    for (int n = 0; n < r->n + r->nin; n++) r->dot[n].cw = (uint8_t)(255.f * smooth(CR_CREST_R, CR_PROBE_R, r->dot[n].r * (1.f / 65535.f)) + 0.5f);
    for (int q = 0; q < 128; q++) {
        r->hin[q] = (uint8_t)(255.f * smooth(0.015f, 0.07f, q * (1.f / FQ_H)) + 0.5f);
        r->hout[q] = (uint8_t)(255.f * smooth(0.02f, 0.08f, q * (1.f / FQ_H)) + 0.5f);
    }
    {   /* the forming crest only colours the band's dots from CR_CREST_R out: those, row by row */
        int nc = 0;
        for (int n = 0; n < r->n; n++) nc += r->dot[n].cw > 0;
        if (!(r->cdot = CR_CALLOC(sizeof(uint16_t) * (size_t)(nc + 1))) || !(r->crow = CR_CALLOC(sizeof(int) * (size_t)(H + 1)))) { cr_destroy(r); return NULL; }
        nc = 0;
        for (int j = 0; j < H; j++) {
            r->crow[j] = nc;
            for (int n = r->brow[j]; n < r->brow[j + 1]; n++) if (r->dot[n].cw > 0) r->cdot[nc++] = (uint16_t)n;
        }
        r->crow[H] = nc;
    }
    r->ver = 1; r->lutc = 0xFFFFFFFFu;
    return r;
}

void cr_destroy(cr_t *r) {
    if (!r) return;
    CR_FREE(r->dot); CR_FREE(r->cr2); CR_FREE(r->cbits); CR_FREE(r->cb);
    CR_FREE(r->rl); CR_FREE(r->rll); CR_FREE(r->rlb); CR_FREE(r->rowph); CR_FREE(r->rowver); CR_FREE(r->brow); CR_FREE(r->irow); CR_FREE(r->cells); CR_FREE(r->cdot); CR_FREE(r->crow); CR_FREE(r);
}

/* a ring-shaped push of the water at radius R (the release): the same bump as a touch, laid round a circle */
static void kick(const cr_t *r, fluid_sim_t *s, float R, float a) {
    for (int k = 0; k < s->N; k++) {
        if (!s->mask[k]) continue;
        const float e = (r->cr2[k] - R) * (1.f / CR_LET_W);
        if (e * e < 9.f) { const float v = (float)s->h[k] + a * expf(-2.f * e * e) * FQ_H; s->h[k] = (int16_t)(v > 32767.f ? 32767.f : v < -32768.f ? -32768.f : v); }
    }
}

/* the voice's touches on the water just inside the line (a listen's ring, shown) */
static void voice(cr_t *r, fluid_sim_t *s, float dt) {
    const float k = r->vin > r->venv ? dt / CR_V_UP : dt / CR_V_DOWN;
    r->venv += (r->vin - r->venv) * (k > 1.f ? 1.f : k);
    if (r->venv < CR_V_MIN) { r->vacc = 0.f; return; }
    for (r->vacc += CR_V_RATE * r->venv * dt; r->vacc >= 1.f; r->vacc -= 1.f) {
        const float a = (float)(hash2(r->vn++, 0x5eedu) >> 8) * (6.2831853f / 16777216.f);
        fluid_sim_impulse(s, CR_V_R * sinf(a), -CR_V_R * cosf(a), CR_V_S * r->venv, CR_V_W);
    }
}

bool cr_paint(cr_t *r, fluid_sim_t *s, float dt, uint32_t frame, const uint8_t *creature, uint8_t *dots, fluid_rgba16_t *col) {
    /* each bin's ink at its line (the stronger line wins): rising or holding, a ring is wanted; falling (its own fade
     * out), it is let go */
    const float norm = 3.f / (FQ_UNIT * RING_INK);
    float sum = 0.f;
    for (int b = 0; b < CR_NB; b++) {
        const int tr0 = s->T[r->kr[b]], tr = tr0 > r->lq ? tr0 : r->lq, ta = s->T[r->ka[b]], t = tr > ta ? tr : ta;
        sum += (float)t;
        if (t > 0 && (tr0 > 0 || ta > 0)) r->col[b] = s->tcol[tr0 >= ta ? r->kr[b] : r->ka[b]];
    }
    const bool want = sum > 0.f && sum >= 0.97f * r->sum;
    r->sum = sum;
    if (want) for (int b = 0; b < CR_NB; b++) {
        const int tr = s->T[r->kr[b]] > r->lq ? s->T[r->kr[b]] : r->lq, t = tr > s->T[r->ka[b]] ? tr : s->T[r->ka[b]];
        const float l = clampf(t * norm, 0.f, 1.f);
        if (fabsf(l - r->lr[b]) > 1.f / 64.f || (l == 0.f) != (r->lr[b] == 0.f)) { r->lr[b] = l; r->ver++; }   /* the ring's rows again */
    }
    if (want && (r->mode == OFF || r->mode == OUT)) {          /* a touch at the centre */
        if (r->mode == OFF) r->c2 = s->p.c2;
        r->lisring = r->lq > 0;
        r->mode = IN; r->T = 0.f; r->ver++; r->lmin = 1e9f; r->lmax = -1.f; r->all = false; for (int b = 0; b < CR_NB; b++) r->land[b] = -1.f;
        s->p.c2 = CR_C2;
        fluid_sim_impulse(s, 0.f, 0.f, CR_TAP_S, CR_TAP_R);
    } else if (!want && (r->mode == IN || r->mode == ON)) {    /* the ring lets go into the water */
        if (r->mode == ON) r->c2 = s->p.c2;
        r->mode = OUT; r->T = 0.f; r->red = 1.f; r->ver++;
        s->p.c2 = CR_C2;
        kick(r, s, RING_R, CR_LET_S);
    }
    if (r->calm > 0.f) {                          /* the ring's wave done: the water settles */
        r->calm -= dt;
        for (int k = 0; k < s->N; k++) { s->h[k] = (int16_t)(s->h[k] * 7 / 8); s->hp[k] = (int16_t)(s->hp[k] * 7 / 8); }
    }
    if (r->mode == OFF) { r->venv = 0.f; r->im = 2; return false; }
    r->T += dt;
    if (r->mode == ON && r->lisring) voice(r, s, dt);
    else r->venv = 0.f;

    if (r->mode == IN) {                          /* a part lands once the crest reaches it (or the driven front has) */
        bool all = true;
        for (int b = 0; b < CR_NB; b++) {
            if (r->land[b] < 0.f && (s->h[r->kp[b]] > (int16_t)(CR_LAND_H * FQ_H) || r->T >= CR_FORM_S)) {
                r->land[b] = r->T;
                if (r->T < r->lmin) r->lmin = r->T;
                if (r->T > r->lmax) r->lmax = r->T;
            }
            if (r->land[b] < 0.f || r->T < r->land[b] + CR_LAND_S) all = false;
        }
        if (!r->all) { r->all = true; for (int b = 0; b < CR_NB; b++) if (r->land[b] < 0.f) r->all = false; }   /* every part has landed */
        if (all && r->T > r->lmax + CR_LAND_S + 2.f * dt) { r->mode = ON; s->p.c2 = r->c2; r->ver++; r->calm = CR_CALM_S; }
    } else if (r->mode == OUT) {
        r->red = expf(-r->T / 0.25f);
        if (r->T > CR_DONE_S) { r->mode = OFF; s->p.c2 = r->c2; r->ver++; r->calm = CR_CALM_S; r->im = 2; return false; }
    }

    /* the dots: the ring where its part has landed, row by row (going, row by row too), and the crest's colour */
    const int H = r->H, W = r->W;
    for (int j = 0; j < H; j++) {                 /* the ring's own: a row made again only when it changes */
        const float rowd = (float)(j * 11 % H) * (CR_LAND_S / H);                    /* its row's turn */
        const uint32_t ph = (frame + (uint32_t)((j * 11 % H) * CR_TW / H)) / CR_TW;   /* its twinkle's */
        /* a row changes at one moment only (it lands once every part has; letting go, it leaves): made again the frame
         * that is crossed */
        const float tp = r->T - dt;
        #define CROSS(t) (tp < (t) && r->T >= (t))
        const bool moving = r->mode == OUT ? CROSS(rowd) : r->mode == IN && r->all && CROSS(r->lmax + rowd);
        #undef CROSS
        if (!moving && r->rowph[j] == ph && r->rowver[j] == r->ver) continue;
        r->rowph[j] = ph; r->rowver[j] = r->ver;
        memset(r->rl + j * W, 0, (size_t)W); memset(r->rll + j * W, 0, (size_t)W);
        for (int n = r->brow[j]; n < r->brow[j + 1]; n++) {
            const cr_dot_t *d = &r->dot[n];
            const float rad = d->r * (1.f / 65535.f);
            const bool shown = r->mode == OUT ? r->T < rowd : r->mode != OFF && r->all && r->T >= r->lmax + rowd;
            if (!shown || rad < CR_LINE_IN || rad > CR_RIM || r->lr[d->bin] <= 0.f) continue;   /* the line alone, to the round rim */
            const uint32_t hh = hash2((uint32_t)n, ph);
            const float tw = (float)(hh >> 8 & 255) * (2.f / 255.f) - 1.f;   /* its twinkle, -1..1 */
            const float b = (0.9f + 0.1f * tw) * r->lr[d->bin];
            if (b <= 0.02f) continue;
            const int q = (int)(b * 32.f + 0.5f);
            r->rl[d->k] |= d->bit;
            if (q > r->rll[d->k]) { r->rll[d->k] = (uint8_t)(q > 127 ? 127 : q); r->rlb[d->k] = d->bin; }
        }
    }
    r->im = r->mode == IN || r->mode == OUT ? CR_ILACE_IN : 2;   /* the drawer's too (cr_ilace, fluid_draw_opts_t.frame) */
    r->ilace = (uint8_t)((frame & 0x0FFFFFFFu) % r->im);   /* as fluid_ilace_row */
    if (r->mode == IN || r->mode == OUT) {        /* the crest: this frame's half of the cell rows (the panel's cost is
                                                   * by row), the other half's kept from the last */
        r->crest = true;
        for (int j = r->ilace; j < H; j += r->im) {   /* the rows the drawer sends the water's changes in this frame */
            memset(r->cbits + j * W, 0, (size_t)W); memset(r->cb + j * W, 0, (size_t)W);
            for (int pass = 0; pass < (r->mode == OUT ? 2 : 1); pass++) {
                /* forming: the dots the crest can colour (a list); letting go: the band's and the inner ones */
                const bool list = r->mode == IN;
                const int n0 = list ? r->crow[j] : pass ? r->irow[j] : r->brow[j], n1 = list ? r->crow[j + 1] : pass ? r->irow[j + 1] : r->brow[j + 1];
                const uint16_t rmin = (uint16_t)((r->mode == OUT ? CR_CREST_IN : CR_CREST_R) * 65535.f);
                const int16_t hmin = (int16_t)((r->mode == OUT ? 0.02f : 0.015f) * FQ_H);
                const int red = (int)(r->red * 255.f), fade = r->mode == OUT ? (int)((0.3f + 0.7f * r->red) * 256.f) : 256;
                for (int m = n0; m < n1; m++) {
                    const cr_dot_t *d = &r->dot[list ? r->cdot[m] : m];
                    if (d->r < rmin) continue;                                     /* not where the crest carries colour */
                    const int16_t hq = s->h[d->k];
                    if (hq <= hmin || creature[d->k]) continue;                     /* the water not up there: no crest */
                    const int hi = hq > 127 ? 127 : hq;                            /* its lift, x 255, from the tables */
                    const int lift = r->mode == OUT ? red * r->hout[hi] / 255 : d->cw * r->hin[hi] / 255;
                    if ((lift >> 1) <= d->h3) continue;
                    const int q = ((14 * 255 + 26 * lift) / 255) * fade >> 8;      /* (0.45 + 0.8 lift) x 32, faded */
                    r->cbits[d->k] |= d->bit; if (q > r->cb[d->k]) r->cb[d->k] = (uint8_t)(q > 255 ? 255 : q);
                }
            }
        }
    } else if (r->crest) { memset(r->cbits, 0, (size_t)s->N); memset(r->cb, 0, (size_t)s->N); r->crest = false; }
    uint32_t c0 = 0;                              /* the ring's colour where a bin has none of its own (the crest) */
    for (int b = 0; b < CR_NB && !c0; b++) c0 = r->col[b];
    /* the listen ring's colour while it shows or lets go (its ink is not in the target); the progress ring's and a
     * question's arcs' their own (their ink is) */
    const bool lis = r->listen >= 0 && r->lisring;
    const int nc = r->mode == OUT || r->crest ? r->ncell : r->nbcell;
    for (int q = 0; q < nc; q++) {                /* each ring cell: its dots and the crest's, the brightest's colour */
        const int k = r->cells[q];
        uint8_t bits = r->rl[k], lvl = r->rll[k], bn = r->rlb[k];
        if (r->cbits[k] && !creature[k]) { bits |= r->cbits[k]; if (r->cb[k] > lvl) { lvl = r->cb[k]; bn = 0; } }
        if (!bits) continue;
        dots[k] |= bits;
        const uint32_t c = lis ? (uint32_t)r->listen : r->col[bn] ? r->col[bn] : c0;
        if (c != r->lutc) {                       /* the brightness table for this colour (eighths, a hot white past 1) */
            r->lutc = c;
            for (int e = 0; e < 64; e++) {
                const float bq = e * (1.f / 8.f), kk = fminf(bq, 1.f), hot = fmaxf(0.f, bq - 1.f) * 0.55f;
                const float R = (c & 255) * kk, G = (c >> 8 & 255) * kk, B = (c >> 16 & 255) * kk;
                r->lut[e][0] = (uint16_t)((R + (255.f - R) * hot) * 257.f); r->lut[e][1] = (uint16_t)((G + (255.f - G) * hot) * 257.f);
                r->lut[e][2] = (uint16_t)((B + (255.f - B) * hot) * 257.f);
            }
        }
        int e = ((lvl ? lvl : 1) + 2) / 4; if (e > 63) e = 63;
        col[k].r = r->lut[e][0]; col[k].g = r->lut[e][1]; col[k].b = r->lut[e][2]; col[k].a = 0;
    }
    return true;
}

void cr_voice(cr_t *r, float level) { if (r) r->vin = level < 0.f ? 0.f : level > 1.f ? 1.f : level; }
int  cr_ilace(const cr_t *r) { return r && r->im ? r->im : 2; }

void cr_listen(cr_t *r, int32_t rgb, float ink) {
    if (!r) return;
    r->listen = rgb < 0 ? -1 : (int32_t)((rgb >> 16 & 255) | (rgb & 0xff00) | (rgb & 255) << 16);
    r->lq = (int16_t)(ink <= 0.f ? 0 : ink >= 1.f ? 16384 : (int)(ink * 16384.f));
}
