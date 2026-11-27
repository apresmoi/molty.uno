/* SPDX-License-Identifier: MIT, see wave_player.h */
#include "wave_player.h"
#include "rhythm_player.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define WAV_MALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#define WAV_FREE(p)   heap_caps_free(p)
#else
#define WAV_MALLOC(n) malloc(n)
#define WAV_FREE(p)   free(p)
#endif

#define KNEE      0.6f                  /* the beat player's soft knee, of full scale                            */
#define FULL      32000.f
#define MIX       1.25f                 /* the sum times this over the voices                                    */
#define IN_MS     5.f
#define OUT_MS    20.f
#define GLIDE_MS  30.f
#define DUCK_MS   2.f
#define PLUCK_TAU 0.45f                 /* s: a preview's fall                                                   */
#define PLUCK_LEN 1.6f                  /* s: ... then it fades as a held one does                               */
#define STRUM_MS  18.f
#define TWO32     4294967296.f
#define FFT_N     WAV_TAB_N             /* a cycle's samples for its spectrum (the hit is under 7.2 kHz: its harmonics
                                         * end by 294 at the lowest cycle, 1024 points hold them unfolded)        */

/* the level over pitch: log2(f) from -1 (0.5 Hz) to 13 (8192 Hz), 4 points an octave; each pitch to a 0.45 peak or a
 * 0.2 RMS, whichever it reaches first (measured on this player with unit gain, the design's calibrate.mjs) */
static const float GAIN[57] = {
    0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.901f, 0.883f, 0.86f, 0.914f, 0.886f, 0.907f, 0.948f, 0.919f, 0.86f, 0.822f,
    0.851f, 0.909f, 0.984f, 0.784f, 0.884f, 0.985f, 0.853f, 0.926f, 0.887f, 0.742f, 0.906f, 0.901f, 0.773f, 0.71f, 0.745f, 0.898f, 1.14f,
    1.074f, 0.849f, 0.727f, 0.655f, 0.616f, 0.567f, 0.583f, 0.569f, 0.619f, 0.684f, 0.68f, 0.732f, 0.882f, 1.076f, 1.178f, 1.106f, 0.96f,
    0.866f, 0.839f, 0.839f, 0.839f,
};

static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static inline float smooth(float u) { u = clampf(u, 0.f, 1.f); return u * u * (3.f - 2.f * u); }
static inline float frac(float v) { return v - floorf(v); }
static inline float ph_of(uint32_t ph) { return (float)ph * (1.f / TWO32); }

/* ── the hit and the cycles ─────────────────────────────────────────────────────────────────────────────────────── */

static inline float hit_at(const wav_tables_t *t, float s) {
    if (s < 0.f || s >= WAV_HIT_LEN) return 0.f;
    const int i = (int)s;
    return t->hit[i] + (t->hit[i + 1] - t->hit[i]) * (s - (float)i);
}
/* one cycle of the choked hit at rate f, tau samples after its hit (0 <= tau < WAV_RATE / f) */
static float cycle(const wav_tables_t *t, float f, float tau) {
    const float T = WAV_RATE / f, d = fminf(WAV_CHOKE, T * 0.5f);
    return hit_at(t, tau) + (tau < d ? (1.f - tau / d) * hit_at(t, tau + T) : 0.f);
}

/* radix 2, in place; cs / sn: a turn's cos and sin in FFT_N steps (half of them) */
static void fft(float *re, float *im, int n, const float *cs, const float *sn, bool inv) {
    for (int i = 1, j = 0; i < n; i++) {
        int b = n >> 1;
        for (; j & b; b >>= 1) j ^= b;
        j ^= b;
        if (i < j) { float x = re[i]; re[i] = re[j]; re[j] = x; x = im[i]; im[i] = im[j]; im[j] = x; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1, step = FFT_N / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < half; k++) {
                const float wr = cs[k * step], wi = inv ? sn[k * step] : -sn[k * step];
                float *ar = &re[i + k], *ai = &im[i + k], *br = &re[i + k + half], *bi = &im[i + k + half];
                const float vr = *br * wr - *bi * wi, vi = *br * wi + *bi * wr;
                *br = *ar - vr; *bi = *ai - vi; *ar += vr; *ai += vi;
            }
    }
}

