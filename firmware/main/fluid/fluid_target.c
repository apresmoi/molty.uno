/*
 * fluid_target.c — what the fluid relaxes toward: blob, effect interludes, speed text.
 *
 * Port of creatures/js/fluid/targets.js (minus the 3D creature target —
 * mole_fluid.c exposes a per-cell target-source hook for that). Each fill writes
 * the sim's T (ink) and tcol (RGBA8 colour + accent); the fluid never shows a
 * target directly — it flows and relaxes toward it.
 */
#include "fluid_sim.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define FLUID_CALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#define FLUID_FREE(p)   heap_caps_free(p)
#else
#define FLUID_CALLOC(n) calloc(1, (n))
#define FLUID_FREE(p)   free(p)
#endif

#define RGB(hex) { ((hex) >> 16 & 255) / 255.f, ((hex) >> 8 & 255) / 255.f, ((hex) & 255) / 255.f }

/* ── blob ───────────────────────────────────────────────────────────────────
 * A breathing metaball: 4 orbiting radial sources summed, iso-surfaced with a soft
 * edge, textured inside by a field-synth patch (radial sin × noise, "Lava"-like). */
static const fluid_patch_t BLOB_PATCH = { 1.2f, 0.55f, 2, {
    { FV_NOISE, FW_SIN, 2.6f, 0.45f, 1.f },
    { FV_RADIAL, FW_SIN, 1.6f, 0.35f, 0.6f },
} };
static const float BLOB_A[3] = RGB(0xffb3c7), BLOB_B[3] = RGB(0xb9a4ff);

void fluid_target_blob(fluid_sim_t *s, float t, float swell, float ox, float oy) {
    FLUID_PROF_BEGIN(c0);
    float C[4][3];
    for (int q = 0; q < 4; q++) {
        float w = 0.35f + q * 0.17f, ph = q * 1.7f, r = (0.19f + 0.025f * q) * swell;
        C[q][0] = 0.2f * cosf(t * w + ph) + ox;
        C[q][1] = 0.17f * sinf(t * w * 1.3f + ph) + oy;
        C[q][2] = r * r;
    }
    for (int j = 0; j < s->H; j++) for (int i = 0; i < s->W; i++) {
        const int k = j * s->W + i;
        if (!s->mask[k]) { s->T[k] = 0; continue; }
        float x = s->xs[i], y = s->ys[j], F = 0.f;
        for (int q = 0; q < 4; q++) {
            float dx = x - C[q][0], dy = y - C[q][1];
            F += C[q][2] / (dx * dx + dy * dy + 1e-4f);
        }
        float edge = fluid_smooth(0.85f, 1.6f, F);
        float tex = edge > 0.f ? fluid_patch(&BLOB_PATCH, x - ox, y - oy, t) : 0.f;
        s->T[k] = fq(edge * (0.5f + 0.5f * tex), FQ_UNIT);
        float m = fluid_clamp01(tex);
        s->tcol[k] = fluid_pack_rgba8(BLOB_A[0] + (BLOB_B[0] - BLOB_A[0]) * m, BLOB_A[1] + (BLOB_B[1] - BLOB_A[1]) * m,
                                      BLOB_A[2] + (BLOB_B[2] - BLOB_A[2]) * m, 0.f);
    }
    FLUID_PROF_END(FP_TARGET, c0);
}

/* ── effect interludes ─────────────────────────────────────────────────────
 * Each is a field-synth patch plus a PHYSICAL action on the fluid, so the interlude
 * is felt in the medium, not painted over it. */
const char *const FLUID_FX_NAMES[FLUID_FX_COUNT] = { "ripple", "vortex", "wave" };

