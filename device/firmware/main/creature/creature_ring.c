/*
 * SPDX-License-Identifier: MIT
 *
 * creature_ring.c: the rings as dust over a rigged creature's crisp cells (creature_ring.h).
 *
 * Made once per grid: the rim's dots (a band inside the disc's edge), each with its cell, braille bit, angle bin, its
 * distance from each ring line in dots, and a seed. Each frame: each angle bin's amount is the target's ink at the line
 * there (the ring's own fade, a question's arcs, the progress comet), eased; a dot shows when a twinkle hash falls
 * under the line's profile at its distance, moved by the forming / dissolving scatter and ripple.
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

#define CR_NB      256       /* angle bins                                                                     */
#define CR_IN      0.68f     /* the band: dots from this radius (of the disc's) out                           */
#define CR_ARC_R   0.92f     /* a question's arcs' line (fluid_ask.h FLUID_ASK_ARC_R)                          */
#define CR_EASE_S  0.08f     /* a bin's forming / dissolving ...                                                */
#define CR_SWEEP_S 0.6f      /* ... begun round the ring clockwise from 12 over this (dissolving: back): only a  
                              * part of the rim changes in a frame, so the panel is sent a part              */
#define CR_SCAT    4.f       /* dots of scatter, unformed                                                      */
#define CR_RIP     1.5f      /* dots of ripple, unformed                                                       */
#define CR_TW      36        /* each dot row's twinkle every this many frames (rows scattered, a few a frame)   */
#define CR_LUT0    (-4.f)    /* the profile's table: dots from the line (+: inward), quarter dots              */
#define CR_LUTN    96

typedef struct { uint16_t k; uint8_t bit, bin; int8_t dr, da; uint8_t seed, row; } cr_dot_t;   /* dr, da: quarter dots */

struct cr {
    int W, H, n;
    cr_dot_t *dot;
    int16_t kr[CR_NB], ka[CR_NB];                 /* each bin's cell on the ring's line and on the arcs' line    */
    float er[CR_NB], ea[CR_NB];                   /* each bin's amount, easing                                   */
    uint32_t cr[CR_NB], ca[CR_NB];                /* ... and its colour (tcol), kept while it dissolves          */
    float lr[CR_NB], la[CR_NB];                   /* ... and its last amounts, held until the sweep back takes it */
    float rip[CR_NB];                             /* the ripple's phase per bin (its angle x 6)                  */
    float ripv[CR_NB];                            /* ... this frame's ripple there, in dots (one sinf a bin)     */
    float ub[CR_NB], gr[CR_NB], ga[CR_NB];        /* ... how unformed it is there, and each line's eased amount  */
    uint8_t lut[CR_LUTN];                         /* the line's profile                                          */
    uint8_t *rd, *rden; uint16_t *rsrc;           /* per cell, this frame: ring dots, their strongest, its bin   */
    float t, sweep, sum;                           /* the sweep round: 0 none .. 1 all of it; last frame's ink     */
    bool any;
};

static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static uint32_t hash2(uint32_t a, uint32_t b) { uint32_t h = a * 374761393u + b * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return h ^ (h >> 16); }

/* the line's profile at d dots from it (+: inward): a thin core (most of its dots), feathered half a dot outward, dust
 * inward thinning to nothing in a few dots */
static float profile(float d) {
    if (d < -0.5f) { const float e = (-d - 0.5f) / 0.5f; return 0.85f * expf(-e * e); }
    if (d <= 0.5f) return 0.85f;
    return 0.4f * expf(-(d - 0.5f) / 1.6f);
}

static int nearest(const float *c, int n, float v) { int b = 0; for (int i = 1; i < n; i++) if (fabsf(c[i] - v) < fabsf(c[b] - v)) b = i; return b; }