wav_tables_t *wav_tables_new(void) {
    wav_tables_t *t = WAV_MALLOC(sizeof(wav_tables_t));   /* (not zeroed: 257 KB of PSRAM; each piece is written before it is read) */
    if (!t) return NULL;
    atomic_store(&t->hit_ready, false); atomic_store(&t->quit, false);
    for (int i = 0; i < 4; i++) { atomic_store(&t->ready[i], 0); atomic_store(&t->want[i], 0); }
    return t;
}
static inline bool has(_Atomic uint32_t *bits, int c) { return atomic_load(&bits[c >> 5]) >> (c & 31) & 1; }

/* the hit: the beat player's low drum and a beater's click (the rim's 1.8 kHz, 2 samples in, gone in a few ms),
 * band-limited under WAV_TOP (a 127-tap Blackman sinc: the drum's breath of noise reached 8 kHz), to a 0.5 peak */
static void make_hit(wav_tables_t *t) {
    enum { L = 127 };
    float *src = t->scratch, *k = src + WAV_HIT_LEN, pk = 0.f;
    const size_t got = rhy_sound(RHY_LOW, src, WAV_HIT_LEN);
    const float sr = WAV_RATE / 1000.f, fc = WAV_TOP / WAV_RATE;
    for (int i = 0; i < WAV_HIT_LEN; i++)
        src[i] = (i < (int)got ? src[i] : 0.f) + 0.95f * sinf(6.2831853f * 1800.f * (float)i / WAV_RATE) * expf(-(float)i / (1.5f * sr)) * fminf(1.f, (float)i / 2.f);
    for (int i = 0; i < L; i++) {
        const float m = (float)i - (L - 1) * 0.5f, win = 0.42f - 0.5f * cosf(6.2831853f * i / (L - 1)) + 0.08f * cosf(12.566371f * i / (L - 1));
        k[i] = (m == 0.f ? 2.f * fc : sinf(6.2831853f * fc * m) / (3.1415927f * m)) * win;
    }
    for (int i = 0; i < WAV_HIT_LEN; i++) {
        float y = 0.f;
        for (int q = 0; q < L; q++) { const int j = i - q + (L - 1) / 2; if (j >= 0 && j < WAV_HIT_LEN) y += k[q] * src[j]; }
        t->hit[i] = y; pk = fmaxf(pk, fabsf(y));
    }
    for (int i = 0; i < WAV_HIT_LEN; i++) t->hit[i] *= 0.5f / pk;
    t->hit[WAV_HIT_LEN] = t->hit[WAV_HIT_LEN + 1] = 0.f;
    t->hit_peak = 0.5f;
    atomic_store(&t->hit_ready, true);
}
/* cycle c: the choked hit at its semitone's rate, its harmonics under WAV_TOP (by the next semitone's, so the blend
 * between two never reaches over it) */