static const fluid_patch_t FX_PATCH[FLUID_FX_COUNT] = {
    { 1.5f, 0.15f, 2, { { FV_RADIAL, FW_SIN, 3.2f, 0.9f, 1.f }, { FV_NOISE, FW_SIN, 2.f, 0.3f, 0.3f } } },
    { 1.3f, 0.25f, 2, { { FV_SPIRAL, FW_SAW, 2.5f, 0.8f, 1.f }, { FV_ANGULAR, FW_SIN, 3.f, -0.5f, 0.7f } } },
    { 1.3f, 0.30f, 3, { { FV_LINY, FW_SIN, 1.6f, 0.6f, 1.f }, { FV_NOISE, FW_SIN, 2.4f, 0.5f, 0.8f },
                        { FV_LINX, FW_SIN, 1.1f, 0.2f, 0.5f } } },
};
static const float FX_COL[FLUID_FX_COUNT][2][3] = {
    { RGB(0x2effb0), RGB(0x7df9ff) },
    { RGB(0xff7a45), RGB(0xffd24a) },
    { RGB(0x7b5cff), RGB(0x2effb0) },
};

void fluid_target_effect(fluid_sim_t *s, int fx, float t, float swell, float ox, float oy) {
    FLUID_PROF_BEGIN(c0);
    if (fx < 0 || fx >= FLUID_FX_COUNT) fx = 0;
    const fluid_patch_t *p = &FX_PATCH[fx];
    const float *cA = FX_COL[fx][0], *cB = FX_COL[fx][1];
    float inv = 1.f / swell;
    for (int j = 0; j < s->H; j++) for (int i = 0; i < s->W; i++) {
        const int k = j * s->W + i;
        if (!s->mask[k]) { s->T[k] = 0; continue; }
        float x = (s->xs[i] - ox) * inv, y = (s->ys[j] - oy) * inv, rr = x * x + y * y;
        float fade = 1.f - fluid_smooth(0.55f, 1.0f, rr);
        float v = fade > 0.f ? fluid_patch(p, x, y, t) : 0.f;
        s->T[k] = fq(v * fade * 0.9f, FQ_UNIT);
        s->tcol[k] = fluid_pack_rgba8(cA[0] + (cB[0] - cA[0]) * v, cA[1] + (cB[1] - cA[1]) * v, cA[2] + (cB[2] - cA[2]) * v, 0.f);
    }
    FLUID_PROF_END(FP_TARGET, c0);
}

void fluid_effect_enter(fluid_sim_t *s, int fx) {
    switch (fx) {
    case FLUID_FX_RIPPLE: fluid_sim_impulse(s, 0.f, 0.f, 2.2f, 0.16f); s->swirl_kick = 2.f; break;
    case FLUID_FX_VORTEX: s->swirl_kick = 4.f; break;
    case FLUID_FX_WAVE:   s->sheet.amp = 1.2f; s->sheet.freq = 1.3f; break;
    default: break;
    }
}

void fluid_effect_tick(fluid_sim_t *s, int fx, float dt, float *st) {
    if (fx == FLUID_FX_RIPPLE) {
        *st += dt;
        if (*st > 0.55f) { *st = 0.f; fluid_sim_impulse(s, 0.f, 0.f, 1.4f, 0.12f); }
    } else if (fx == FLUID_FX_VORTEX) {
        *st += dt * 0.9f; if (*st > 1.f) *st = 1.f;
        s->rotate = 1.6f * *st;
    }
}

void fluid_effect_exit(fluid_sim_t *s, int fx) {
    if (fx == FLUID_FX_VORTEX) s->rotate = 0.f;
    if (fx == FLUID_FX_WAVE) s->sheet.amp = 0.f;
}

/* ── speed text ('plain' look) ──────────────────────────────────────────────
 * The ink pools under each letter; real letters condense out of it (drawn by the
 * renderer over a dark box once enough ink arrived) and melt back on word change.
 * The word sits IN the fluid: streak lines stream past and bow around it ('near'),
 * the field is softly darker right at the letters ('dark') for legibility.
 * Pivot-aligned on the ORP letter like mole_reader.c. */

/* separable box blur, edge-normalised (plain loops; runs once per word) */
static void blur(const float *src, float *dst, float *tmp, int W, int H, int rx, int ry) {
    for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
        float sum = 0.f; int n = 0;
        for (int d = -rx; d <= rx; d++) { int x = i + d; if (x >= 0 && x < W) { sum += src[j * W + x]; n++; } }
        tmp[j * W + i] = sum / n;
    }
    for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
        float sum = 0.f; int n = 0;
        for (int d = -ry; d <= ry; d++) { int y = j + d; if (y >= 0 && y < H) { sum += tmp[y * W + i]; n++; } }
        dst[j * W + i] = sum / n;
    }
}

