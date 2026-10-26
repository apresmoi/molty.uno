/*
 * fluid_sim.c — fluid core: params, sim step, view, field synth.
 *
 * Port of creatures/js/fluid/sim.js + synth.js. See fluid_sim.h.
 * Deviations from the JS, all for speed on the S3 (measured on device):
 *   - hot scalar fields are int16 fixed point in internal DRAM (math in float, mostly
 *     in raw fixed-point units so loads need no rescale); PSRAM-resident floats made
 *     every per-cell pass memory-bound.
 *   - colour/accent are advected + relaxed once per FRAME (fluid_sim_color) as packed
 *     RGBA16 in PSRAM, not 5 float fields per 60 Hz step.
 *   - the eddy stream function ψ is computed once per frame (fluid_sim_swirl_field)
 *     and its curl inline where it is used (no su/sv fields).
 *   - noise3 hashes lattice points with integer mixing, floors without libm, and the
 *     synth sine is a polynomial; the box blurs are integer running sums.
 */
#include "fluid_sim.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define FLUID_CALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#define FLUID_FREE(p)   heap_caps_free(p)
/* hot fields: each one in internal DRAM while ≥ 32 KB of internal heap stays free for
 * Wi-Fi / lwip / httpd / OTA, otherwise PSRAM (correct, just slower). Per field, not
 * one block: the internal heap is fragmented into regions, so one ~50 KB block often
 * fails where 3 KB fields fit. */
#define FLUID_INTERNAL_RESERVE (32 * 1024)
static void *calloc_fast(size_t n, bool *internal) {
    void *p = NULL;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= n + FLUID_INTERNAL_RESERVE)
        p = heap_caps_calloc(1, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    *internal = p != NULL;
    return p ? p : heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
}
#else
#define FLUID_CALLOC(n) calloc(1, (n))
#define FLUID_FREE(p)   free(p)
static void *calloc_fast(size_t n, bool *internal) { *internal = true; return calloc(1, n); }
#endif

#define TAU 6.2831853f
#define INV_UNIT (1.f / FQ_UNIT)
#define INV_PHI  (1.f / FQ_PHI)
#define INV_H    (1.f / FQ_H)
#define INV_U    (1.f / FQ_U)

uint32_t fluid_prof[FP_N];
const char *const FLUID_PROF_NAMES[FP_N] = {
    "target", "bg", "swirl", "wave", "demand", "flow", "advect", "relax", "color", "view",
    "c_update", "c_raster", "c_clear", "c_vert", "c_samples", "c_agg", "c_fill", "d_sample", "d_raster",
};

static inline int ffloor(float x) { int i = (int)x; return i - (x < (float)i); }
static inline int16_t sat16(float x) { return fq(x, 1.f); }   /* raw-unit store */

/* ── params ──────────────────────────────────────────────────────────────── */

void fluid_params_default(fluid_params_t *p) {
    memset(p, 0, sizeof(*p));
    p->c2 = 0.11f;  p->damp = 0.975f; p->leak = 0.985f;
    p->attract = 70.f; p->drag = 2.6f; p->vmax = 30.f;
    p->relax_up = 1.1f; p->relax_down = 1.8f; p->tint_rate = 3.f;
    p->swirl = 0.7f; p->swirl_freq = 1.6f;
    p->refr = 1.1f; p->crest = 0.55f;
    p->speed_mul = 1.f; p->hold = 0.f;
    p->bg_level = 0.2f; p->bg_freq = 2.3f; p->bg_drift = 0.05f; p->bg_speed = 0.12f;
    p->bg_rim = 0.78f; p->bg_mob = 0.45f;
    p->bg_color[0] = 0.52f; p->bg_color[1] = 0.58f; p->bg_color[2] = 0.86f;
}

/* ── create / destroy ────────────────────────────────────────────────────── */

#define N16 13      /* h hp ink ink2 u v D phi tmp psi T view bgn */
#define N8  5       /* mask sponge rim bgm tf */

void fluid_grid_for_stage(int stage, int *W, int *H) {
    int w = (int)lrintf(52.f * (float)stage / FLUID_STAGE_REF), h = (int)lrintf(32.f * (float)stage / FLUID_STAGE_REF);
    *W = w < FLUID_GRID_MIN_W ? FLUID_GRID_MIN_W : w > FLUID_GRID_MAX_W ? FLUID_GRID_MAX_W : w;
    *H = h < FLUID_GRID_MIN_H ? FLUID_GRID_MIN_H : h > FLUID_GRID_MAX_H ? FLUID_GRID_MAX_H : h;
}