static void make_cycle(wav_tables_t *t, int c) {
    float *re = t->scratch, *im = re + FFT_N, *cs = im + FFT_N, *sn = cs + FFT_N / 2;
    for (int i = 0; i < FFT_N / 2; i++) { cs[i] = cosf(6.2831853f * i / FFT_N); sn[i] = sinf(6.2831853f * i / FFT_N); }
    const float f = wav_hz((float)(WAV_TAB_LO + c)), T = WAV_RATE / f;
    const int K = (int)fminf(WAV_TAB_N / 2 - 1, floorf(WAV_TOP / wav_hz((float)(WAV_TAB_LO + c + 1))));
    for (int i = 0; i < FFT_N; i++) { re[i] = cycle(t, f, (float)i / FFT_N * T); im[i] = 0.f; }
    fft(re, im, FFT_N, cs, sn, false);
    for (int h = K + 1; h <= FFT_N - K - 1; h++) re[h] = im[h] = 0.f;
    fft(re, im, FFT_N, cs, sn, true);
    float p = 0.f;
    for (int i = 0; i < WAV_TAB_N; i++) p = fmaxf(p, fabsf(re[i]));
    p = p > 0.f ? p / FFT_N : 1.f;
    const float to = 32767.f / (p * FFT_N);
    for (int i = 0; i < WAV_TAB_N; i++) t->tab[c][i] = (int16_t)lrintf(re[i] * to);
    t->tab[c][WAV_TAB_N] = t->tab[c][0];
    t->unit[c] = p / 32767.f; t->peak[c] = p;
    atomic_fetch_or(&t->ready[c >> 5], 1u << (c & 31));
}
bool wav_tables_step(wav_tables_t *t) {
    if (atomic_load(&t->quit)) return false;
    if (!atomic_load(&t->hit_ready)) { make_hit(t); return true; }
    int next = -1;
    for (int c = 0; c < WAV_TABS && next < 0; c++) if (has(t->want, c) && !has(t->ready, c)) next = c;
    for (int c = 0; c < WAV_TABS && next < 0; c++) if (!has(t->ready, c)) next = c;
    if (next < 0) return false;
    make_cycle(t, next);
    return true;
}
bool wav_tables_made(wav_tables_t *t) {
    if (!atomic_load(&t->hit_ready)) return false;
    for (int c = 0; c < WAV_TABS; c++) if (!has(t->ready, c)) return false;
    return true;
}
void wav_tables_free(wav_tables_t *t) { WAV_FREE(t); }
/* cycles m and m + 1 made (blended between); else asked for first */
static bool cycles(wav_tables_t *t, int m) {
    const int m1 = m + 1 < WAV_TABS ? m + 1 : m;
    if (has(t->ready, m) && has(t->ready, m1)) return true;
    atomic_fetch_or(&t->want[m >> 5], 1u << (m & 31)); atomic_fetch_or(&t->want[m1 >> 5], 1u << (m1 & 31));
    return false;
}

/* where rate f reads the cycles: the semitone under it (*m) and how far to the next (*u) */
static inline void table_pos(float f, int *m, float *u) {
    const float x = clampf(69.f + 12.f * log2f(f / 440.f) - WAV_TAB_LO, 0.f, WAV_TABS - 1 - 1e-4f);
    *m = (int)x; *u = x - (float)*m;
}
static inline float table_weight(float f) { return smooth((log2f(f) - log2f(WAV_DIRECT_HI)) / (log2f(WAV_TABLE_LO) - log2f(WAV_DIRECT_HI))); }
static inline float table_at(const wav_tables_t *t, int m, float u, float ph, bool norm) {
    const float p = ph * WAV_TAB_N;
    int i = (int)p;
    if (i > WAV_TAB_N - 1) i = WAV_TAB_N - 1;
    const float w = p - (float)i;
    const int m1 = m + 1 < WAV_TABS ? m + 1 : m;
    const int16_t *a = t->tab[m], *b = t->tab[m1];
    const float va = (a[i] + (a[i + 1] - a[i]) * w) * t->unit[m], vb = (b[i] + (b[i + 1] - b[i]) * w) * t->unit[m1];
    return norm ? va / t->peak[m] * (1.f - u) + vb / t->peak[m1] * u : va + (vb - va) * u;
}

/* ── the player ─────────────────────────────────────────────────────────────────────────────────────────────────── */

static float s_in, s_out, s_glide, s_duck, s_pluck;   /* each sample's share of the way (the same for every player) */

void wav_init(wav_player_t *p, wav_tables_t *t) {
    s_in = 1.f - expf(-1.f / (IN_MS / 1000.f * WAV_RATE)); s_out = 1.f - expf(-1.f / (OUT_MS / 1000.f * WAV_RATE));
    s_glide = 1.f - expf(-1.f / (GLIDE_MS / 1000.f * WAV_RATE)); s_duck = 1.f - expf(-1.f / (DUCK_MS / 1000.f * WAV_RATE));
    s_pluck = expf(-1.f / (PLUCK_TAU * WAV_RATE));
    memset(p, 0, sizeof(*p));
    p->t = t;
    p->duck = p->duckT = 1.f;
    p->fc = 1.f; p->root = 256.f;
    p->ch.n = 0;
    for (int i = 0; i < WAV_VOICES; i++) { wav_voice_t *q = &p->v[i]; q->n = 1; q->s = q->sp = 1e9f; q->lf = q->lft = 8.f; q->f = 256.f; q->g = 1.f; q->d = WAV_CHOKE; }
}