fluid_text_t *fluid_text_create(const fluid_sim_t *s) {
    fluid_text_t *tx = FLUID_CALLOC(sizeof(fluid_text_t));
    if (!tx) return NULL;
    size_t N = (size_t)s->N;
    tx->block = FLUID_CALLOC(6 * N * sizeof(float));
    if (!tx->block) { FLUID_FREE(tx); return NULL; }
    float *f = (float *)tx->block;
    tx->cov = f; tx->orp = f + N; tx->near = f + 2 * N; tx->near_s = f + 3 * N; tx->dark = f + 4 * N; tx->tmp = f + 5 * N;
    tx->stage = s->stage;
    tx->plain_font = 30.f;
    return tx;
}

void fluid_text_destroy(fluid_text_t *tx) {
    if (!tx) return;
    FLUID_FREE(tx->block);
    FLUID_FREE(tx);
}

void fluid_text_set_sentence(fluid_text_t *tx, float ext) {
    if (ext < 1.f) ext = 1.f;
    /* letter advance 0.6 em; budget is the disc's middle chord (330 px of 412). The
     * largest size scales with the stage like the chord; the smallest stays 18 px, the
     * legibility floor of the 5×7 font on any panel. */
    const float k = (float)tx->stage / FLUID_STAGE_REF;
    float chord = 330.f * k, fmax = 40.f * k;
    if (fmax < 18.f) fmax = 18.f;
    float fs = chord / (2.f * ext) / 0.6f;
    tx->plain_font = fs < 18.f ? 18.f : fs > fmax ? fmax : fs;
}

/* 'near' (wide, normalised) shapes the meniscus that wraps the word; 'dark' (tight)
 * is the soft darker falloff right around the letters */
static void text_shape(fluid_text_t *tx, int W, int H, int N) {
    blur(tx->cov, tx->near, tx->tmp, W, H, 9, 4);
    float mx = 1e-6f;
    for (int k = 0; k < N; k++) if (tx->near[k] > mx) mx = tx->near[k];
    for (int k = 0; k < N; k++) { float v = tx->near[k] / mx * 1.15f; tx->near[k] = v > 1.f ? 1.f : v; }
    blur(tx->cov, tx->dark, tx->tmp, W, H, 2, 1);
    for (int k = 0; k < N; k++) { float v = tx->dark[k] * 2.2f; tx->dark[k] = v > 1.f ? 1.f : v; }
}

void fluid_text_set_word(fluid_text_t *tx, const fluid_sim_t *s, const char *w, int len, int orp) {
    const int W = s->W, H = s->H, N = s->N;
    if (len > FLUID_WORD_LETTERS) len = FLUID_WORD_LETTERS;
    if (len < 0) len = 0;
    /* orp < 0: a static label (lone token) -- centred, no accent letter */
    if (orp >= len) orp = len > 0 ? len - 1 : 0;
    const float fs = tx->plain_font, adv = fs * 0.6f, lh = fs * 1.05f;
    const float x0 = s->stage / 2.f - (orp < 0 ? len / 2.f : orp + 0.5f) * adv, ytop = s->stage / 2.f - lh / 2.f;
    tx->nletters = len;
    for (int i = 0; i < len; i++) {
        fluid_letter_t *L = &tx->letters[i];
        L->ch = w[i]; L->acc = (i == orp);
        L->x = x0 + i * adv; L->y = ytop; L->w = adv; L->h = lh; L->fs = fs; L->melt = 0.f;
    }
    const float cw = (float)s->stage / W, ch = (float)s->stage / H;
    for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
        int k = j * W + i;
        float px = (i + 0.5f) * cw, py = (j + 0.5f) * ch;
        float inL = 0.f, isO = 0.f;
        if (py > ytop && py < ytop + lh) {
            int li = (int)floorf((px - x0) / adv);
            if (li >= 0 && li < len && w[li] != ' ') { inL = 1.f; isO = li == orp ? 1.f : 0.f; }
        }
        tx->cov[k] = inL * 0.62f; tx->orp[k] = isO;
    }
    text_shape(tx, W, H, N);
}