fluid_sim_t *fluid_sim_create(int W, int H, int stage) {
    if (W < FLUID_GRID_MIN_W || W > FLUID_GRID_MAX_W || H < FLUID_GRID_MIN_H || H > FLUID_GRID_MAX_H) return NULL;
    if (stage < 32 || stage > 4096) return NULL;
    fluid_sim_t *s = FLUID_CALLOC(sizeof(fluid_sim_t));
    if (!s) return NULL;
    const int N = W * H;
    /* hottest first, so a tight heap keeps the per-step fields internal */
    int16_t **f16[N16] = { &s->h, &s->hp, &s->ink, &s->ink2, &s->D, &s->phi, &s->tmp, &s->u, &s->v, &s->psi, &s->T, &s->view, &s->bgn };
    uint8_t **f8[N8] = { &s->mask, &s->sponge, &s->rim, &s->bgm, &s->tf };
    int nb = 0;
    bool in;
    for (int k = 0; k < N16; k++) {
        *f16[k] = calloc_fast((size_t)N * sizeof(int16_t), &in);
        s->blk[nb++] = *f16[k]; s->n_internal += in;
    }
    for (int k = 0; k < N8; k++) {
        *f8[k] = calloc_fast((size_t)N, &in);
        s->blk[nb++] = *f8[k]; s->n_internal += in;
    }
    s->xs = calloc_fast((size_t)(W + H) * sizeof(float), &in);
    s->blk[nb++] = s->xs; s->n_internal += in;
    s->n_blk = nb;
    s->cblock = FLUID_CALLOC((size_t)N * (2 * sizeof(fluid_rgba16_t) + sizeof(uint32_t)));
    for (int k = 0; k < nb; k++) if (!s->blk[k]) { fluid_sim_destroy(s); return NULL; }
    if (!s->cblock) { fluid_sim_destroy(s); return NULL; }
    s->internal = s->n_internal == nb;
    s->ys = s->xs + W;
    s->col = (fluid_rgba16_t *)s->cblock; s->col2 = s->col + N; s->tcol = (uint32_t *)(s->col2 + N);

    s->W = W; s->H = H; s->N = N; s->stage = stage;
    s->A = ((float)stage / H) / ((float)stage / W);
    fluid_params_default(&s->p);
    for (int i = 0; i < W; i++) s->xs[i] = ((i + 0.5f) / W) * 2.f - 1.f;
    for (int j = 0; j < H; j++) s->ys[j] = ((j + 0.5f) / H) * 2.f - 1.f;
    int inside = 0;
    const float br = s->p.bg_rim;
    const uint16_t c06 = (uint16_t)(0.6f * 65535.f);
    for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
        int k = j * W + i;
        float x = s->xs[i], y = s->ys[j], rr = sqrtf(x * x + y * y);
        float sp = (rr - 0.86f) / 0.14f;                    /* absorbing rim: ripples fade out */
        sp = sp < 0.f ? 0.f : sp > 1.f ? 1.f : sp;
        s->sponge[k] = (uint8_t)(sp * 255.f + 0.5f);
        if (x * x + y * y <= 1.f && i > 0 && j > 0 && i < W - 1 && j < H - 1) { s->mask[k] = 1; inside++; }
        float rim = 1.f;
        if (rr >= br) { float t = 1.f - (rr - br) / (1.f - br); t = t < 0.f ? 0.f : t; rim = t * t; }
        s->rim[k] = (uint8_t)(rim * 255.f + 0.5f);
        s->bgm[k] = 255;
        s->col[k].r = s->col[k].g = s->col[k].b = c06; s->col[k].a = 0;
        s->col2[k] = s->col[k];
        s->tcol[k] = fluid_pack_rgba8(0.6f, 0.6f, 0.6f, 0.f);
    }
    s->inside = inside > 0 ? inside : 1;
    s->osc.freq = 4.f; s->vibe.freq = 9.f; s->sheet.freq = 1.5f;
    s->err = 1.f;
    return s;
}

void fluid_sim_destroy(fluid_sim_t *s) {
    if (!s) return;
    for (int k = 0; k < s->n_blk; k++) if (s->blk[k]) FLUID_FREE(s->blk[k]);
    if (s->cblock) FLUID_FREE(s->cblock);
    FLUID_FREE(s);
}

/* ── noise / synth ───────────────────────────────────────────────────────── */

static inline float hash3(int x, int y, int z) {
    uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u ^ (uint32_t)z * 0xcb1ab31fu;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return (float)(h & 0xFFFFFFu) * (1.f / 16777216.f);
}

/* time is the third lattice axis, so the pattern morphs in place instead of sliding */
float fluid_noise3(float x, float y, float z) {
    int xi = ffloor(x), yi = ffloor(y), zi = ffloor(z);
    float xf = x - xi, yf = y - yi, zf = z - zi;
    float u = xf * xf * (3.f - 2.f * xf), v = yf * yf * (3.f - 2.f * yf), w = zf * zf * (3.f - 2.f * zf);
    float a000 = hash3(xi, yi, zi),         a100 = hash3(xi + 1, yi, zi);
    float a010 = hash3(xi, yi + 1, zi),     a110 = hash3(xi + 1, yi + 1, zi);
    float a001 = hash3(xi, yi, zi + 1),     a101 = hash3(xi + 1, yi, zi + 1);
    float a011 = hash3(xi, yi + 1, zi + 1), a111 = hash3(xi + 1, yi + 1, zi + 1);
    float f0 = (a000 + (a100 - a000) * u) * (1.f - v) + (a010 + (a110 - a010) * u) * v;
    float f1 = (a001 + (a101 - a001) * u) * (1.f - v) + (a011 + (a111 - a011) * u) * v;
    return f0 + (f1 - f0) * w;
}