static float pitch(const wav_player_t *p, int i, float root) {
    const float f = p->tuning == WAV_JUST ? root * p->ch.count[i] / p->ch.count[0] : root * exp2f(p->ch.semi[i] / 12.f);
    return clampf(f, WAV_MIN_HZ, WAV_MAX_HZ);
}
static float gain_at(float f) {
    const float x = (log2f(f) + 1.f) * 4.f;
    const int i = (int)clampf(floorf(x), 0.f, 55.f);
    const float u = clampf(x - (float)i, 0.f, 1.f);
    return GAIN[i] + (GAIN[i + 1] - GAIN[i]) * u;
}
/* a voice's control rate: its pitch from its glide, and what follows from it (every 16 samples) */
static void control(wav_voice_t *q) {
    q->f = exp2f(q->lf);
    q->dph = (uint32_t)(q->f * (TWO32 / WAV_RATE));
    q->g = gain_at(q->f); q->wt = table_weight(q->f); q->d = fminf(WAV_CHOKE, WAV_RATE / q->f * 0.5f);
    table_pos(q->f, &q->tm, &q->tu);
}
static void clock_of(wav_player_t *p) {
    p->fc = p->v[0].f / (p->ch.n ? p->ch.count[0] : 1);
    p->dH = (uint32_t)(p->fc * (TWO32 / WAV_RATE));
}
static bool ringing(const wav_player_t *p) { for (int i = 0; i < WAV_VOICES; i++) if (p->v[i].env > 1e-3f) return true; return false; }
static void restart(wav_voice_t *q) { q->ph = q->lastph = 0; q->s = 0.f; q->sp = 1e9f; q->pg = 0.f; }

static void apply(wav_player_t *p, const wav_chord_t *ch, float root, int mode, int tuning, bool fresh) {
    p->tuning = (uint8_t)tuning; p->ch = *ch; p->root = clampf(root, WAV_MIN_HZ, WAV_MAX_HZ);
    const bool again = fresh || mode == WAV_PLUCK;
    if (again) p->H = 0;
    for (int i = 0; i < WAV_VOICES; i++) {
        wav_voice_t *q = &p->v[i];
        if (i >= ch->n) { q->envT = 0.f; q->gate = false; if (again) { q->env = 0.f; q->mode = WAV_HOLD; } continue; }
        q->n = ch->count[i]; q->semi = ch->semi[i];
        q->lft = log2f(pitch(p, i, p->root));
        if (again || q->env < 1e-4f) { q->lf = q->lft; control(q); }
        if (again) { restart(q); q->gate = true; }
        q->mode = (uint8_t)mode;
        if (mode == WAV_PLUCK) { q->wait = (int)lrintf(i * STRUM_MS * WAV_RATE / 1000.f); q->t = 0; q->env = q->envT = 0.f; }
        else q->envT = q->mute ? 0.f : 1.f;
    }
    p->hold = mode == WAV_HOLD;
    clock_of(p);
}
static void retune(wav_player_t *p, int tuning) {
    p->tuning = (uint8_t)tuning;
    for (int i = 0; i < p->ch.n; i++) {
        wav_voice_t *q = &p->v[i];
        q->ph = q->lastph = q->n * p->H;
        q->lf = q->lft = log2f(pitch(p, i, p->root));
        control(q);
    }
    clock_of(p);
}
/* fn now if nothing rings, else once the sum has ducked to silence */
static void at_silence(wav_player_t *p, const wav_chord_t *ch, float root, int mode, int tuning, bool fresh, bool re) {
    if (!ringing(p)) { if (re) retune(p, tuning); else apply(p, ch, root, mode, tuning, fresh); return; }
    p->later.on = true; p->later.retune = re; p->later.root = root; p->later.mode = (uint8_t)mode; p->later.tuning = (uint8_t)tuning; p->later.fresh = fresh;
    if (ch) p->later.ch = *ch;
    p->duckT = 0.f;
}

