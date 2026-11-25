/* SPDX-License-Identifier: MIT — see rhythm_player.h */
#include "rhythm_player.h"
#include <math.h>
#include <string.h>

const char *const RHY_SOUND_NAMES[RHY_SOUNDS] = { "low", "head", "rim", "clap", "bell", "hat", "snare", "click" };

/* ── the drums ──────────────────────────────────────────────────────────────────────────────────────────────────────
 * A tone (a sine and one partial, its pitch falling from f0 to f1 with time constant sweep) under a decay tau, plus
 * noise (low-passed by lp, high-passed by hp, its own decay ntau), for len. The clap's noise comes three times, 11 ms
 * apart, then rings on. gain: each sound's level against the others (an unaccented hit plays at RHY_SOFT of it).
 * Tuned by ear on a computer's speaker, at 16 kHz: on Molty's small speaker the low drum is mostly its partial. */
typedef struct { float f0, f1, sweep_ms, tau_ms, len_ms, p2, l2, noise, ntau_ms, lp, hp, gain; } snd_t;
static const snd_t SND[RHY_SOUNDS] = {
    /* low   */ { 150.f,  55.f, 40.f, 160.f, 450.f, 2.0f,   0.35f, 0.25f,  4.f, 0.50f, 0.00f, 0.95f },
    /* head  */ { 220.f, 110.f, 30.f,  90.f, 300.f, 1.5f,   0.25f, 0.45f, 12.f, 0.35f, 0.00f, 0.90f },
    /* rim   */ { 1800.f, 1800.f, 1.f, 22.f,  90.f, 2.7f,   0.50f, 0.50f,  4.f, 0.80f, 0.60f, 0.70f },
    /* clap  */ {   0.f,   0.f,  1.f,   1.f, 180.f, 0.0f,   0.00f, 1.60f,  5.f, 0.45f, 0.75f, 0.80f },
    /* bell  */ { 830.f, 830.f,  1.f, 260.f, 700.f, 1.646f, 0.60f, 0.00f,  1.f, 0.00f, 0.00f, 0.55f },
    /* hat   */ {   0.f,   0.f,  1.f,   1.f,  70.f, 0.0f,   0.00f, 1.60f, 18.f, 0.90f, 0.85f, 0.50f },
    /* snare */ { 190.f, 160.f, 30.f,  60.f, 220.f, 0.0f,   0.00f, 1.00f, 70.f, 0.60f, 0.50f, 0.75f },
    /* click */ { 1500.f, 1500.f, 1.f, 12.f,  50.f, 0.0f,   0.00f, 0.00f,  1.f, 0.00f, 0.00f, 0.60f },
};
#define MASTER   0.34f                  /* one accented hit's peak, about -9 dBFS                                */
#define RHY_SOFT 0.6f                   /* an unaccented hit                                                     */
#define KNEE     0.6f                   /* the soft limiter starts here (of full scale)                          */
#define FULL     32000.f                /* full scale, a little under 32767                                      */
#define ATTACK   16                     /* samples: 1 ms in                                                      */
#define RELEASE  80                     /* samples: 5 ms out at the end of a voice                               */
#define CLAP_GAP 176                    /* samples: 11 ms between the clap's bursts                              */

/* sine by table (one turn = 1.0): no sinf a sample */
#define SINE_N 256
static float s_sine[SINE_N + 1];
static void sine_init(void) {
    if (s_sine[SINE_N / 4] != 0.f) return;
    for (int i = 0; i <= SINE_N; i++) s_sine[i] = sinf(6.28318531f * (float)i / SINE_N);
}
static inline float sine(float ph) {
    const float x = ph * SINE_N;
    const int i = (int)x;
    return s_sine[i] + (s_sine[i + 1] - s_sine[i]) * (x - (float)i);
}
static inline float noise(rhy_player_t *p) {
    uint32_t x = p->noise;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    p->noise = x;
    return (float)(int32_t)x * (1.f / 2147483648.f);
}