cr_t *cr_create(const fluid_sim_t *s) {
    cr_t *r = CR_CALLOC(sizeof(cr_t));
    if (!r) return NULL;
    const int W = s->W, H = s->H;
    r->W = W; r->H = H;
    for (int q = 0; q < CR_LUTN; q++) r->lut[q] = (uint8_t)(255.f * profile(CR_LUT0 + q * 0.25f) + 0.5f);
    for (int b = 0; b < CR_NB; b++) {
        const float a = (b + 0.5f) * (6.2831853f / CR_NB), sx = sinf(a), sy = -cosf(a);   /* clockwise from 12 */
        r->kr[b] = (int16_t)(nearest(s->ys, H, RING_R * sy) * W + nearest(s->xs, W, RING_R * sx));
        r->ka[b] = (int16_t)(nearest(s->ys, H, CR_ARC_R * sy) * W + nearest(s->xs, W, CR_ARC_R * sx));
        r->rip[b] = a * 6.f;
    }
    for (int pass = 0; pass < 2; pass++) {
        int n = 0;
        for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
            const int k = j * W + i;
            if (!s->mask[k]) continue;
            for (int q = 0; q < 8; q++) {
                const int c = q >> 2, row = q & 3;
                const float x = s->xs[i] + (c ? 0.5f : -0.5f) / W, y = s->ys[j] + ((row + 0.5f) / 4.f - 0.5f) * 2.f / H;
                const float rad = sqrtf(x * x + y * y);
                if (rad < CR_IN || rad > 1.f) continue;
                if (pass) {
                    const float foot = (fabsf(x) / W + fabsf(y) / (2.f * H)) / rad;   /* a dot's pitch along the radius */
                    float a = atan2f(x, -y); if (a < 0.f) a += 6.2831853f;
                    cr_dot_t *d = &r->dot[n];
                    d->k = (uint16_t)k; d->bit = (uint8_t)(1 << fluid_braille_bit(c, row));
                    d->bin = (uint8_t)((int)(a * (CR_NB / 6.2831853f)) & (CR_NB - 1));
                    d->dr = (int8_t)clampf(4.f * (RING_R - rad) / foot, -127.f, 127.f);
                    d->da = (int8_t)clampf(4.f * (CR_ARC_R - rad) / foot, -127.f, 127.f);
                    d->seed = (uint8_t)(hash2((uint32_t)k, (uint32_t)q) >> 24); d->row = (uint8_t)(j * 4 + row);
                }
                n++;
            }
        }
        if (!pass) {
            r->n = n;
            r->dot = CR_CALLOC(sizeof(cr_dot_t) * (size_t)(n ? n : 1));
            r->rd = CR_CALLOC((size_t)W * H); r->rden = CR_CALLOC((size_t)W * H); r->rsrc = CR_CALLOC(sizeof(uint16_t) * (size_t)W * H);
            if (!r->dot || !r->rd || !r->rden || !r->rsrc) { cr_destroy(r); return NULL; }
        }
    }
    return r;
}

void cr_destroy(cr_t *r) {
    if (!r) return;
    CR_FREE(r->dot); CR_FREE(r->rd); CR_FREE(r->rden); CR_FREE(r->rsrc); CR_FREE(r);
}

static inline float ease(float x) { return x * x * (3.f - 2.f * x); }
static inline int prof_at(const cr_t *r, float d) { int q = (int)((d - CR_LUT0) * 4.f); return q < 0 || q >= CR_LUTN ? 0 : r->lut[q]; }