void wav_chord(wav_player_t *p, const wav_chord_t *c, float root, int mode, int tuning, bool fresh) {
    wav_chord_t ch = *c;
    if (ch.n < 1) return;
    if (ch.n > WAV_VOICES) ch.n = WAV_VOICES;
    if (fresh || mode == WAV_PLUCK) at_silence(p, &ch, root, mode, tuning, fresh, false);
    else apply(p, &ch, root, mode, tuning, false);
}
void wav_set_root(wav_player_t *p, float root) {
    p->root = clampf(root, WAV_MIN_HZ, WAV_MAX_HZ);
    if (p->later.on && !p->later.retune) p->later.root = p->root;   /* (a chord waiting for silence starts there) */
    for (int i = 0; i < p->ch.n; i++) p->v[i].lft = log2f(pitch(p, i, p->root));
}
void wav_set_mute(wav_player_t *p, uint32_t mask) {
    for (int i = 0; i < WAV_VOICES; i++) {
        wav_voice_t *q = &p->v[i];
        q->mute = mask >> i & 1;
        if (q->mode == WAV_HOLD && i < p->ch.n) q->envT = q->mute ? 0.f : 1.f;
    }
}
void wav_set_tuning(wav_player_t *p, int tuning) {
    if (tuning == p->tuning) return;
    at_silence(p, NULL, p->root, WAV_HOLD, tuning, false, true);
}
void wav_silence(wav_player_t *p) {
    p->later.on = false; p->hold = false;
    p->duckT = p->yield ? 0.f : 1.f;
    for (int i = 0; i < WAV_VOICES; i++) {
        wav_voice_t *q = &p->v[i];
        q->envT = 0.f;
        if (q->mode == WAV_PLUCK && q->t < (int)(PLUCK_LEN * WAV_RATE)) q->t = (int)(PLUCK_LEN * WAV_RATE);
        q->wait = 0; q->gate = false;
    }
}
void wav_yield(wav_player_t *p, bool yield) {
    p->yield = yield;
    p->duckT = yield || p->later.on ? 0.f : 1.f;
}
bool wav_active(const wav_player_t *p) {
    if (!p->t) return false;
    if (p->hold || p->later.on) return true;
    for (int i = 0; i < WAV_VOICES; i++) if (p->v[i].env > 1e-5f || p->v[i].gate || (p->v[i].mode == WAV_PLUCK && p->v[i].wait > 0)) return true;
    return false;
}

static void snapshot(wav_player_t *p) {
    wav_snap_t *s = &p->snap[(p->n / WAV_VIEW_EVERY) % WAV_VIEWS];
    s->n = p->n; s->H = p->H; s->fc = p->fc;
    for (int i = 0; i < WAV_VOICES; i++) { s->ph[i] = p->v[i].ph; s->f[i] = p->v[i].f; s->env[i] = p->v[i].env; }
    if (p->nsnap < WAV_VIEWS) p->nsnap++;
}