/* max error ~0.005 rad — plenty for a field-synth phase */
float fluid_atan2(float y, float x) {
    float ax = fabsf(x), ay = fabsf(y);
    float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
    if (mx < 1e-12f) return 0.f;
    float a = mn / mx, s2 = a * a;
    float r = ((-0.0464964749f * s2 + 0.15931422f) * s2 - 0.327622764f) * s2 * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0.f) r = 3.14159274f - r;
    return y < 0.f ? -r : r;
}

#define sin_turns fluid_sin_turns

static inline float synth_wave(int kind, float t) {
    switch (kind) {
    case FW_SAW: { float p = t - (float)ffloor(t); return 2.f * p - 1.f; }
    case FW_TRI: { float p = t - (float)ffloor(t); return 4.f * fabsf(p - 0.5f) - 1.f; }
    default:     return sin_turns(t);
    }
}

/* A field-synth patch → 0..1 at (x,y) in −1..1 stage space. combine = add, so the
 * mix-weight fold reduces to Σ amp·voice. */
float fluid_patch(const fluid_patch_t *p, float x, float y, float t) {
    float c = 0.f;
    for (int k = 0; k < p->nv; k++) {
        const fluid_voice_t *v = &p->v[k];
        float o, raw;
        if (v->field == FV_NOISE) {
            o = 2.f * fluid_noise3(x * v->freq, y * v->freq, t * v->speed) - 1.f;
        } else {
            switch (v->field) {
            case FV_LINX:    raw = x; break;
            case FV_LINY:    raw = y; break;
            case FV_ANGULAR: raw = fluid_atan2(y, x) / TAU; break;
            case FV_SPIRAL:  raw = sqrtf(x * x + y * y) + fluid_atan2(y, x) / TAU; break;
            default:         raw = sqrtf(x * x + y * y); break;
            }
            o = synth_wave(v->wave, raw * v->freq - t * v->speed);
        }
        c += v->amp * o;
    }
    return fluid_clamp01(p->bias + p->gain * c * 0.5f);
}

/* ── forcing ─────────────────────────────────────────────────────────────── */

void fluid_sim_impulse(fluid_sim_t *s, float x, float y, float strength, float radius) {
    int W = s->W, H = s->H;
    float ci = (x + 1.f) * 0.5f * W, cj = (y + 1.f) * 0.5f * H;
    float ri = ceilf(radius * W), rj = ceilf(radius * H);
    int j0 = (int)floorf(cj - rj), j1 = (int)ceilf(cj + rj);
    int i0 = (int)floorf(ci - ri), i1 = (int)ceilf(ci + ri);
    if (j0 < 1) j0 = 1;
    if (j1 > H - 1) j1 = H - 1;
    if (i0 < 1) i0 = 1;
    if (i1 > W - 1) i1 = W - 1;
    float inv = 1.f / (radius * radius);
    for (int j = j0; j < j1; j++) for (int i = i0; i < i1; i++) {
        int k = j * W + i;
        if (!s->mask[k]) continue;
        float dx = s->xs[i] - x, dy = s->ys[j] - y, d2 = (dx * dx + dy * dy) * inv;
        if (d2 < 4.f) s->h[k] = sat16((float)s->h[k] + strength * expf(-d2 * 2.f) * FQ_H);
    }
}

void fluid_sim_push(fluid_sim_t *s, float x, float y, float du, float dv, float radius) {
    float inv = 1.f / (radius * radius);
    for (int j = 0; j < s->H; j++) for (int i = 0; i < s->W; i++) {
        int k = j * s->W + i;
        if (!s->mask[k]) continue;
        float dx = s->xs[i] - x, dy = s->ys[j] - y, d2 = (dx * dx + dy * dy) * inv;
        if (d2 > 3.9f) continue;                      /* w = e^-d2 < 0.02 */
        float w = expf(-d2) * FQ_U;
        s->u[k] = sat16((float)s->u[k] + du * w); s->v[k] = sat16((float)s->v[k] + dv * w);
    }
}