static void voice_start(rhy_player_t *p, int sound, int layer, bool accent) {
    int k = 0;
    for (int i = 0; i < RHY_VOICES; i++) {
        if (!p->v[i].len) { k = i; break; }
        if (p->v[i].t > p->v[k].t) k = i;     /* none free: the oldest (by now the quietest) gives way */
    }
    const snd_t *s = &SND[sound];
    rhy_voice_t *v = &p->v[k];
    const float sr = (float)RHY_RATE / 1000.f, up = sound == RHY_CLICK && accent ? 1.333f : 1.f;
    *v = (rhy_voice_t){ .sound = (uint8_t)sound, .layer = (uint8_t)(layer < 0 ? 0xff : layer), .len = (uint32_t)(s->len_ms * sr),
                        .gain = s->gain * MASTER * (accent ? 1.f : RHY_SOFT), .fade = 1.f,
                        .f = s->f0 * up, .f1 = s->f1 * up, .ksw = expf(-1.f / (s->sweep_ms * sr)),
                        .env = 1.f, .kenv = expf(-1.f / (s->tau_ms * sr)), .nenv = 1.f, .knenv = expf(-1.f / (s->ntau_ms * sr)) };
}

static float voice_sample(rhy_player_t *p, rhy_voice_t *v) {
    const snd_t *s = &SND[v->sound];
    float x = 0.f;
    if (s->f0 > 0.f) {
        x = (sine(v->ph) + s->l2 * sine(v->ph2)) * v->env;
        v->env *= v->kenv;
        v->ph += v->f * (1.f / RHY_RATE);
        v->ph2 += v->f * s->p2 * (1.f / RHY_RATE);
        v->ph -= floorf(v->ph); v->ph2 -= floorf(v->ph2);
        v->f = v->f1 + (v->f - v->f1) * v->ksw;
    }
    if (s->noise > 0.f) {
        v->lp += s->lp * (noise(p) - v->lp);
        float y = v->lp;
        if (s->hp > 0.f) { const float h = s->hp * (v->hp + y - v->hpx); v->hpx = y; v->hp = h; y = h; }
        x += s->noise * y * v->nenv;
        v->nenv *= v->knenv;
        if (v->sound == RHY_CLAP && (v->t == CLAP_GAP || v->t == 2 * CLAP_GAP)) {
            v->nenv += 1.f;
            if (v->t == 2 * CLAP_GAP) v->knenv = expf(-1.f / (40.f * RHY_RATE / 1000.f));   /* then it rings on */
        }
    }
    if (v->t < ATTACK) x *= (float)v->t / ATTACK;
    if (v->len - v->t < RELEASE) x *= (float)(v->len - v->t) / RELEASE;
    x *= v->gain;
    if (v->fading) { x *= v->fade > 0.f ? v->fade : 0.f; v->fade -= 1.f / RHY_FADE; }
    if (++v->t >= v->len || (v->fading && v->fade <= 0.f)) v->len = 0;
    return x;
}

static void fade_voices(rhy_player_t *p, uint32_t layers, bool clicks) {
    for (int i = 0; i < RHY_VOICES; i++) {
        rhy_voice_t *v = &p->v[i];
        if (v->len && ((v->layer == 0xff && clicks) || (v->layer != 0xff && (layers >> v->layer & 1)))) v->fading = true;
    }
}

/* ── the pattern ─────────────────────────────────────────────────────────────────────────────────────────────────── */

double rhy_onset(const rhy_pattern_t *pt, int s) {
    double o = pt->start[s];
    if (pt->swing > 0.f && (s & 1)) o += (double)pt->swing * ((double)pt->start[s] - pt->start[s - 1]);
    return o;
}
uint32_t rhy_bar_hits(const rhy_pattern_t *pt, int slot, int l) {
    if (!pt->nbars || !pt->norder) return pt->layer[l].hits;
    const int i = ((slot % pt->norder) + pt->norder) % pt->norder;
    return pt->bar[pt->order[i]][l];
}
static inline const rhy_pattern_t *cur(const rhy_player_t *p) { return &p->lib[p->cur]; }
static inline double bar_len(const rhy_player_t *p) { return cur(p)->start[cur(p)->steps]; }
static inline int first_beat(const rhy_pattern_t *pt) { for (int s = 0; s < pt->steps; s++) if (pt->beats >> s & 1) return s; return 0; }

/* ── the clock ───────────────────────────────────────────────────────────────────────────────────────────────────── */