size_t wav_fill(wav_player_t *p, int16_t *out, size_t n) {
    if (!wav_active(p) || (p->yield && p->duck < 0.003f && !p->later.on)) { memset(out, 0, n * sizeof(int16_t)); return 0; }
    wav_tables_t *t = p->t;
    if (!atomic_load(&t->hit_ready)) { memset(out, 0, n * sizeof(int16_t)); return n; }   /* (its first ms: the hit is being made) */
    const int in_n = (int)(IN_MS * WAV_RATE / 1000.f), pluck_n = (int)(PLUCK_LEN * WAV_RATE);
    for (size_t i = 0; i < n; i++) {
        if (p->n % WAV_VIEW_EVERY == 0) snapshot(p);
        p->duck += (p->duckT - p->duck) * s_duck;
        if (p->later.on && p->duck < 0.003f) {
            p->later.on = false; p->duck = 0.f;
            if (p->later.retune) retune(p, p->later.tuning);
            else apply(p, &p->later.ch, p->later.root, p->later.mode, p->later.tuning, p->later.fresh);
            p->duckT = p->yield ? 0.f : 1.f;
        }
        const bool ctl = (p->n & 15) == 0;
        float sum = 0.f;
        bool any = p->hold || p->later.on;
        for (int k = 0; k < WAV_VOICES; k++) {
            wav_voice_t *q = &p->v[k];
            q->lf += (q->lft - q->lf) * s_glide;
            if (ctl) {
                control(q);
                if (!k) clock_of(p);
                const bool made = q->wt <= 0.f || cycles(t, q->tm);
                const float want = made ? q->wt : 0.f;   /* (its cycles not made yet, gliding there: the hit itself) */
                if (q->gate && made) { q->gate = false; q->tw = want; }   /* (a start waits for them: it begins on them) */
                q->tw += (want - q->tw) * 0.25f;
                if (fabsf(want - q->tw) < 0.004f) q->tw = want;
            }
            if (q->gate) { any = true; continue; }
            if (q->mode == WAV_PLUCK) {
                if (q->wait > 0) { q->wait--; any = true; continue; }
                q->t++;
                q->env = q->t <= in_n ? (float)q->t / in_n : q->t < pluck_n ? q->env * s_pluck : q->env * (1.f - s_out);
            } else q->env += (q->envT - q->env) * (q->envT > q->env ? s_in : s_out);
            any |= q->env > 1e-5f;
            /* the phase, and a hit each time it wraps */
            if (p->tuning == WAV_JUST && q->mode == WAV_HOLD) q->ph = q->n * (p->H + p->dH);
            else q->ph += q->dph;
            if (q->ph < q->lastph) { q->sp = q->s; q->pg = 1.f; q->s = ph_of(q->ph) * WAV_RATE / q->f; }
            else q->s += 1.f;
            q->lastph = q->ph;
            if (q->env < 1e-6f) continue;
            float y = 0.f;
            if (q->tw < 1.f) y = hit_at(t, q->s) + (q->pg > 0.f ? q->pg * hit_at(t, q->sp) : 0.f);
            if (q->pg > 0.f) { q->sp += 1.f; q->pg -= 1.f / q->d; }
            if (q->tw > 0.f) y = y * (1.f - q->tw) + table_at(t, q->tm, q->tu, ph_of(q->ph), false) * q->tw;
            sum += y * q->g * q->env;
        }
        p->H += p->dH;
        float o = sum * p->duck * fminf(1.f, MIX / (float)(p->ch.n > 1 ? p->ch.n : 1));
        const float a = fabsf(o);
        if (a > KNEE) o = copysignf(KNEE + (1.f - KNEE) * tanhf((a - KNEE) / (1.f - KNEE)), o);
        out[i] = (int16_t)lrintf(o * FULL);
        p->n++;
        if (!any || (p->yield && p->duck < 0.003f && !p->later.on)) {   /* faded out, or ducked for a job: silence from here */
            memset(out + i + 1, 0, (n - i - 1) * sizeof(int16_t));
            return i + 1;
        }
    }
    return n;
}

int wav_top_midi(const wav_chord_t *ch, int tuning) {
    if (ch->n < 1) return 113;
    const float up = tuning == WAV_JUST ? (float)ch->count[ch->n - 1] / ch->count[0] : exp2f(ch->semi[ch->n - 1] / 12.f);
    return (int)floorf(69.f + 12.f * log2f(WAV_MAX_HZ / up / 440.f));
}

/* ── the picture ────────────────────────────────────────────────────────────────────────────────────────────────── */