/* floor = level · breath · (0.3 + 0.7·1.6·n²) · rimFade · BGM; colour → bgColor where it wins */
void fluid_sim_background(fluid_sim_t *s, float t, float breath, bool refresh) {
    const fluid_params_t *p = &s->p;
    FLUID_PROF_BEGIN(c0);
    if (refresh) {          /* the floor's noise drifts ≤ 0.12 lattice units/s: refresh it every other frame */
        const float f = p->bg_freq, tx = t * p->bg_drift, tz = t * p->bg_speed;
        for (int j = 0; j < s->H; j++) {
            const float yy = s->ys[j] * f * 1.4f;
            for (int i = 0; i < s->W; i++) {
                const int k = j * s->W + i;
                float n = s->mask[k] ? fluid_noise3(s->xs[i] * f + tx, yy, tz) : 0.f;
                s->bgn[k] = (int16_t)((0.3f + 1.12f * n * n) * 16384.f * 0.5f);    /* ≤ 1.42 → Q13 */
            }
        }
    }
    const float lv = p->bg_level * (0.6f + 0.4f * breath) / (255.f * 255.f * 8192.f);
    const float *c = p->bg_color;
    (void)t;
    for (int j = 0; j < s->H; j++) {
        for (int i = 0; i < s->W; i++) {
            const int k = j * s->W + i;
            if (!s->mask[k]) continue;
            float bg = lv * (float)s->bgn[k] * (float)s->rim[k] * (float)s->bgm[k];
            float tk = s->T[k] * INV_UNIT;
            if (tk < bg) {
                float w = tk < 0.02f ? 1.f : 1.f - tk / bg;
                uint32_t tc = s->tcol[k];
                float r = (tc & 255) * (1.f / 255.f), g = (tc >> 8 & 255) * (1.f / 255.f);
                float b = (tc >> 16 & 255) * (1.f / 255.f), a = (tc >> 24) * (1.f / 255.f);
                s->T[k] = fq(bg, FQ_UNIT);
                s->tcol[k] = fluid_pack_rgba8(r + (c[0] - r) * w, g + (c[1] - g) * w, b + (c[2] - b) * w, a * (1.f - w));
            }
        }
    }
    FLUID_PROF_END(FP_BG, c0);
}


/* ── step ────────────────────────────────────────────────────────────────── */

/* 32768/n, for the integer box blur's divide */
static const int32_t RCP15[33] = {
    0, 32768, 16384, 10923, 8192, 6554, 5461, 4681, 4096, 3641, 3277, 2979, 2731, 2521, 2341, 2185, 2048,
    1928, 1820, 1725, 1638, 1560, 1489, 1425, 1365, 1311, 1260, 1214, 1170, 1130, 1092, 1057, 1024,
};

/* 1-D box filter over n samples at stride st, radius r (requires n ≥ 2r+2): the
 * head/tail shrink the window at the edges (edge-normalised mean, like the JS); the
 * middle has no branches — this runs 4× per step over every cell. */
static inline void box_line(const int16_t *in, int16_t *out, int n, int st, int r) {
    int32_t sum = 0;
    for (int i = 0; i <= r; i++) sum += in[i * st];
    int cnt = r + 1, i = 0;
    for (; i < r; i++) {
        out[i * st] = (int16_t)((sum * RCP15[cnt] + 16384) >> 15);
        sum += in[(i + r + 1) * st]; cnt++;
    }
    const int32_t full = RCP15[2 * r + 1];
    for (; i < n - r - 1; i++) {
        out[i * st] = (int16_t)((sum * full + 16384) >> 15);
        sum += in[(i + r + 1) * st] - in[(i - r) * st];
    }
    for (; i < n; i++) {
        out[i * st] = (int16_t)((sum * RCP15[cnt] + 16384) >> 15);
        sum -= in[(i - r) * st]; cnt--;
    }
}

/* separable box blur, radius rx (columns) × ry (rows), integer running sums: O(N) */
static void box_blur(const int16_t *src, int16_t *dst, int16_t *tmp, int W, int H, int rx, int ry) {
    if (rx > (W - 2) / 2) rx = (W - 2) / 2;
    if (ry > (H - 2) / 2) ry = (H - 2) / 2;
    for (int j = 0; j < H; j++) box_line(src + j * W, tmp + j * W, W, 1, rx);
    for (int i = 0; i < W; i++) box_line(tmp + i, dst + i, H, W, ry);
}

void fluid_sim_swirl_field(fluid_sim_t *s) {
    float f = s->p.swirl_freq * 2.f, tz = s->t * 0.35f;
    FLUID_PROF_BEGIN(c0);
    for (int j = 0; j < s->H; j++) {
        const float yy = s->ys[j] * f;
        for (int i = 0; i < s->W; i++) {
            const int k = j * s->W + i;
            s->psi[k] = s->mask[k] ? fq(fluid_noise3(s->xs[i] * f + 7.3f, yy, tz), FQ_UNIT) : 0;
        }
    }
    FLUID_PROF_END(FP_SWIRL, c0);
}

/* curl-noise eddies + vortex rotation + inertial flow, scaled by mobility (cells/s) */
typedef struct { float sw, swA, rotW; } flow_k_t;