static double du_of(const rhy_player_t *p) { return (double)p->bpm * cur(p)->per / 60.0 / RHY_RATE; }
static void anchor(rhy_player_t *p, double u0) {
    memmove(&p->anc[1], &p->anc[0], sizeof(p->anc[0]) * (RHY_ANCHORS - 1));
    p->anc[0].n0 = p->n; p->anc[0].u0 = u0; p->anc[0].du = du_of(p);
}
double rhy_u_at(const rhy_player_t *p, uint64_t n) {
    int i = 0;
    while (i < RHY_ANCHORS - 1 && p->anc[i].n0 > n && p->anc[i + 1].du > 0.0) i++;
    return p->anc[i].u0 + ((double)n - (double)p->anc[i].n0) * p->anc[i].du;
}
/* the count-in, then bar k from its start */
static void restart(rhy_player_t *p, int64_t k) {
    const double L = bar_len(p);
    const int cin = cur(p)->count_in;
    p->cin_end = (double)k * L;
    anchor(p, (double)(k - cin) * L);
    p->ev_bar = k - cin; p->ev_step = 0;
}
static inline double ev_u(const rhy_player_t *p) { return (double)p->ev_bar * bar_len(p) + rhy_onset(cur(p), p->ev_step); }

/* step (ev_bar, ev_step) sounds now, at sample p->n */
static void place(rhy_player_t *p) {
    const rhy_pattern_t *pt = cur(p);
    const int s = p->ev_step;
    const bool beat = pt->beats >> s & 1, first = s == first_beat(pt);
    if (ev_u(p) < p->cin_end - 1e-9) {
        if (beat) { voice_start(p, RHY_CLICK, -1, first); if (p->hit_fn) p->hit_fn(p->hit_ctx, p->n, -1, s, first); }
    } else {
        if (s == 0 && p->next_req) { p->shift++; p->next_req = false; }
        const int slot = (int)(p->ev_bar + p->shift);
        for (int l = 0; l < pt->nlayers; l++) {
            if (!(rhy_bar_hits(pt, slot, l) >> s & 1) || (p->mute >> l & 1)) continue;
            const bool acc = pt->layer[l].accent >> s & 1;
            voice_start(p, pt->layer[l].sound, l, acc);
            if (p->hit_fn) p->hit_fn(p->hit_ctx, p->n, l, s, acc);
        }
        if (beat && p->nbeat < RHY_BEATS) { p->beat_n[p->nbeat] = p->n; p->beat_first[p->nbeat++] = first; }
    }
    if (++p->ev_step >= pt->steps) { p->ev_step = 0; p->ev_bar++; }
}

/* ── the calls ───────────────────────────────────────────────────────────────────────────────────────────────────── */

void rhy_init(rhy_player_t *p) {
    sine_init();
    memset(p, 0, sizeof(*p));
    p->noise = 0x9e3779b9u;
    p->bpm = 100.f;
}

static float clamp_bpm(float bpm) { return !(bpm >= RHY_BPM_MIN) ? RHY_BPM_MIN : bpm > RHY_BPM_MAX ? RHY_BPM_MAX : bpm; }

bool rhy_load(rhy_player_t *p, const rhy_pattern_t *lib, int n, int pick, float bpm, uint32_t mute, bool paused) {
    if (n < 1 || n > RHY_LIB || pick < 0 || pick >= n) return false;
    memcpy(p->lib, lib, sizeof(rhy_pattern_t) * (size_t)n);
    p->nlib = n; p->cur = pick; p->bpm = clamp_bpm(bpm); p->mute = mute; p->shift = 0; p->next_req = false;
    p->on = true; p->paused = paused; p->stopping = false; p->quit = false; p->nbeat = 0;
    fade_voices(p, ~0u, true);
    restart(p, 0);
    p->ver++;
    return true;
}

void rhy_set_bpm(rhy_player_t *p, float bpm) {
    const double u = rhy_u_at(p, p->n);
    p->bpm = clamp_bpm(bpm);
    anchor(p, u);
}

bool rhy_pick(rhy_player_t *p, int i) {
    if (!p->on || i < 0 || i >= p->nlib) return false;
    fade_voices(p, ~0u, true);
    p->cur = i; p->shift = 0; p->next_req = false; p->nbeat = 0;
    p->mute &= (1u << cur(p)->nlayers) - 1u;
    restart(p, 0);
    p->ver++;
    return true;
}

void rhy_set_mute(rhy_player_t *p, uint32_t mute) {
    fade_voices(p, mute & ~p->mute, false);
    p->mute = mute;
    p->ver++;
}