void wav_view(const wav_player_t *p, uint64_t n, wav_view_t *o) {
    memset(o, 0, sizeof(*o));
    o->hold = p->hold; o->nv = p->ch.n; o->just = p->tuning == WAV_JUST;
    for (int i = 0; i < WAV_VOICES; i++) { o->n[i] = p->v[i].n; o->mute |= (uint32_t)p->v[i].mute << i; o->pluck[i] = p->v[i].mode == WAV_PLUCK; }
    if (!p->t) return;
    o->on = true;
    /* the newest snapshot at or before n; what is made now when n is past it (the speaker plays it all) */
    wav_snap_t now = { p->n, p->H, p->fc, { 0 }, { 0 }, { 0 } };
    for (int i = 0; i < WAV_VOICES; i++) { now.ph[i] = p->v[i].ph; now.f[i] = p->v[i].f; now.env[i] = p->v[i].env; }
    const wav_snap_t *s = &now;
    if (n < p->n) {
        const wav_snap_t *old = NULL;
        for (int k = 0; k < p->nsnap; k++) {
            const wav_snap_t *q = &p->snap[k];
            if (q->n <= n && (s == &now || q->n > s->n)) s = q;
            if (!old || q->n < old->n) old = q;
        }
        if (s == &now && old) s = old;               /* (older than all: the oldest) */
    }
    const float dt = n > s->n ? fminf(0.1f, (float)(n - s->n) / WAV_RATE) : 0.f;
    o->fc = s->fc;
    o->H = frac(ph_of(s->H) + s->fc * dt);
    for (int i = 0; i < WAV_VOICES; i++) {
        o->f[i] = s->f[i]; o->env[i] = s->env[i]; o->to[i] = exp2f(p->v[i].lft);
        o->ph[i] = p->tuning == WAV_JUST && p->v[i].mode == WAV_HOLD && i < p->ch.n ? frac(p->v[i].n * o->H) : frac(ph_of(s->ph[i]) + s->f[i] * dt);
    }
}

void wav_trace(wav_tables_t *t, const wav_view_t *v, float held, float rc, float amp, float persist, float *out, int npts) {
    struct { float e, k, f, wt, u, T, p0; int m, n; } L[WAV_VOICES];
    const bool playing = v->hold;
    int nl = 0;
    float wts = 0.f;
    const bool hit = t && atomic_load(&t->hit_ready);
    for (int j = 0; hit && j < v->nv && j < WAV_VOICES; j++) {
        const bool live = playing ? !(v->mute >> j & 1) : v->pluck[j] && v->env[j] > 1e-3f;
        if (playing && live) wts += 1.f;
        if (!live) continue;
        const float f = v->to[j] > 0.f ? v->to[j] : v->f[j] > 0.f ? v->f[j] : 262.f;
        /* (whole numbers: voice j turns n_j times the clock, exactly, and its outline stands still) */
        L[nl].e = v->env[j]; L[nl].f = f; L[nl].k = v->just ? (float)v->n[j] : v->f[j] / fmaxf(v->fc, 1e-6f); L[nl].n = v->n[j]; L[nl].p0 = v->ph[j];
        table_pos(f, &L[nl].m, &L[nl].u);
        L[nl].wt = table_weight(f) > 0.f && cycles(t, L[nl].m) ? table_weight(f) : 0.f;
        L[nl].T = WAV_RATE / f;
        nl++;
    }
    if (!playing) wts = (float)(v->nv > 1 ? v->nv : 1);
    const float reach = 1.f, fc = fmaxf(v->fc, 1e-6f), h = v->H;   /* (the whole reach at every speed: a slow hit plainly bounces on the glass) */
    for (int q = 0; q < npts; q++) {
        const float x = (float)q / npts, turns = playing ? frac(h - x) : 0.f;
        const float keep = playing ? reach * (held < 1.f ? expf(-turns / fc / persist) * (1.f - held) + held : 1.f) : 1.f;
        float s = 0.f;
        for (int j = 0; j < nl; j++) {
            const float ph = playing ? frac(L[j].p0 - L[j].k * turns) : frac(L[j].n * x);
            float y = L[j].wt < 1.f ? cycle(t, L[j].f, ph * L[j].T) / t->hit_peak : 0.f;
            if (L[j].wt > 0.f) y = y * (1.f - L[j].wt) + table_at(t, L[j].m, L[j].u, ph, true) * L[j].wt;
            s += L[j].e * y;
        }
        out[q] = rc + amp * keep * (wts > 0.f ? s / wts : 0.f);
    }
    out[npts] = out[0];
}