static inline flow_k_t flow_consts(const fluid_sim_t *s) {
    flow_k_t c;
    c.sw = (s->p.swirl + s->swirl_kick) * s->p.speed_mul * 10.f * 0.5f * INV_UNIT;
    c.swA = c.sw / s->A;
    c.rotW = s->rotate * s->W * 0.5f;
    return c;
}

static inline void flow_cell(const fluid_sim_t *s, const flow_k_t *c, int i, int j, int k, float *wu, float *wv) {
    const int W = s->W;
    const int16_t *psi = s->psi;
    float mob = s->D[k] * INV_PHI;
    float su = (float)(psi[k + W] - psi[k - W]) * c->swA - c->rotW * s->ys[j];
    float sv = -(float)(psi[k + 1] - psi[k - 1]) * c->sw + c->rotW * s->xs[i];
    *wu = (s->u[k] * INV_U + su) * mob;
    *wv = (s->v[k] * INV_U + sv) * mob;
}

void fluid_sim_flow_at(const fluid_sim_t *s, int i, int j, float *wu, float *wv) {
    int k = j * s->W + i;
    if (!s->mask[k]) { *wu = *wv = 0.f; return; }
    flow_k_t c = flow_consts(s);
    flow_cell(s, &c, i, j, k, wu, wv);
}

static inline float bilerp16(const int16_t *a, int W, int k0, float fx, float fy) {
    float t0 = a[k0] + (float)(a[k0 + 1] - a[k0]) * fx;
    float t1 = a[k0 + W] + (float)(a[k0 + W + 1] - a[k0 + W]) * fx;
    return t0 + (t1 - t0) * fy;
}