void rhy_next(rhy_player_t *p) { if (p->on && cur(p)->norder > 1) p->next_req = true; }

void rhy_pause(rhy_player_t *p, bool pause) {
    if (!p->on || pause == p->paused) return;
    if (pause) {
        p->paused = true; p->stopping = true; p->nbeat = 0;
        fade_voices(p, ~0u, true);
        return;
    }
    const double u = rhy_u_at(p, p->n), L = bar_len(p);
    p->paused = false; p->stopping = false;
    restart(p, u < p->cin_end ? (int64_t)llround(p->cin_end / L) : (int64_t)floor(u / L));
}

void rhy_stop(rhy_player_t *p) {
    if (!p->on) return;
    p->ver++;
    if (p->paused && !p->stopping) { p->on = false; return; }
    p->quit = true; p->paused = true; p->stopping = true; p->nbeat = 0;
    fade_voices(p, ~0u, true);
}

size_t rhy_fill(rhy_player_t *p, int16_t *out, size_t n) {
    if (!rhy_sounding(p)) { memset(out, 0, n * sizeof(int16_t)); return 0; }
    for (size_t i = 0; i < n; i++) {
        if (!p->paused) {
            const double u = p->anc[0].u0 + ((double)p->n - (double)p->anc[0].n0) * p->anc[0].du;
            for (int guard = 0; u + 1e-9 >= ev_u(p) && guard < RHY_STEPS * 2; guard++) place(p);   /* (1e-9 units: a millionth of a sample, so an onset on a sample lands on it, not after) */
        }
        float s = 0.f;
        bool any = false;
        for (int k = 0; k < RHY_VOICES; k++) if (p->v[k].len) { s += voice_sample(p, &p->v[k]); any = true; }
        const float a = fabsf(s);
        if (a > KNEE) s = copysignf(KNEE + (1.f - KNEE) * tanhf((a - KNEE) / (1.f - KNEE)), s);
        out[i] = (int16_t)lrintf(s * FULL);
        p->n++;
        if (p->stopping && !any) {                     /* the fade is out: silence from here */
            p->stopping = false;
            if (p->quit) { p->on = false; p->quit = false; p->ver++; }
            memset(out + i + 1, 0, (n - i - 1) * sizeof(int16_t));
            return i + 1;
        }
    }
    return n;
}

bool rhy_take_beat(rhy_player_t *p, uint64_t upto, bool *first) {
    if (!p->nbeat || p->beat_n[0] > upto) return false;
    *first = p->beat_first[0];
    p->nbeat--;
    memmove(p->beat_n, p->beat_n + 1, sizeof(p->beat_n[0]) * (size_t)p->nbeat);
    memmove(p->beat_first, p->beat_first + 1, (size_t)p->nbeat);
    return true;
}

void rhy_view(const rhy_player_t *p, uint64_t n, rhy_view_t *o) {
    memset(o, 0, sizeof(*o));
    o->step = o->cin_step = -1;
    if (!p->nlib) return;
    const rhy_pattern_t *pt = cur(p);
    o->on = p->on && !p->quit; o->paused = p->paused; o->cur = p->cur; o->mute = p->mute; o->bpm = p->bpm; o->ver = p->ver;
    o->norder = pt->nbars ? pt->norder : 0;
    const double u = rhy_u_at(p, n < p->n ? n : p->n), L = bar_len(p), b = floor(u / L), um = u - b * L;
    int s = 0;
    while (s + 1 < pt->steps && rhy_onset(pt, s + 1) <= um) s++;
    const double ups = p->bpm * pt->per / 60.0;      /* units a second */
    o->count_in = u < p->cin_end - 1e-9;
    if (o->count_in) {
        int q = s;
        while (q > 0 && !(pt->beats >> q & 1)) q--;
        o->since = (float)((um - rhy_onset(pt, q)) / ups);
        o->cin_step = pt->beats >> q & 1 ? q : -1;
    } else {
        o->step = s;
        o->since = (float)((um - rhy_onset(pt, s)) / ups);
    }
    const int slot = (int)b + p->shift;
    o->slot = o->norder ? ((slot % o->norder) + o->norder) % o->norder : 0;
    for (int l = 0; l < pt->nlayers; l++) o->hits[l] = rhy_bar_hits(pt, slot, l);
}