/* A question's labels: the word's rules (letter boxes, ink 0.62 under each letter, the
 * meniscus and the dark falloff) for several words, each centred on its own spot. */
void fluid_text_set_labels(fluid_text_t *tx, const fluid_sim_t *s, const fluid_label_t *lb, int n, float fs) {
    const int W = s->W, H = s->H, N = s->N;
    const float adv = fs * 0.6f, lh = fs * 1.05f;
    float x0[8], y0[8];
    int nl = 0, cnt = n < 0 ? 0 : n > 8 ? 8 : n;
    for (int b = 0; b < cnt; b++) {
        int len = lb[b].len < 0 ? 0 : lb[b].len;
        x0[b] = lb[b].cx - len * adv * 0.5f; y0[b] = lb[b].cy - lh * 0.5f;
        for (int i = 0; i < len && nl < FLUID_MAX_LETTERS; i++) {
            fluid_letter_t *L = &tx->letters[nl++];
            L->ch = lb[b].text[i]; L->acc = lb[b].acc;
            L->x = x0[b] + i * adv; L->y = y0[b]; L->w = adv; L->h = lh; L->fs = fs; L->melt = 0.f;
        }
    }
    tx->nletters = nl;
    const float cw = (float)s->stage / W, ch = (float)s->stage / H;
    for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
        int k = j * W + i;
        float px = (i + 0.5f) * cw, py = (j + 0.5f) * ch, inL = 0.f, isA = 0.f;
        for (int b = 0; b < cnt && inL == 0.f; b++) {
            if (!lb[b].shown || py <= y0[b] || py >= y0[b] + lh) continue;
            int li = (int)floorf((px - x0[b]) / adv);
            if (li >= 0 && li < lb[b].len && lb[b].text[li] != ' ') { inL = 1.f; isA = lb[b].acc ? 1.f : 0.f; }
        }
        tx->cov[k] = inL * 0.62f; tx->orp[k] = isA;
    }
    text_shape(tx, W, H, N);
}

void fluid_text_fill(fluid_text_t *tx, fluid_sim_t *s, float t) {
    FLUID_PROF_BEGIN(c0);
    const float *cov = tx->cov;
    for (int j = 0; j < s->H; j++) for (int i = 0; i < s->W; i++) {
        const int k = j * s->W + i;
        if (!s->mask[k]) { s->T[k] = 0; continue; }
        tx->near_s[k] += (tx->near[k] - tx->near_s[k]) * 0.2f;   /* re-shape smoothly between words */
        float nr = tx->near_s[k], dk = tx->dark[k] - cov[k];
        if (dk < 0.f) dk = 0.f;
        float field = 0.f;
        if (nr > 0.002f) {
            float y = s->ys[j];
            float ys = y - 0.2f * nr * (y + 1e-3f >= 0.f ? 1.f : -1.f);
            float n = fluid_noise3((s->xs[i] - t * 0.32f) * 2.2f, ys * 3.2f, t * 0.35f);
            float streak = 0.5f + 0.5f * fluid_sin_turns((ys * 30.f + n * 5.f) * (1.f / 6.2831853f));
            field = powf(nr, 1.3f) * (0.14f + 0.4f * streak * (0.6f + 0.4f * n)) * (1.f - 0.5f * dk);
        }
        s->T[k] = fq(cov[k] > 0.f ? cov[k] : field, FQ_UNIT);
        s->bgm[k] = (uint8_t)((1.f - 0.5f * dk) * 255.f + 0.5f);
        float wv = cov[k] > 0.f ? 1.f : 0.35f + 0.5f * nr;
        s->tcol[k] = fluid_pack_rgba8(0.6f + 0.35f * wv, 0.68f + 0.28f * wv, 0.92f + 0.08f * wv, tx->orp[k]);
    }
    FLUID_PROF_END(FP_TARGET, c0);
}