void fluid_sim_step(fluid_sim_t *s, float dt) {
    const int W = s->W, H = s->H, N = s->N;
    const float A = s->A;
    const fluid_params_t *p = &s->p;
    const uint8_t *mask = s->mask;
    s->t += dt;
    const float sp = p->speed_mul;

    FLUID_PROF_BEGIN(c0);
    /* ---- 1. wave equation (+ forcing), raw Q10 units; result written in place of hp ---- */
    {
        int16_t *h = s->h, *hp = s->hp;
        const float cy = p->c2 / (A * A);
        s->osc.phase += dt * s->osc.freq; s->vibe.phase += dt * s->vibe.freq; s->sheet.phase += dt * s->sheet.freq;
        if (s->osc.phase > 1e4f) s->osc.phase -= floorf(s->osc.phase);
        if (s->vibe.phase > 1e4f) s->vibe.phase -= floorf(s->vibe.phase);
        if (s->sheet.phase > 1e4f) s->sheet.phase -= floorf(s->sheet.phase);
        const float oscV = s->osc.amp * sin_turns(s->osc.phase) * 0.35f * FQ_H;
        const float vibV = s->vibe.amp * sin_turns(s->vibe.phase) * 0.02f * FQ_H;
        const float shV  = s->sheet.amp * sin_turns(s->sheet.phase) * 0.25f * FQ_H;
        const float ox = s->osc.x, oy = s->osc.y;
        const float dk0 = p->damp, dk1 = p->damp * 0.15f / 255.f, leak = p->leak, c2 = p->c2;
        for (int j = 1; j < H - 1; j++) {
            const float y = s->ys[j];
            for (int i = 1; i < W - 1; i++) {
                const int k = j * W + i;
                if (!mask[k]) { hp[k] = 0; continue; }
                const float c = h[k];
                float n = (c + (c - hp[k]) * (dk0 - dk1 * s->sponge[k])
                           + c2 * (float)(h[k - 1] + h[k + 1] - 2 * h[k]) + cy * (float)(h[k - W] + h[k + W] - 2 * h[k])) * leak;
                if (oscV != 0.f) {
                    float dx = s->xs[i] - ox, dy = y - oy, d2 = dx * dx + dy * dy;
                    if (d2 < 0.012f) n += oscV * (1.f - d2 * (1.f / 0.012f));
                }
                if (vibV != 0.f) n += vibV * (s->ink[k] * INV_UNIT + 0.15f) * (((i + j) & 1) ? 1.f : -1.f);
                if (shV != 0.f && y < -0.82f) n += shV;
                hp[k] = sat16(n);
            }
        }
        s->hp = h; s->h = hp;
    }
    FLUID_PROF_END(FP_WAVE, c0); c0 = fluid_cycles();

    /* ---- 2. demand potential φ = blur(T − ink) at two scales (Q13) ---- */
    int16_t *D = s->D, *phi = s->phi, *ink = s->ink, *T = s->T, *big = s->ink2;
    for (int k = 0; k < N; k++) D[k] = mask[k] ? (int16_t)((T[k] - ink[k]) / 2) : 0;   /* Q14 → Q13 */
    int ry2 = (int)lrintf(2.f / A); if (ry2 < 1) ry2 = 1;
    int ry7 = (int)lrintf(7.f / A); if (ry7 < 1) ry7 = 1;
    box_blur(D, phi, s->tmp, W, H, 2, ry2);
    box_blur(D, big, s->tmp, W, H, 7, ry7);
    /* mobility: where the ink already matches its target the fluid HOLDS (no advection
     * → no numerical diffusion → cell-sharp detail); where it doesn't, it flows. */
    {
        float kick_mob = s->swirl_kick * 0.12f; if (kick_mob > 1.f) kick_mob = 1.f;
        const float mob_k = 9.f * (1.f - 0.8f * p->hold) * INV_PHI, bg_mob = p->bg_mob;
        const int16_t ink03 = (int16_t)(0.3f * FQ_UNIT);
        for (int k = 0; k < N; k++) {
            float m = (float)abs(phi[k]) * mob_k + kick_mob;
            if (ink[k] < ink03 && m < bg_mob) m = bg_mob;     /* the dim field always drifts */
            D[k] = (int16_t)((m > 1.f ? 1.f : m) * FQ_PHI + 0.5f);
            phi[k] = sat16(0.5f * phi[k] + 0.9f * big[k]);
        }
    }
    FLUID_PROF_END(FP_DEMAND, c0); c0 = fluid_cycles();

    /* ---- 3. inertial flow toward demand (+ IMU tilt bias), raw Q9 ---- */
    float vsum = 0.f;
    {
        int16_t *u = s->u, *v = s->v;
        const float drag = expf(-p->drag * dt), ka = p->attract * (1.6f - 0.8f * p->hold) * sp * dt;
        const float kx = ka * 0.5f * W * 0.5f * INV_PHI * FQ_U, ky = kx / A;
        const float tbx = p->tilt_x * dt * FQ_U, tby = p->tilt_y * dt * FQ_U;
        const float vmax = p->vmax * FQ_U, vmax2 = vmax * vmax;
        for (int j = 1; j < H - 1; j++) for (int i = 1; i < W - 1; i++) {
            const int k = j * W + i;
            if (!mask[k]) { u[k] = v[k] = 0; continue; }
            float uu = ((float)u[k] + kx * (float)(phi[k + 1] - phi[k - 1]) + tbx) * drag;
            float vv = ((float)v[k] + ky * (float)(phi[k + W] - phi[k - W]) + tby) * drag;
            float m2 = uu * uu + vv * vv;
            if (m2 > vmax2) { float sc = vmax / sqrtf(m2); uu *= sc; vv *= sc; }
            u[k] = sat16(uu); v[k] = sat16(vv);
            vsum += fabsf(uu) + fabsf(vv);
        }
        vsum *= INV_U;
    }
    s->swirl_kick *= expf(-dt * 1.8f);
    FLUID_PROF_END(FP_FLOW, c0); c0 = fluid_cycles();

    /* ---- 4+5. eddies (curl ψ, inline) + semi-Lagrangian advection of ink ---- */
    {
        const flow_k_t fc = flow_consts(s);
        int16_t *i2 = s->ink2;
        const float xmax = W - 1.001f, ymax = H - 1.001f, dtA = dt / A;
        for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
            const int k = j * W + i;
            if (!mask[k]) { i2[k] = 0; continue; }
            if (D[k] == 0) { i2[k] = ink[k]; continue; }
            float wu, wv;
            flow_cell(s, &fc, i, j, k, &wu, &wv);
            float x = i - wu * dt, y = j - wv * dtA;
            x = x < 0.f ? 0.f : x > xmax ? xmax : x;
            y = y < 0.f ? 0.f : y > ymax ? ymax : y;
            int x0 = (int)x, y0 = (int)y;
            i2[k] = (int16_t)(bilerp16(ink, W, y0 * W + x0, x - x0, y - y0) + 0.5f);
        }
    }
    FLUID_PROF_END(FP_ADVECT, c0); c0 = fluid_cycles();

    /* ---- 6. relax toward the target (what the flow did not deliver) ---- */
    {
        const float hm = 0.4f + 4.6f * p->hold;
        const float up = 1.f - expf(-p->relax_up * hm * sp * dt), down = 1.f - expf(-p->relax_down * hm * sp * dt);
        const int16_t *i2 = s->ink2, *h = s->h;
        int32_t err = 0, hs = 0;
        for (int k = 0; k < N; k++) {
            if (!mask[k]) { ink[k] = 0; continue; }
            float d = i2[k], tk = T[k];
            d += (tk - d) * (tk > d ? up : down);
            int v = (int)(d + 0.5f);
            v = v < 0 ? 0 : v > (int)FQ_UNIT ? (int)FQ_UNIT : v;
            ink[k] = (int16_t)v;
            err += abs(T[k] - v); hs += abs(h[k]);
        }
        s->err = err * INV_UNIT / s->inside;
        float e = hs * INV_H / s->inside * 3.f + vsum / s->inside / 25.f + s->swirl_kick * 0.05f;
        s->energy = e > 1.f ? 1.f : e;
    }
    FLUID_PROF_END(FP_RELAX, c0);
}