bool cr_paint(cr_t *r, const fluid_sim_t *s, float dt, uint32_t frame, uint8_t *dots, fluid_rgba16_t *col) {
    /* each bin's amount: the target's ink at its line (the stronger line wins the bin), eased in and out */
    const float step = dt / CR_EASE_S, norm = 1.f / (FQ_UNIT * RING_INK);
    bool any = false;
    float sum = 0.f;                               /* the lines' ink: rising or holding, a ring is wanted; falling (its
                                                    * own fade out), it is let go, the sweep taking it back */
    for (int b = 0; b < CR_NB; b++) sum += (float)(s->T[r->kr[b]] > s->T[r->ka[b]] ? s->T[r->kr[b]] : s->T[r->ka[b]]);
    const bool want = sum > 0.f && sum >= 0.97f * r->sum;
    r->sum = sum;
    r->sweep = clampf(r->sweep + (want ? dt : -dt) / CR_SWEEP_S, 0.f, 1.f);
    const int reach = (int)(r->sweep * CR_NB + 0.5f);                /* bins the sweep has reached */
    for (int b = 0; b < CR_NB; b++) {
        const int tr = s->T[r->kr[b]], ta = s->T[r->ka[b]];
        /* a third of the line's ink shows it whole: its own fade in does not redraw the rim each frame (the sweep forms
         * it); a progress comet's tail still thins */
        if (want) { r->lr[b] = tr >= ta ? clampf(tr * norm * 3.f, 0.f, 1.f) : 0.f; r->la[b] = ta > tr ? clampf(ta * norm * 3.f, 0.f, 1.f) : 0.f; }
        const float ar = b < reach ? r->lr[b] : 0.f, aa = b < reach ? r->la[b] : 0.f;
        if (ar > 0.f) r->cr[b] = s->tcol[r->kr[b]];
        if (aa > 0.f) r->ca[b] = s->tcol[r->ka[b]];
        r->er[b] += clampf(ar - r->er[b], -step, step);
        r->ea[b] += clampf(aa - r->ea[b], -step, step);
        any |= r->er[b] > 0.f || r->ea[b] > 0.f;
    }
    r->t += dt;
    if (!any) { r->any = false; return false; }
    r->any = true;
    const int H = r->H;
    for (int n = 0; n < r->n; n++) { const int k = r->dot[n].k; r->rd[k] = 0; r->rden[k] = 0; }
    const float rt = r->t * 3.f;
    for (int b = 0; b < CR_NB; b++) {
        r->gr[b] = ease(r->er[b]); r->ga[b] = ease(r->ea[b]);
        const float u = r->ub[b] = 1.f - (r->gr[b] > r->ga[b] ? r->gr[b] : r->ga[b]);
        r->ripv[b] = u > 0.f && u < 1.f ? u * CR_RIP * sinf(r->rip[b] + rt) : 0.f;
    }
    for (int n = 0; n < r->n; n++) {
        const cr_dot_t *d = &r->dot[n];
        const float gr = r->gr[d->bin], ga = r->ga[d->bin];
        if (gr <= 0.f && ga <= 0.f) continue;
        /* unformed, it is scattered dust with a ripple running round; formed, the line; the dust thins as it goes */
        const float off = r->ub[d->bin] * CR_SCAT * (d->seed - 127.5f) * (1.f / 127.5f) + r->ripv[d->bin];
        const int pr = gr > 0.f ? (int)(prof_at(r, d->dr * 0.25f + off) * gr) : 0;
        const int pa = ga > 0.f ? (int)(prof_at(r, d->da * 0.25f + off) * ga) : 0;
        const int den = pr > pa ? pr : pa;
        if (den <= 0) continue;
        const uint32_t ph = (frame + (uint32_t)(((d->row >> 2) * 11 % H) * CR_TW / H)) / CR_TW;   /* its row's twinkle */
        if ((int)(hash2((uint32_t)n, ph) >> 24) >= den) continue;
        r->rd[d->k] |= d->bit;
        if (den > r->rden[d->k]) { r->rden[d->k] = (uint8_t)den; r->rsrc[d->k] = (uint16_t)(d->bin | (pr >= pa ? 0 : 0x100)); }
    }
    for (int n = 0; n < r->n; n++) {                  /* each lit cell: its dots and the ring's colour, dimmer in the dust */
        const int k = r->dot[n].k;
        if (!r->rd[k] || r->rden[k] == 0) continue;
        const int b = r->rsrc[k] & 0xff;
        const uint32_t c = r->rsrc[k] & 0x100 ? r->ca[b] : r->cr[b];
        const float br = 0.35f + 0.65f * r->rden[k] * (1.f / 255.f);
        dots[k] |= r->rd[k];
        col[k].r = (uint16_t)((c & 255) * 257 * br); col[k].g = (uint16_t)((c >> 8 & 255) * 257 * br); col[k].b = (uint16_t)((c >> 16 & 255) * 257 * br);
        col[k].a = 0;
        r->rden[k] = 0;                               /* once a cell */
    }
    return true;
}