/* Colour + accent ride the same flow; once per frame (dt = the frame's stepped time). */
void fluid_sim_color(fluid_sim_t *s, float dt) {
    if (dt <= 0.f) return;
    FLUID_PROF_BEGIN(c0);
    const int W = s->W, H = s->H;
    const flow_k_t fc = flow_consts(s);
    const float xmax = W - 1.001f, ymax = H - 1.001f, dtA = dt / s->A;
    const float tr = 1.f - expf(-s->p.tint_rate * s->p.speed_mul * dt);
    const fluid_rgba16_t *c = s->col;
    fluid_rgba16_t *o = s->col2;
    for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
        const int k = j * W + i;
        if (!s->mask[k]) { o[k] = c[k]; continue; }
        float r, g, b, a;
        if (s->D[k] == 0) {
            r = c[k].r; g = c[k].g; b = c[k].b; a = c[k].a;
        } else {
            float wu, wv;
            flow_cell(s, &fc, i, j, k, &wu, &wv);
            float x = i - wu * dt, y = j - wv * dtA;
            x = x < 0.f ? 0.f : x > xmax ? xmax : x;
            y = y < 0.f ? 0.f : y > ymax ? ymax : y;
            int x0 = (int)x, y0 = (int)y, k0 = y0 * W + x0;
            float fx = x - x0, fy = y - y0;
            float w00 = (1.f - fx) * (1.f - fy), w10 = fx * (1.f - fy), w01 = (1.f - fx) * fy, w11 = fx * fy;
            const fluid_rgba16_t *p0 = &c[k0], *p1 = &c[k0 + 1], *p2 = &c[k0 + W], *p3 = &c[k0 + W + 1];
            r = p0->r * w00 + p1->r * w10 + p2->r * w01 + p3->r * w11;
            g = p0->g * w00 + p1->g * w10 + p2->g * w01 + p3->g * w11;
            b = p0->b * w00 + p1->b * w10 + p2->b * w01 + p3->b * w11;
            a = p0->a * w00 + p1->a * w10 + p2->a * w01 + p3->a * w11;
        }
        uint32_t tc = s->tcol[k];
        const float s8 = 65535.f / 255.f;
        float wt = tr * (0.25f + s->T[k] * INV_UNIT), wa = wt * 1.5f;
        if (wa > 1.f) wa = 1.f;
        r += ((tc & 255) * s8 - r) * wt;
        g += ((tc >> 8 & 255) * s8 - g) * wt;
        b += ((tc >> 16 & 255) * s8 - b) * wt;
        a += ((tc >> 24) * s8 - a) * wa;
        o[k].r = (uint16_t)(r < 0.f ? 0 : r > 65535.f ? 65535 : (int)(r + 0.5f));
        o[k].g = (uint16_t)(g < 0.f ? 0 : g > 65535.f ? 65535 : (int)(g + 0.5f));
        o[k].b = (uint16_t)(b < 0.f ? 0 : b > 65535.f ? 65535 : (int)(b + 0.5f));
        o[k].a = (uint16_t)(a < 0.f ? 0 : a > 65535.f ? 65535 : (int)(a + 0.5f));
    }
    s->col = o; s->col2 = (fluid_rgba16_t *)c;
    FLUID_PROF_END(FP_COLOR, c0);
}

/* What the screen shows: ink refracted by the wave slope, plus a crest highlight. */
void fluid_sim_view(fluid_sim_t *s) {
    const int W = s->W, H = s->H;
    const int16_t *h = s->h, *ink = s->ink;
    const float rk = s->p.refr * 6.f * 0.5f * INV_H, rkA = rk / s->A, crest = s->p.crest * 0.55f;
    const float xmax = W - 1.001f, ymax = H - 1.001f;
    int16_t *view = s->view;
    FLUID_PROF_BEGIN(c0);
    for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
        int k = j * W + i;
        if (!s->mask[k]) { view[k] = 0; continue; }       /* mask excludes the border ring */
        float x = i + (float)(h[k + 1] - h[k - 1]) * rk, y = j + (float)(h[k + W] - h[k - W]) * rkA;
        x = x < 0.f ? 0.f : x > xmax ? xmax : x;
        y = y < 0.f ? 0.f : y > ymax ? ymax : y;
        int x0 = (int)x, y0 = (int)y;
        float sm = bilerp16(ink, W, y0 * W + x0, x - x0, y - y0) * INV_UNIT;
        float hc = h[k] * INV_H;
        float val = sm * (1.f + 0.9f * hc) + crest * (hc > 0.f ? hc : 0.f);
        view[k] = (int16_t)((val < 0.f ? 0.f : val > 1.f ? 1.f : val) * FQ_UNIT + 0.5f);
    }
    FLUID_PROF_END(FP_VIEW, c0);
}

/* ── frame snapshot ──────────────────────────────────────────────────────── */

fluid_frame_t *fluid_frame_create(int W, int H) {
    fluid_frame_t *f = FLUID_CALLOC(sizeof(fluid_frame_t));
    if (!f) return NULL;
    size_t N = (size_t)W * H;
    f->block = FLUID_CALLOC(N * (3 * sizeof(int16_t) + sizeof(fluid_rgba16_t) + 3));
    if (!f->block) { FLUID_FREE(f); return NULL; }
    f->W = W; f->H = H;
    f->col = (fluid_rgba16_t *)f->block;
    f->view = (int16_t *)(f->col + N); f->ink = f->view + N; f->h = f->ink + N;
    f->mask = (uint8_t *)(f->h + N); f->stroke = f->mask + N; f->feat = f->stroke + N;
    return f;
}

void fluid_frame_destroy(fluid_frame_t *f) {
    if (!f) return;
    FLUID_FREE(f->block);
    FLUID_FREE(f);
}

void fluid_sim_snapshot(const fluid_sim_t *s, fluid_frame_t *f, bool strokes) {
    const size_t N = (size_t)s->N;
    memcpy(f->view, s->view, N * sizeof(int16_t));
    memcpy(f->ink, s->ink, N * sizeof(int16_t));
    memcpy(f->h, s->h, N * sizeof(int16_t));
    memcpy(f->col, s->col, N * sizeof(fluid_rgba16_t));
    memcpy(f->mask, s->mask, N);
    /* render.js: feat = edges · edgeW · hold > 0.5 && TF — solid once the shape has settled */
    if (s->edge_w * s->p.hold > 0.5f) memcpy(f->feat, s->tf, N); else memset(f->feat, 0, N);
    if (!strokes) { memset(f->stroke, 0, N); return; }
    /* fast-moving ink shows a streamline stroke along its velocity (render.js) */
    const flow_k_t fc = flow_consts(s);
    const int16_t v015 = (int16_t)(0.15f * FQ_UNIT), v07 = (int16_t)(0.7f * FQ_UNIT);
    for (int j = 0; j < s->H; j++) for (int i = 0; i < s->W; i++) {
        const int k = j * s->W + i;
        uint8_t st = 0;
        if (s->mask[k] && s->view[k] > v015 && s->view[k] < v07) {
            float wu, wv;
            flow_cell(s, &fc, i, j, k, &wu, &wv);
            if (wu * wu + wv * wv > 15.f * 15.f) {
                float a = fluid_atan2(wv, wu);
                if (a < 0.f) a += 3.14159265f;
                st = (uint8_t)(1 + ((int)(a * (4.f / 3.14159265f) + 0.5f) & 3));
            }
        }
        f->stroke[k] = st;
    }
}

static inline void sample_pos(int W, int H, float x, float y, int *k0, float *ax, float *ay) {
    float fx = (x + 1.f) * 0.5f * W - 0.5f, fy = (y + 1.f) * 0.5f * H - 0.5f;
    float xmax = W - 1.001f, ymax = H - 1.001f;
    fx = fx < 0.f ? 0.f : fx > xmax ? xmax : fx;
    fy = fy < 0.f ? 0.f : fy > ymax ? ymax : fy;
    int x0 = (int)fx, y0 = (int)fy;
    *k0 = y0 * W + x0; *ax = fx - x0; *ay = fy - y0;
}

float fluid_frame_sample(const fluid_frame_t *f, const int16_t *arr, float inv_scale, float x, float y) {
    int k0; float ax, ay;
    sample_pos(f->W, f->H, x, y, &k0, &ax, &ay);
    return bilerp16(arr, f->W, k0, ax, ay) * inv_scale;
}

float fluid_frame_sample_acc(const fluid_frame_t *f, float x, float y) {
    int k0; float ax, ay;
    sample_pos(f->W, f->H, x, y, &k0, &ax, &ay);
    const fluid_rgba16_t *c = f->col; int W = f->W;
    float t0 = c[k0].a + ((float)c[k0 + 1].a - c[k0].a) * ax;
    float t1 = c[k0 + W].a + ((float)c[k0 + W + 1].a - c[k0 + W].a) * ax;
    return (t0 + (t1 - t0) * ay) * (1.f / 65535.f);
}

void fluid_sim_centroid(const fluid_sim_t *s, float *cx, float *cy) {
    float sx = 0.f, sy = 0.f, sw = 0.f;
    for (int j = 0; j < s->H; j++) for (int i = 0; i < s->W; i++) {
        float w = s->ink[j * s->W + i] * INV_UNIT;
        sx += s->xs[i] * w; sy += s->ys[j] * w; sw += w;
    }
    if (sw > 1.f) { *cx = sx / sw; *cy = sy / sw; } else { *cx = 0.f; *cy = 0.f; }
}
