/*
 * SPDX-License-Identifier: MIT
 *
 * creature_player.c: a creature package, parsed strictly and played (creature_player.h).
 * Each part names the code it ports from device/creatures/js/creature-player.js; the
 * compile order follows it too, because a range's draw slot is its place in that order.
 */
#include "creature_player.h"
#include "pkg_parse.h"
#include "cute_act.h"
#include "cJSON.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define CP_MALLOC(n)     heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CP_REALLOC(p, n) heap_caps_realloc((p), (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CP_FREE(p)       heap_caps_free(p)
#else
#define CP_MALLOC(n)     malloc(n)
#define CP_REALLOC(p, n) realloc((p), (n))
#define CP_FREE(p)       free(p)
#endif

#define TAU 6.283185307179586
#define PI_F 3.14159265f

static const char *const MOODS[CP_N_MOODS] = { "idle", "listening", "thinking", "speaking", "happy", "sleepy", "surprised" };
static const char *const EYES[4] = { "wide", "happy", "sleepy", "squint" };
static const char *const POSE[18] = { "pos.x", "pos.y", "pos.z", "rot.x", "rot.y", "rot.z", "head.x", "head.y", "head.z",
                                      "spine.x", "spine.y", "spine.z", "squash", "lid", "mouth", "look.x", "look.y", "prowl" };
static const char *const WALK[7] = { "q", "k", "go", "along", "side", "fz", "up" };
static const char *const PROPS[11] = { "rx", "ry", "rz", "px", "py", "pz", "sx", "sy", "sz", "s", "sq" };
/* the pose fields a creature's own gesture takes back from the shared one (UNSHARE) */
#define N_UNSHARE 15

/* the value store: these slots first, in this order; then every name the creature uses */
enum { S_POSE = 0, S_T = 18, S_DT, S_ACT, S_WALK, S_IS = S_WALK + 7, S_W = S_IS + 7, S_W6 = S_W + 7,
       S_NOW = S_W6 + 7, S_M = S_NOW + 7, S_EYE = S_M + 7, S_FIXED = S_EYE + 4 };

/* ── the compiled program ─────────────────────────────────────────────────── */

enum { P_NUM, P_DUR, P_RANGE, P_ATTR, P_IDX, P_MIX };
typedef struct { uint8_t k; uint16_t i, n; double a, b; } prm_t;            /* RANGE: a lo, b hi, i slot; MIX: a base, i first, n count */
typedef struct { uint16_t slot; float k; } mix_t;

enum { O_V, O_SIN, O_NZ, O_FRAC, O_ENV, O_BUMP, O_SS, O_SSD, O_EXP, O_CYC, O_BUMPR, O_STEP, O_PULSE, O_DZ, O_STEPS,
       O_CHANCE, O_FLAP, O_LT, O_GT, O_IS, O_SGN, O_LIN, O_CLAMP, O_POW, O_RCP, O_ASIN, O_FL, O_DL, O_COUNT };
static const struct { const char *name; uint8_t lo, hi; } OPS[O_COUNT] = {
    { "v", 0, 0 }, { "sin", 0, 2 }, { "nz", 0, 3 }, { "frac", 0, 2 }, { "env", 3, 3 }, { "bump", 2, 2 }, { "ss", 2, 2 },
    { "ssd", 2, 2 }, { "exp", 1, 2 }, { "cyc", 3, 3 }, { "bumpr", 4, 4 }, { "step", 3, 3 }, { "pulse", 3, 3 }, { "dz", 2, 2 },
    { "steps", 2, 2 }, { "chance", 3, 4 }, { "flap", 0, 0 }, { "lt", 1, 1 }, { "gt", 1, 1 }, { "is", 1, 1 }, { "sgn", 0, 0 },
    { "lin", 2, 2 }, { "clamp", 2, 2 }, { "pow", 1, 1 }, { "rcp", 0, 2 }, { "asin", 0, 0 }, { "fl", 0, 1 }, { "dl", 1, 1 },
};
/* defaults of the params a package may leave off (the JS defaults) */
static const double DEFAULTS[O_COUNT][4] = { [O_SIN] = { 1, 0 }, [O_NZ] = { 1, 0, 0 }, [O_FRAC] = { 1, 0 }, [O_EXP] = { 0, 0 },
                                             [O_CHANCE] = { 0, 0, 0, 0 }, [O_RCP] = { 0, 1 }, [O_FL] = { 1 } };
enum { M_NONE, M_ABS, M_POS, M_NEG, M_SQ };
enum { X_NUM, X_A, X_DRAW, X_ATTR, X_IDX, X_LOCAL, X_CH, X_WIDE };

typedef struct { uint8_t op, mod, src, np; uint16_t si; double sv; prm_t p[4]; } fac_t;
typedef struct { prm_t g; uint16_t f0; uint8_t nf; } term_t;
typedef struct { uint16_t t0, nt; } terms_t;

enum { D_SLOT, D_LOCAL, D_BONE };
enum { F_INT = 1, F_D = 2, F_LP = 4, F_SP = 8, F_LAG = 16, F_CL = 32, F_UNW = 64 };
typedef struct {
    uint8_t dst, op, prop, flags;
    uint16_t to;                 /* slot, local or bone (GROUP_BONE: the group instance's)      */
    terms_t t;
    uint16_t st;                 /* first state (stateful: one per group instance)               */
    int16_t hist;                /* history ring, -1 none                                         */
    float cl[2], d[2], sp[2];
    prm_t lp;
} ch_t;
#define GROUP_BONE 0xFFFF
enum { OP_ADD, OP_MAX, OP_MUL, OP_SET };

/* a list is a run of items: >= 0 a channel, < 0 a group fan -(f + 1) */
typedef struct { uint16_t i0, n; } list_t;
typedef struct { uint8_t group, nlocal; list_t l; char local[32][CP_NAME_MAX + 1]; } fan_t;
typedef struct { int nb, nattr; uint16_t bone[CP_GROUP_MAX]; char attr_name[CP_ATTRS_MAX][25]; float *attr; } group_t;

typedef struct { char name[CP_NAME_MAX + 1]; uint8_t keep, wide; } slot_t;
typedef struct { double iv, dp; float lpv, spx, spv; uint8_t hasd, haslp, hassp; } st_t;
#define HIST_CAP 96
typedef struct { uint16_t slot, head, n; float keep; double t; double ht[HIST_CAP]; float hv[HIST_CAP]; } hist_t;

typedef struct { list_t l; prm_t len; } pick_t;
typedef struct {
    int namelen, npick, n;
    double first, rate, next;
    bool has_rate, has_gate, has_w, has_stop;
    terms_t gate, w, stop;
    prm_t gap;
    pick_t pick[CP_EVENTS_MAX];
    bool run; int ri; double ra, rseed, rlen;
} event_t;

typedef struct { list_t l; int act; bool replace, now; int mood; } block_t;
typedef struct { uint16_t ch; bool wave; float wave_v[3]; int16_t to[CP_PALETTE_MAX], neg[CP_PALETTE_MAX]; uint8_t rgb[2][CP_PALETTE_MAX][3]; } tint_t;

/* ambient */
typedef struct { bool range; float lo, hi; } rv_t;
enum { K_BUBBLE, K_BLOT };
typedef struct {
    uint8_t type;
    float rise[3], acc, glass, pop[5], big, shine;   /* pop: time, (colour), size, strength, radius */
    rv_t f, wob, vx;
    uint8_t pop_rgb[3], col[2][3], shine_rgb[3], blot_rgb[3];
    int pts; float life, sink, spin;
} kind_t;
enum { AT_FLOOR, AT_SIDE, AT_TOP, AT_BOTTOM, AT_CENTER };
enum { E_EVERY, E_ON, E_RATE, E_NONE };
typedef struct {
    uint8_t kind, at, mode; bool has_imp, has_rp;
    rv_t every, n, dx, dy, r;
    uint16_t on; uint16_t rate_ch; float rate_per, rate_min;
    float rp, stack, stagger, twin, imp[2];
    bool edge; double next;
} emit_t;
#define BLOT_PTS 16
typedef struct { uint8_t kind; float x, y, r, age, ph, vy, f, wob, vx, pop; bool popped; uint8_t npts; float pts[BLOT_PTS][3]; } part_t;
typedef struct { float x, y, k; } mote_t;
#define KINDS_MAX 8
#define EMITS_MAX 16
#define MOTES_MAX 64

struct cp {
    char id[CP_ID_MAX + 1], name[CP_NAME_MAX + 1], version[16];
    cp_body_t body;
    /* names and values */
    int nslot, slot_cap;
    slot_t *slot;                /* every name the creature uses, the fixed ones first           */
    float *C; double *Cd;        /* values; Cd: the wide slots' (clocks, integrated phases)      */
    /* program */
    fac_t *fac; int nfac, fac_cap;
    term_t *term; int nterm, term_cap;
    ch_t *ch; int nch, ch_cap;
    int16_t *item; int nitem, item_cap;
    fan_t fan[16]; int nfan;
    group_t group[8]; char group_name[8][25]; int ngroup;
    mix_t *mix; int nmix, mix_cap;
    st_t *st; int nst;
    hist_t *hist; int nhist;
    block_t walk, base, post;
    block_t *gest; int ngest;
    block_t mood[16]; int nmood;
    int gest_of[32];             /* cute_act index -> gesture block, -1 none                     */
    event_t ev[CP_EVENTS_MAX]; int nev;
    tint_t *tint; int ntint;
    /* ambient */
    bool amb;
    kind_t kind[KINDS_MAX]; int nkind;
    emit_t emit[EMITS_MAX]; int nemit;
    bool has_motes; int nmotes; float m_fall[2], m_drift, m_size[2]; uint8_t m_rgb[2][3];
    bool has_current; uint16_t cur_ch[4]; float cur_r;
    part_t live[CP_LIVE_MAX + 2]; int nlive;
    mote_t motes[MOTES_MAX]; bool motes_on;
    uint32_t seq;
    /* play state */
    double time, dt, mclock[CP_N_MOODS];
    int moodnow, wm[CP_N_MOODS], frame_factors;
    struct { double a, dur, seed; float w; } B;
    const fan_t *I; int ii; float L[32];
    float out[CP_BONES_MAX + 1][9];
    unsigned seed_n[2]; double seed_v[2];
};

/* ── maths (the JS helpers, the hash bit for bit) ──────────────────────────── */

double cp_hash(double n) {
    uint32_t h = (uint32_t)(int64_t)floor(n * 16) ^ 0x9e3779b9u;
    h = (h ^ (h >> 16)) * 0x85ebca6bu;
    h = (h ^ (h >> 13)) * 0xc2b2ae35u;
    return (double)(h ^ (h >> 16)) / 4294967296.0;
}
static inline float clf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline float ssf(float x) { x = clf(x, 0, 1); return x * x * (3 - 2 * x); }
static inline float bumpf(float x, float a, float b) { return x < a || x > b ? 0.f : sinf(PI_F * (x - a) / (b - a)); }
static inline double fract(double x) { return x - floor(x); }
static float noise(double x, double k) {
    double i = floor(x); float u = ssf((float)(x - i));
    return (float)((cp_hash(i + k) * (1 - u) + cp_hash(i + 1 + k) * u) * 2 - 1);
}
static inline double draw(const cp_t *c, double slot) { return cp_hash(c->B.seed * 7.31 + slot * 1.37); }

/* ── play ──────────────────────────────────────────────────────────────────── */

static double prm(const cp_t *c, const prm_t *p) {
    switch (p->k) {
    case P_NUM:   return p->a;
    case P_DUR:   return c->B.dur;
    case P_RANGE: return p->a + (p->b - p->a) * draw(c, p->i);
    case P_ATTR:  return c->I ? c->group[c->I->group].attr[p->i * CP_GROUP_MAX + c->ii] : 0;
    case P_IDX:   return c->ii;
    default: {
        double s = p->a;
        for (int k = 0; k < p->n; k++) s += c->mix[p->i + k].k * c->C[c->mix[p->i + k].slot];
        return s;
    }
    }
}

static float dl(const cp_t *c, uint16_t slot, double d) {
    const hist_t *h = NULL;
    for (int k = 0; k < c->nhist; k++) if (c->hist[k].slot == slot) h = &c->hist[k];
    if (!h || !h->n) return c->C[slot];
    double at = h->t - d;
    for (int j = h->n - 1; j >= 0; j--) { int q = (h->head + j) % HIST_CAP; if (h->ht[q] <= at) return h->hv[q]; }
    return h->hv[h->head];
}

static float factor(cp_t *c, const fac_t *f) {
    c->frame_factors++;
    double x;
    switch (f->src) {
    case X_NUM:   x = f->sv; break;
    case X_A:     x = c->B.a; break;
    case X_DRAW:  x = draw(c, f->sv); break;
    case X_ATTR:  x = c->I ? c->group[c->I->group].attr[f->si * CP_GROUP_MAX + c->ii] : 0; break;
    case X_IDX:   x = c->ii; break;
    case X_LOCAL: x = c->L[f->si]; break;
    case X_CH:    x = c->C[f->si]; break;
    default:      x = c->Cd[f->si]; break;
    }
    double P[4];
    for (int k = 0; k < f->np; k++) P[k] = prm(c, &f->p[k]);
    const float xf = (float)x;
    float v;
    switch (f->op) {
    case O_V:      v = xf; break;
    case O_SIN:    v = sinf((float)(TAU * fract(P[0] * x)) + (float)P[1]); break;
    case O_NZ:     v = noise(P[0] * x + P[2], P[1]); break;
    case O_FRAC:   v = (float)fract(P[0] * x + P[1]); break;
    case O_ENV:    v = ssf(xf / (float)P[1]) * ssf(((float)P[0] - xf) / (float)P[2]); break;
    case O_BUMP:   v = bumpf(xf, (float)P[0], (float)P[1]); break;
    case O_SS:     v = ssf((xf - (float)P[0]) / (float)P[1]); break;
    case O_SSD:    v = 1.f - ssf((xf - (float)P[0]) / (float)P[1]); break;
    case O_EXP:    v = expf(-(float)P[0] * fmaxf(0.f, xf - (float)P[1])); break;
    case O_CYC:  { float u = (float)fract(P[0] * x + P[1]), up = (float)P[2]; v = u < up ? ssf(u / up) : 1.f - ssf((u - up) / (1.f - up)); break; }
    case O_BUMPR:  v = bumpf((float)fract(P[0] * x + P[1]), (float)P[2], (float)P[3]); break;
    case O_STEP: { float u = (float)fract(P[0] * x + P[1]), d = (float)P[2]; v = u < d ? -1.f + 2.f * u / d : cosf(PI_F * (u - d) / (1.f - d)); break; }
    case O_PULSE: { double y = x * P[0] + 0.3 * noise(x * 0.6, P[1]), i = floor(y); float fr = (float)(y - i), w = (float)P[2];
                    v = fr < w ? sinf(PI_F * fr / w) * (float)(0.55 + 0.45 * cp_hash(i + P[1])) : 0.f; break; }
    case O_DZ:   { float n = noise(x * P[0], P[1]); v = (n > 0 ? 1.f : n < 0 ? -1.f : 0.f) * fmaxf(0.f, fabsf(n) - 0.35f) / 0.65f; break; }
    case O_STEPS: { double i = floor(x / P[0]); float s = ssf((float)((x - i * P[0]) / 0.09));
                    v = (float)(cp_hash(i - 1 + P[1]) * (1 - s) + cp_hash(i + P[1]) * s); break; }
    case O_CHANCE: v = cp_hash(floor((x - P[3]) / P[0]) + P[1]) < P[2] ? 1.f : 0.f; break;
    case O_FLAP: { float cc = cosf((float)(TAU * fract(x))); v = powf(0.5f - 0.5f * cc, cc < 0 ? 0.7f : 1.4f); break; }
    case O_LT:     v = x < P[0] ? 1.f : 0.f; break;
    case O_GT:     v = x > P[0] ? 1.f : 0.f; break;
    case O_IS:     v = floor(x + 0.5) == floor(P[0] + 0.5) ? 1.f : 0.f; break;
    case O_SGN:    v = x < 0.5 ? -1.f : 1.f; break;
    case O_LIN:    v = (float)(P[0] + P[1] * x); break;
    case O_CLAMP:  v = clf(xf, (float)P[0], (float)P[1]); break;
    case O_POW:    v = (xf > 0 ? 1.f : xf < 0 ? -1.f : 0.f) * powf(fabsf(xf), (float)P[0]); break;
    case O_RCP:  { float d = (float)(P[0] + P[1] * x); v = 1.f / (fabsf(d) < 1e-3f ? (d < 0 ? -1e-3f : 1e-3f) : d); break; }
    case O_ASIN:   v = asinf(clf(xf, -1, 1)); break;
    case O_FL:     v = (float)floor(P[0] * x); break;
    default:       v = dl(c, f->si, P[0]); break;
    }
    switch (f->mod) {
    case M_ABS: return fabsf(v);
    case M_POS: return fmaxf(0.f, v);
    case M_NEG: return fmaxf(0.f, -v);
    case M_SQ:  return v * v;
    default:    return v;
    }
}

static float terms(cp_t *c, terms_t t) {
    float s = 0;
    for (int k = 0; k < t.nt; k++) {
        const term_t *T = &c->term[t.t0 + k];
        float v = (float)prm(c, &T->g);
        for (int j = 0; j < T->nf && v != 0.f; j++) v *= factor(c, &c->fac[T->f0 + j]);
        s += v;
    }
    return s;
}

static void hist_push(cp_t *c, int hi, double v) {
    hist_t *h = &c->hist[hi];
    h->t = c->time;
    if (h->n == HIST_CAP) { h->head = (h->head + 1) % HIST_CAP; h->n--; }
    int q = (h->head + h->n) % HIST_CAP;
    h->ht[q] = c->time; h->hv[q] = (float)v; h->n++;
    while (h->n > 2 && h->ht[(h->head + 1) % HIST_CAP] < c->time - h->keep) { h->head = (h->head + 1) % HIST_CAP; h->n--; }
}

static void channel(cp_t *c, const ch_t *h) {
    double v = terms(c, h->t);
    const double dt = c->dt;
    if (h->flags & (F_INT | F_D | F_LP | F_SP)) {
        st_t *s = &c->st[h->st + (c->I ? c->ii : 0)];
        if (h->flags & F_INT) { v = s->iv + v * dt; s->iv = v; }
        if (h->flags & F_D) { double r = !s->hasd || !dt ? 0 : clf((float)((v - s->dp) / dt), h->d[0], h->d[1]); s->dp = v; s->hasd = 1; v = r; }
        if (h->flags & F_LP) { if (s->haslp) v = s->lpv + (v - s->lpv) * (1 - exp(-dt * prm(c, &h->lp))); s->lpv = (float)v; s->haslp = 1; }
        if (h->flags & F_SP) {
            if (!s->hassp) { s->spx = (float)v; s->spv = 0; s->hassp = 1; }
            const float target = (float)v, q = (float)dt / 4;
            for (int k = 0; k < 4; k++) { s->spv += (h->sp[0] * (target - s->spx) - h->sp[1] * s->spv) * q; s->spx += s->spv * q; }
            v = h->flags & F_LAG ? s->spx - target : s->spx;
        }
    }
    if (h->flags & F_CL) v = clf((float)v, h->cl[0], h->cl[1]);
    const double w = h->flags & F_UNW ? 1 : c->B.w;
    if (h->dst == D_BONE) {
        float *a = c->out[h->to == GROUP_BONE ? c->group[c->I->group].bone[c->ii] : h->to];
        if (h->prop == 10) { float y = (float)(1 + w * v), xz = 1.f / sqrtf(fmaxf(0.3f, y)); a[7] *= y; a[6] *= xz; a[8] *= xz; }
        else if (h->prop >= 6) { float f = (float)(h->op == OP_SET ? v : 1 + w * v); if (h->prop == 9) { a[6] *= f; a[7] *= f; a[8] *= f; } else a[h->prop] *= f; }
        else a[h->prop] += (float)(w * v);
        return;
    }
    const bool wide = h->dst == D_SLOT && c->slot[h->to].wide;
    double cur = h->dst == D_LOCAL ? c->L[h->to] : wide ? c->Cd[h->to] : c->C[h->to], r;
    switch (h->op) {
    case OP_ADD: r = cur + w * v; break;
    case OP_MAX: r = fmax(cur, w * v); break;
    case OP_MUL: r = cur * (1 + w * v); break;
    default:     r = cur + w * (v - cur); break;
    }
    if (h->dst == D_LOCAL) c->L[h->to] = (float)r;
    else { c->C[h->to] = (float)r; if (wide) c->Cd[h->to] = r; }
    if (h->hist >= 0) hist_push(c, h->hist, r);
}

static void list(cp_t *c, list_t l) {
    for (int k = 0; k < l.n; k++) {
        int it = c->item[l.i0 + k];
        if (it >= 0) { channel(c, &c->ch[it]); continue; }
        const fan_t *f = &c->fan[-it - 1];
        for (int i = 0; i < c->group[f->group].nb; i++) {
            c->I = f; c->ii = i;
            memset(c->L, 0, sizeof(float) * f->nlocal);
            for (int j = 0; j < f->l.n; j++) channel(c, &c->ch[f->l.i0 + j]);   /* a fan's channels are consecutive */
        }
        c->I = NULL; c->ii = 0;
    }
}

/* a block at its own clock, length, seed and weight (JS `at`) */
static void at(cp_t *c, list_t l, double a, double dur, double seed, double w) {
    c->B.a = a; c->B.dur = dur; c->B.seed = seed; c->B.w = (float)w;
    list(c, l);
    c->B.w = 1; c->B.seed = 0; c->B.a = c->time;
}

void cp_pose(cp_t *c, cp_pose_t *p, double dt, const cp_action_t *act, const cp_action_t *prev, float prev_w, const cp_walk_t *wk) {
    c->dt = dt; c->time += dt; c->frame_factors = 0;
    for (int b = 0; b <= c->body.nbones; b++) { float *a = c->out[b]; a[0] = a[1] = a[2] = a[3] = a[4] = a[5] = 0; a[6] = a[7] = a[8] = 1; }
    for (int s = 0; s < c->nslot; s++) if (!c->slot[s].keep) { c->C[s] = 0; c->Cd[s] = 0; }
    const float pv[18] = { p->pos[0], p->pos[1], p->pos[2], p->rot[0], p->rot[1], p->rot[2], p->head[0], p->head[1], p->head[2],
                           p->spine[0], p->spine[1], p->spine[2], p->squash, p->lid, p->mouth, p->look[0], p->look[1], p->prowl };
    for (int k = 0; k < 18; k++) c->C[S_POSE + k] = pv[k];
    const int mood = p->mood >= 0 && p->mood < CP_N_MOODS ? p->mood : 0;
    if (mood != c->moodnow) { c->mclock[mood] = 0; c->wm[mood]++; c->moodnow = mood; }
    const float k4 = (float)(1 - exp(-dt * 4)), k6 = (float)(1 - exp(-dt * 6));
    for (int m = 0; m < CP_N_MOODS; m++) {
        const float on = m == mood ? 1.f : 0.f;
        c->C[S_IS + m] = on;
        c->C[S_W + m] += (on - c->C[S_W + m]) * k4;
        c->C[S_W6 + m] += (on - c->C[S_W6 + m]) * k6;
        c->C[S_NOW + m] = on ? 1.f : c->C[S_W + m];
        if (m != mood) c->mclock[m] += dt;
    }
    c->mclock[mood] += dt;
    for (int m = 0; m < CP_N_MOODS; m++) { c->Cd[S_M + m] = c->mclock[m]; c->C[S_M + m] = (float)c->mclock[m]; }
    for (int e = 0; e < 4; e++) c->C[S_EYE + e] = p->eye == e + 1 ? 1.f : 0.f;
    const float wv[7] = { wk ? wk->q : 0, wk ? wk->k : 0, wk ? wk->go : 0, wk ? wk->along : 0, wk ? wk->side : 0, wk ? wk->fz : 0, wk ? wk->up : 0 };
    for (int k = 0; k < 7; k++) c->C[S_WALK + k] = wv[k];
    c->C[S_T] = (float)c->time; c->Cd[S_T] = c->time; c->C[S_DT] = (float)dt;
    c->C[S_ACT] = act && act->act >= 0 && act->act < 32 && act->act != CA_INSPECT ? 1.f : 0.f;

    at(c, c->walk.l, c->C[S_WALK], 1, 0, 1);

    for (int e = 0; e < c->nev; e++) {   /* each a scheduler: first, then gap (or rate), gated, cut short by stop */
        event_t *E = &c->ev[e];
        if (E->run) {
            E->ra += dt;
            if (E->ra > E->rlen || (E->has_stop && terms(c, E->stop) > 0.5f)) { E->run = false; c->B.seed = c->time * 3.7 + E->n; E->next = c->time + prm(c, &E->gap); }
        }
        if (!E->run) {
            const float g = E->has_gate ? terms(c, E->gate) : 1.f;
            const bool ok = E->has_rate ? cp_hash(c->time * 61.7 + E->n * 3.1) < E->rate * g * c->dt : c->time >= E->next && g > 0.5f;
            if (ok) {
                const double s = ++E->n * 17.3 + E->namelen;
                c->B.seed = s;
                E->ri = (int)floor(cp_hash(s * 0.71) * E->npick);
                E->run = true; E->ra = 0; E->rseed = s;
                E->rlen = prm(c, &E->pick[E->ri].len);
            }
        }
        if (E->run) at(c, E->pick[E->ri].l, E->ra, E->rlen, E->rseed, E->has_w ? terms(c, E->w) : 1.f);
    }

    at(c, c->base.l, c->time, 0, 0, 1);

    const cp_action_t *plays[2] = { prev, act };
    for (int k = 0; k < 2; k++) {        /* the gesture fading out, then the one playing */
        const cp_action_t *a = plays[k];
        const double w = k ? 1 : prev_w;
        const int gi = a && a->act >= 0 && a->act < 32 ? c->gest_of[a->act] : -1;
        if (gi < 0 || w <= 0) continue;
        const block_t *g = &c->gest[gi];
        if (g->replace) {
            cute_act_pose_t o; cute_act_pose(a->act, a->t, &o);
            const float sh[N_UNSHARE] = { o.pos[0], o.pos[1], o.pos[2], o.rot[0], o.rot[1], o.rot[2], o.head[0], o.head[1], o.head[2],
                                          o.spine[0], o.spine[1], o.spine[2], o.squash, o.look[0], o.look[1] };
            static const uint8_t AT_[N_UNSHARE] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 15, 16 };
            for (int j = 0; j < N_UNSHARE; j++) c->C[S_POSE + AT_[j]] -= (float)(w * sh[j]);
        }
        /* a play's seed, taken the first frame it is seen (JS a.seed ??= ...); n counts plays from 1 */
        const unsigned other = k ? (prev ? prev->n : 0) : (act ? act->n : 0);
        int si = c->seed_n[0] == a->n ? 0 : c->seed_n[1] == a->n ? 1 : -1;
        if (si < 0) { si = c->seed_n[0] == other && other ? 1 : 0; c->seed_n[si] = a->n; c->seed_v[si] = fmod(a->t * 977.0 + c->time * 31, 9973); }
        at(c, g->l, a->t, cute_act_dur(a->act), c->seed_v[si], w);
    }
    for (int m = 0; m < c->nmood; m++) {
        const block_t *b = &c->mood[m];
        if (b->mood < 0) continue;
        const float w = b->now ? c->C[S_NOW + b->mood] : c->C[S_W + b->mood];
        if (w > 0.01f) at(c, b->l, c->mclock[b->mood], 0, c->wm[b->mood] * 13.7 + b->mood, w);
    }
    at(c, c->post.l, c->time, 0, 0, 1);

    float *o[18] = { &p->pos[0], &p->pos[1], &p->pos[2], &p->rot[0], &p->rot[1], &p->rot[2], &p->head[0], &p->head[1], &p->head[2],
                     &p->spine[0], &p->spine[1], &p->spine[2], &p->squash, &p->lid, &p->mouth, &p->look[0], &p->look[1], &p->prowl };
    for (int k = 0; k < 18; k++) *o[k] = c->C[S_POSE + k];
}

const float *cp_bone_out(const cp_t *c, int b) { return c->out[b < 0 || b > c->body.nbones ? c->body.nbones : b]; }
int cp_factors(const cp_t *c) { return c->frame_factors; }
bool cp_tinted(const cp_t *c) { return c->ntint > 0; }

void cp_tints(const cp_t *c, float depth, uint32_t *out) {
    for (int i = 0; i < c->body.npal; i++) {
        float col[3] = { c->body.pal_rgb[i][0], c->body.pal_rgb[i][1], c->body.pal_rgb[i][2] };
        for (int t = 0; t < c->ntint; t++) {
            const tint_t *T = &c->tint[t];
            if (T->to[i] < 0 && T->neg[i] < 0) continue;
            float v = c->C[T->ch];
            if (T->wave) { float s = fmaxf(0.f, sinf((float)(TAU * fract(T->wave_v[0] * c->time - T->wave_v[1] * depth)))); v = clf(v, 0, 1) * T->wave_v[2] * s * s; }
            v = clf(v, -1, 1);
            const int side = v >= 0 ? 0 : 1;
            if ((side ? T->neg[i] : T->to[i]) < 0 || v == 0) continue;
            const float k = fabsf(v);
            for (int j = 0; j < 3; j++) col[j] += (T->rgb[side][i][j] - col[j]) * k;
        }
        uint32_t rgb = 0;
        for (int j = 0; j < 3; j++) rgb = rgb << 8 | (uint32_t)floorf(clf(col[j], 0, 255) + 0.5f);
        out[i] = rgb;
    }
}

/* ── the glass: particles and a current (JS ambientOf) ─────────────────────── */

static float rnd(cp_t *c) { return (float)cp_hash(c->seq++ * 0.7131 + 3.3); }
static float R(cp_t *c, const rv_t *v) { return v->range ? v->lo + (v->hi - v->lo) * rnd(c) : v->lo; }

typedef struct { const cp_env_t *e; float ox, oy, top, bot; int sn; cp_mark_t *out; int n, max; } amb_t;

static int cell(const amb_t *A, float x, float y) {
    const int W = A->e->W, H = A->e->H;
    int i = (int)floorf((x / A->e->win + 1) / 2 * W), j = (int)floorf((1 - y / A->e->win) / 2 * H);
    return i < 1 || j < 1 || i >= W - 1 || j >= H - 1 ? -1 : j * W + i;
}
static bool body_at(const amb_t *A, float x, float y) {
    const int k = cell(A, x, y), W = A->e->W; const float *cv = A->e->cov;
    return k >= 0 && (cv[k] > 0 || cv[k + 1] > 0 || cv[k - 1] > 0 || cv[k + W] > 0 || cv[k - W] > 0);
}
static void where(cp_t *c, const amb_t *A, int at, float *x, float *y) {
    switch (at) {
    case AT_FLOOR: { float u = (rnd(c) * 2 - 1) * 0.6f; *x = u; *y = -sqrtf(fmaxf(0.f, 0.8f - u * u)); break; }
    case AT_SIDE:   *x = A->ox + (A->top - A->oy) * 0.55f; *y = A->oy + (A->top - A->oy) * 0.15f; break;
    case AT_TOP:    *x = A->ox + 0.05f; *y = A->top - 0.05f; break;
    case AT_BOTTOM: *x = A->ox; *y = A->bot; break;
    default:        *x = A->ox; *y = A->oy - 0.05f; break;
    }
}
static void mark(amb_t *A, int kind, float x, float y, float size, float rot, const uint8_t *rgb, int d) {
    if (A->n >= A->max) return;
    cp_mark_t *m = &A->out[A->n++];
    m->kind = (uint8_t)kind; m->x = x; m->y = y; m->size = size; m->rot = rot; m->d = (uint8_t)d;
    memcpy(m->rgb, rgb, 3);
}
static void live_push(cp_t *c, const part_t *q) {
    c->live[c->nlive++] = *q;
    if (c->nlive > CP_LIVE_MAX) { memmove(c->live, c->live + 1, sizeof(part_t) * (size_t)--c->nlive); }
}

int cp_ambient(cp_t *c, const cp_env_t *env, cp_mark_t *out, int max) {
    if (!c->amb) return 0;
    amb_t A = { env, 0, 0, -1, 1, 0, out, 0, max };
    const float dt = env->dt, win = env->win, t = (float)c->time;
    const int W = env->W, H = env->H;
    float sx = 0, sy = 0;
    for (int k = 0; k < W * H; k++) if (env->cov[k] > 0) {
        float x = (((float)(k % W) + 0.5f) / W * 2 - 1) * win, y = (1 - ((float)(k / W) + 0.5f) / H * 2) * win;
        sx += x; sy += y; A.sn++;
        if (y > A.top) A.top = y;
        if (y < A.bot) A.bot = y;
    }
    A.ox = A.sn ? sx / A.sn : 0; A.oy = A.sn ? sy / A.sn : 0;
    A.top = fminf(A.top, A.oy + 0.5f); A.bot = fmaxf(A.bot, A.oy - 0.45f);
    for (int e = 0; e < c->nemit; e++) {
        emit_t *E = &c->emit[e];
        int n = 0;
        if (E->mode == E_EVERY) { if (c->time > E->next) { n = 1; E->next = c->time + R(c, &E->every); } }
        else if (E->mode == E_ON) { const float v = c->C[E->on]; if (v > 0.5f && !E->edge && A.sn) n = 1; E->edge = v > 0.5f; }
        else if (E->mode == E_RATE) { const float k = c->C[E->rate_ch]; if (k > E->rate_min && rnd(c) < E->rate_per * dt * k && A.sn) n = 1; }
        if (!n) continue;
        if (E->has_imp && A.sn) { float x, y; where(c, &A, E->at, &x, &y); if (env->impulse) env->impulse(env->ctx, x / win, -y / win, E->imp[0], E->imp[1]); }
        const kind_t *P = &c->kind[E->kind];
        const int cnt = (int)floorf(R(c, &E->n) + 0.5f);
        for (int k = 0; k < cnt; k++) {
            float x0, y0; where(c, &A, E->at, &x0, &y0);
            part_t q = { .kind = E->kind };
            q.x = x0 + R(c, &E->dx);
            q.y = y0 + R(c, &E->dy) - E->stack * k;
            q.r = E->has_rp ? E->r.lo + (E->r.hi - E->r.lo) * powf(rnd(c), E->rp) : R(c, &E->r);
            q.age = -E->stagger * k; q.ph = rnd(c) * (float)TAU;
            if (P->type == K_BUBBLE) { q.vy = P->rise[0] + P->rise[1] * q.r + P->rise[2] * rnd(c); q.f = R(c, &P->f); q.wob = R(c, &P->wob); }
            else {
                q.vx = R(c, &P->vx); q.npts = (uint8_t)P->pts;
                for (int j = 0; j < P->pts; j++) { float u = sqrtf(rnd(c)), an = rnd(c) * (float)TAU; q.pts[j][0] = 0.4f * u * cosf(an); q.pts[j][1] = 0.4f * u * sinf(an); q.pts[j][2] = 0.25f + 0.2f * rnd(c); }
            }
            live_push(c, &q);
            if (E->twin > 0 && rnd(c) < E->twin) { part_t tw = q; tw.x = q.x + 0.05f; tw.y = -0.75f; tw.r = 0.03f; tw.vy = P->rise[0] + P->rise[1] * 0.03f; c->live[c->nlive++] = tw; }
        }
    }
    for (int i = c->nlive - 1; i >= 0; i--) {
        part_t *q = &c->live[i]; const kind_t *P = &c->kind[q->kind];
        q->age += dt;
        if (q->age < 0) continue;
        if (P->type == K_BUBBLE) {
            q->vy += P->acc * dt; q->y += q->vy * dt;
            const float x = q->x + q->wob * sinf(q->ph + q->f * (float)TAU * q->age * 0.5f), lim = sqrtf(fmaxf(0.f, P->glass - x * x)) - q->r;
            if (q->y > lim) {
                if (!q->popped) { q->popped = true; q->pop = P->pop[0]; if (q->r > P->pop[1] && env->impulse) env->impulse(env->ctx, x / win, -q->y / win, P->pop[2], P->pop[3]); }
                q->pop -= dt;
                if (q->pop > 0) mark(&A, CP_MARK_STAR, x, q->y, 2.6f * q->r * (1.3f - q->pop * 2), q->ph, P->pop_rgb, 5);
                else { memmove(q, q + 1, sizeof(part_t) * (size_t)(c->nlive - i - 1)); c->nlive--; }
                continue;
            }
            if (!body_at(&A, x, q->y)) {
                mark(&A, CP_MARK_DOT, x, q->y, 2 * q->r, 0, q->r > P->big ? P->col[1] : P->col[0], 5);
                if (q->r > P->shine) mark(&A, CP_MARK_DOT, x - 0.3f * q->r, q->y + 0.35f * q->r, 0.6f * q->r, 0, P->shine_rgb, 6);
            }
        } else {
            if (q->age > P->life) { memmove(q, q + 1, sizeof(part_t) * (size_t)(c->nlive - i - 1)); c->nlive--; continue; }
            q->x += q->vx * dt; q->y -= P->sink * dt; q->ph += P->spin * dt;
            const float s = q->r * (0.5f + 2.4f * ssf(q->age / 0.5f)) * (1 - ssf((q->age - 1.4f) / 1.2f)), co = cosf(q->ph), si = sinf(q->ph);
            for (int j = 0; j < q->npts; j++) {
                const float u = q->pts[j][0], v = q->pts[j][1], x = q->x + s * (u * co - v * si), y = q->y + s * (u * si + v * co);
                if (!body_at(&A, x, y)) mark(&A, CP_MARK_BLOT, x, y, s * q->pts[j][2], q->ph + u, P->blot_rgb, 2);
            }
        }
    }
    if (c->has_motes) {
        if (!c->motes_on) {
            for (int i = 0; i < c->nmotes; i++) c->motes[i] = (mote_t){ (float)(cp_hash(i * 3.1) * 2 - 1) * 0.8f, (float)(cp_hash(i * 7.7) * 2 - 1) * 0.8f, (float)cp_hash(i * 1.3) };
            c->motes_on = true;
        }
        for (int i = 0; i < c->nmotes; i++) {
            mote_t *m = &c->motes[i];
            m->y -= (c->m_fall[0] + c->m_fall[1] * m->k) * dt;
            m->x += c->m_drift * noise(c->time * 0.2 + m->k * 9.0, 5) * dt;
            if (m->y < -0.85f) { m->y = 0.85f; m->x = (float)(cp_hash(c->time + m->k) * 2 - 1) * 0.7f; }
            if (hypotf(m->x, m->y) < 0.82f && !body_at(&A, m->x, m->y)) mark(&A, CP_MARK_DOT, m->x, m->y, c->m_size[0] + c->m_size[1] * m->k, 0, c->m_rgb[m->k > 0.6f], 1);
        }
    }
    if (c->has_current && env->push) env->push(env->ctx, c->C[c->cur_ch[0]], c->C[c->cur_ch[1]], c->C[c->cur_ch[2]], c->C[c->cur_ch[3]], c->cur_r);
    (void)t;
    return A.n;
}

/* ── parse + compile (JS creaturePlayer's construction, in its order) ──────── */

typedef struct { cp_t *c; const char *err; int ranges; const fan_t *fan; int fan_i; } cx_t;
#define FAIL(x, m) do { if (!(x)->err) (x)->err = (m); return -1; } while (0)
#define TRY(e)     do { if ((e) < 0) return -1; } while (0)

static bool grow(void **p, int *cap, int need, size_t sz) {
    if (need <= *cap) return true;
    int n = *cap ? *cap * 2 : 64;
    while (n < need) n *= 2;
    void *q = CP_REALLOC(*p, (size_t)n * sz);
    if (!q) return false;
    *p = q; *cap = n;
    return true;
}

static bool name_ok(const char *s, size_t max) {
    size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > max) return false;
    for (size_t i = 0; i < n; i++) if (s[i] <= 32 || s[i] > 126) return false;
    return true;
}

/* only the keys in `keys` (NULL-terminated) */
static int keys_only(cx_t *x, const cJSON *o, const char *const *keys, const char *what) {
    if (!cJSON_IsObject(o)) FAIL(x, what);
    for (const cJSON *k = o->child; k; k = k->next) {
        bool ok = false;
        for (int i = 0; keys[i]; i++) if (!strcmp(k->string, keys[i])) ok = true;
        if (!ok) FAIL(x, what);
    }
    return 0;
}

static int num(cx_t *x, const cJSON *v, double lo, double hi, double *out, const char *what) {
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < lo || v->valuedouble > hi) FAIL(x, what);
    *out = v->valuedouble;
    return 0;
}
static int numf(cx_t *x, const cJSON *v, double lo, double hi, float *out, const char *what) {
    double d; TRY(num(x, v, lo, hi, &d, what)); *out = (float)d; return 0;
}
/* n numbers into out */
static int nums(cx_t *x, const cJSON *a, int n, double lo, double hi, float *out, const char *what) {
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != n) FAIL(x, what);
    int i = 0;
    for (const cJSON *v = a->child; v; v = v->next) TRY(numf(x, v, lo, hi, &out[i++], what));
    return 0;
}
static bool truthy(const cJSON *v) { return cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valuedouble != 0); }

static int slot(cx_t *x, const char *name) {
    cp_t *c = x->c;
    for (int s = 0; s < c->nslot; s++) if (!strcmp(c->slot[s].name, name)) return s;
    if (!name_ok(name, CP_NAME_MAX)) FAIL(x, "creature: channel name");
    if (c->nslot >= 4 * CP_CHANNELS_MAX) FAIL(x, "creature: too many channels");
    if (!grow((void **)&c->slot, &c->slot_cap, c->nslot + 1, sizeof(slot_t))) FAIL(x, "oom");
    memset(&c->slot[c->nslot], 0, sizeof(slot_t));
    strcpy(c->slot[c->nslot].name, name);
    return c->nslot++;
}
static int local(cx_t *x, const char *name) {
    if (!x->fan) FAIL(x, "creature: a local outside a group");
    fan_t *f = &x->c->fan[x->fan_i];
    for (int i = 0; i < f->nlocal; i++) if (!strcmp(f->local[i], name)) return i;
    if (f->nlocal >= 32 || !name_ok(name, CP_NAME_MAX)) FAIL(x, "creature: locals");
    strcpy(f->local[f->nlocal], name);
    return f->nlocal++;
}
static int bone_of(const cp_t *c, const char *id) {
    if (!strcmp(id, "root")) return c->body.nbones;
    for (int b = 0; b < c->body.nbones; b++) if (!strcmp(c->body.bones[b].id, id)) return b;
    return -1;
}
static int mood_of(const char *s) {
    for (int m = 0; m < CP_N_MOODS; m++) if (!strcmp(s, MOODS[m])) return m;
    return -1;
}
static int attr_of(cx_t *x, const char *k, prm_t *p) {
    if (!x->fan) FAIL(x, "creature: an attribute outside a group");
    if (!strcmp(k, "i")) { p->k = P_IDX; return 0; }
    const group_t *g = &x->c->group[x->fan->group];
    for (int i = 0; i < g->nattr; i++) if (!strcmp(g->attr_name[i], k)) { p->k = P_ATTR; p->i = (uint16_t)i; return 0; }
    FAIL(x, "creature: unknown attribute");
}

/* param: a number, "D", [lo, hi] / [lo, hi, slot], "$attr", or (options) { "_": base, "<mood>": k } */
static int param(cx_t *x, const cJSON *v, prm_t *p) {
    memset(p, 0, sizeof(*p));
    if (cJSON_IsNumber(v)) { p->k = P_NUM; return num(x, v, -1e6, 1e6, &p->a, "creature: param"); }
    if (cJSON_IsString(v) && !strcmp(v->valuestring, "D")) { p->k = P_DUR; return 0; }
    if (cJSON_IsString(v) && v->valuestring[0] == '$') return attr_of(x, v->valuestring + 1, p);
    if (cJSON_IsArray(v)) {
        int n = cJSON_GetArraySize(v);
        if (n != 2 && n != 3) FAIL(x, "creature: range");
        double s;
        TRY(num(x, cJSON_GetArrayItem(v, 0), -1e6, 1e6, &p->a, "creature: range"));
        TRY(num(x, cJSON_GetArrayItem(v, 1), -1e6, 1e6, &p->b, "creature: range"));
        if (n == 3) TRY(num(x, cJSON_GetArrayItem(v, 2), 0, 999, &s, "creature: range slot"));
        else s = 1000 + x->ranges++;
        p->k = P_RANGE; p->i = (uint16_t)s;
        return 0;
    }
    if (cJSON_IsObject(v)) {
        cp_t *c = x->c;
        p->k = P_MIX; p->i = (uint16_t)c->nmix;
        for (const cJSON *e = v->child; e; e = e->next) {
            double k; TRY(num(x, e, -1e6, 1e6, &k, "creature: mood mix"));
            if (!strcmp(e->string, "_")) { p->a += k; continue; }
            int m = mood_of(e->string);
            if (m < 0) FAIL(x, "creature: mood mix");
            if (!grow((void **)&c->mix, &c->mix_cap, c->nmix + 1, sizeof(mix_t))) FAIL(x, "oom");
            c->mix[c->nmix++] = (mix_t){ (uint16_t)(S_W + m), (float)k };
            p->n++;
        }
        return 0;
    }
    FAIL(x, "creature: param");
}

static int source(cx_t *x, const cJSON *v, fac_t *f) {
    if (cJSON_IsNumber(v)) { f->src = X_NUM; return num(x, v, -1e6, 1e6, &f->sv, "creature: source"); }
    if (!cJSON_IsString(v)) FAIL(x, "creature: source");
    const char *s = v->valuestring;
    if (!strcmp(s, "a")) { f->src = X_A; return 0; }
    if (s[0] == '~') { char *end; f->sv = strtod(s + 1, &end); if (!s[1] || *end || !isfinite(f->sv)) FAIL(x, "creature: draw"); f->src = X_DRAW; return 0; }
    if (s[0] == '$') { prm_t p; TRY(attr_of(x, s + 1, &p)); f->src = p.k == P_IDX ? X_IDX : X_ATTR; f->si = p.i; return 0; }
    if (s[0] == '@' && s[1] == '%') { int l = local(x, s + 1); TRY(l); f->src = X_LOCAL; f->si = (uint16_t)l; return 0; }
    if (s[0] == '@') { int k = slot(x, s + 1); TRY(k); f->src = X_CH; f->si = (uint16_t)k; return 0; }
    FAIL(x, "creature: source");
}

static int factor_c(cx_t *x, const cJSON *v, fac_t *f) {
    memset(f, 0, sizeof(*f));
    if (!cJSON_IsArray(v)) { f->op = O_V; return source(x, v, f); }
    const int n = cJSON_GetArraySize(v);
    const cJSON *o = cJSON_GetArrayItem(v, 0);
    if (n < 2 || !cJSON_IsString(o) || strlen(o->valuestring) > 15) FAIL(x, "creature: factor");
    char op[16]; strcpy(op, o->valuestring);
    char *colon = strchr(op, ':');
    if (colon) {
        *colon = 0;
        static const char *const MODS[] = { "", "abs", "pos", "neg", "sq" };
        for (int m = 1; m < 5; m++) if (!strcmp(op, MODS[m])) f->mod = (uint8_t)m;
        if (!f->mod) FAIL(x, "creature: unknown modifier");
    }
    const char *name = colon ? colon + 1 : op;
    f->op = O_COUNT;
    for (int k = 0; k < O_COUNT; k++) if (!strcmp(name, OPS[k].name)) f->op = (uint8_t)k;
    if (f->op == O_COUNT) FAIL(x, "creature: unknown op");
    const cJSON *src = cJSON_GetArrayItem(v, 1);
    if (f->op == O_DL && !(cJSON_IsString(src) && src->valuestring[0] == '@' && src->valuestring[1] != '%')) FAIL(x, "creature: dl reads a channel");
    TRY(source(x, src, f));
    const int np = n - 2;
    if (np < OPS[f->op].lo || np > OPS[f->op].hi) FAIL(x, "creature: op params");
    int k = 0;
    for (const cJSON *p = src->next; p; p = p->next) TRY(param(x, p, &f->p[k++]));
    for (; k < OPS[f->op].hi; k++) f->p[k] = (prm_t){ .k = P_NUM, .a = DEFAULTS[f->op][k] };
    f->np = OPS[f->op].hi;
    return 0;
}

static int terms_c(cx_t *x, const cJSON *a, terms_t *t) {
    cp_t *c = x->c;
    if (!cJSON_IsArray(a)) FAIL(x, "creature: terms");
    t->t0 = (uint16_t)c->nterm; t->nt = 0;
    for (const cJSON *tv = a->child; tv; tv = tv->next) {
        const int n = cJSON_GetArraySize(tv);
        if (!cJSON_IsArray(tv) || n < 1 || n - 1 > CP_TERM_FACTORS) FAIL(x, "creature: term");
        if (c->nterm >= CP_TERMS_MAX || c->nfac + n - 1 > CP_FACTORS_MAX) FAIL(x, "creature: too many terms");
        if (!grow((void **)&c->term, &c->term_cap, c->nterm + 1, sizeof(term_t)) || !grow((void **)&c->fac, &c->fac_cap, c->nfac + n, sizeof(fac_t))) FAIL(x, "oom");
        term_t *T = &c->term[c->nterm++];
        T->f0 = (uint16_t)c->nfac; T->nf = (uint8_t)(n - 1);
        TRY(param(x, tv->child, &T->g));
        for (const cJSON *f = tv->child->next; f; f = f->next) TRY(factor_c(x, f, &c->fac[c->nfac++]));
        t->nt++;
    }
    return 0;
}

static int channel_c(cx_t *x, const cJSON *a) {
    cp_t *c = x->c;
    const int n = cJSON_GetArraySize(a);
    const cJSON *nm = cJSON_GetArrayItem(a, 0), *o = n == 3 ? cJSON_GetArrayItem(a, 2) : NULL;
    if (n < 2 || n > 3 || !cJSON_IsString(nm) || !name_ok(nm->valuestring, CP_NAME_MAX)) FAIL(x, "creature: channel");
    static const char *const OPT[] = { "op", "cl", "int", "lp", "sp", "lag", "d", "hist", "keep", NULL };
    if (o) TRY(keys_only(x, o, OPT, "creature: channel options"));
    if (c->nch >= CP_CHANNELS_MAX) FAIL(x, "creature: too many channels");
    if (!grow((void **)&c->ch, &c->ch_cap, c->nch + 1, sizeof(ch_t))) FAIL(x, "oom");
    ch_t h = { .hist = -1 };
    TRY(terms_c(x, cJSON_GetArrayItem(a, 1), &h.t));
    const char *name = nm->valuestring, *colon = strchr(name, ':');
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(o, "op"), *lp = cJSON_GetObjectItemCaseSensitive(o, "lp");
    if (truthy(cJSON_GetObjectItemCaseSensitive(o, "int"))) h.flags |= F_INT;
    if (cJSON_GetObjectItemCaseSensitive(o, "d")) { h.flags |= F_D; TRY(nums(x, cJSON_GetObjectItemCaseSensitive(o, "d"), 2, -1e6, 1e6, h.d, "creature: d")); }
    if (lp) { h.flags |= F_LP; TRY(param(x, lp, &h.lp)); }
    if (cJSON_GetObjectItemCaseSensitive(o, "sp")) { h.flags |= F_SP; TRY(nums(x, cJSON_GetObjectItemCaseSensitive(o, "sp"), 2, 0, 1e5, h.sp, "creature: sp")); }
    if (truthy(cJSON_GetObjectItemCaseSensitive(o, "lag"))) h.flags |= F_LAG;
    if (cJSON_GetObjectItemCaseSensitive(o, "cl")) { h.flags |= F_CL; TRY(nums(x, cJSON_GetObjectItemCaseSensitive(o, "cl"), 2, -1e6, 1e6, h.cl, "creature: cl")); }
    const bool stateful = h.flags & (F_INT | F_D | F_LP | F_SP);
    h.op = stateful ? OP_SET : OP_ADD;
    if (op) {
        static const char *const OPN[] = { "+", "max", "*", "=" };
        h.op = 4;
        for (int k = 0; k < 4; k++) if (cJSON_IsString(op) && !strcmp(op->valuestring, OPN[k])) h.op = (uint8_t)k;
        if (h.op == 4) FAIL(x, "creature: channel op");
    }
    if (stateful || name[0] == '_' || name[0] == '%') h.flags |= F_UNW;
    if (colon) {
        char b[32]; size_t bl = (size_t)(colon - name);
        if (bl >= sizeof(b)) FAIL(x, "creature: bone output");
        memcpy(b, name, bl); b[bl] = 0;
        h.dst = D_BONE; h.prop = 11;
        for (int k = 0; k < 11; k++) if (!strcmp(colon + 1, PROPS[k])) h.prop = (uint8_t)k;
        if (h.prop == 11) FAIL(x, "creature: unknown bone prop");
        if (!bl) { if (!x->fan) FAIL(x, "creature: ':prop' outside a group"); h.to = GROUP_BONE; }
        else { int bi = bone_of(c, b); if (bi < 0) FAIL(x, "creature: unknown bone"); h.to = (uint16_t)bi; }
    } else if (name[0] == '%') {
        int l = local(x, name); TRY(l); h.dst = D_LOCAL; h.to = (uint16_t)l;
    } else {
        int s = slot(x, name); TRY(s); h.dst = D_SLOT; h.to = (uint16_t)s;
    }
    const cJSON *hs = cJSON_GetObjectItemCaseSensitive(o, "hist");
    if (hs) {
        float keep; TRY(numf(x, hs, 0, CP_HIST_S, &keep, "creature: hist"));
        if (h.dst != D_SLOT) FAIL(x, "creature: hist on a channel");
        int hi = -1;
        for (int k = 0; k < c->nhist; k++) if (c->hist[k].slot == h.to) hi = k;
        if (hi < 0) {
            void *q = CP_REALLOC(c->hist, sizeof(hist_t) * (size_t)(c->nhist + 1));
            if (!q) FAIL(x, "oom");
            c->hist = q; hi = c->nhist++;
            memset(&c->hist[hi], 0, sizeof(hist_t));
            c->hist[hi].slot = h.to;
        }
        c->hist[hi].keep = keep;
        h.hist = (int16_t)hi;
    }
    if (truthy(cJSON_GetObjectItemCaseSensitive(o, "keep"))) { if (h.dst != D_SLOT) FAIL(x, "creature: keep"); c->slot[h.to].keep = 1; }
    if (stateful) { h.st = (uint16_t)c->nst; c->nst += x->fan ? c->group[x->fan->group].nb : 1; }
    if ((h.flags & F_INT) && h.dst == D_SLOT) c->slot[h.to].wide = 1;
    c->ch[c->nch] = h;
    return c->nch++;
}

static int item_push(cx_t *x, int v) {
    cp_t *c = x->c;
    if (!grow((void **)&c->item, &c->item_cap, c->nitem + 1, sizeof(int16_t))) FAIL(x, "oom");
    c->item[c->nitem++] = (int16_t)v;
    return 0;
}

/* a list: channels and group fans (whose channels are consecutive: fans do not nest) */
static int list_c(cx_t *x, const cJSON *a, list_t *l) {
    cp_t *c = x->c;
    if (!a) { l->i0 = (uint16_t)c->nitem; l->n = 0; return 0; }
    if (!cJSON_IsArray(a)) FAIL(x, "creature: channel list");
    const int cnt = cJSON_GetArraySize(a);
    if (cnt > CP_CHANNELS_MAX) FAIL(x, "creature: too many channels");
    int16_t *ids = CP_MALLOC(sizeof(int16_t) * (size_t)(cnt + 1)); int n = 0;
    if (!ids) FAIL(x, "oom");
    for (const cJSON *it = a->child; it && !x->err; it = it->next) {
        if (cJSON_IsArray(it)) { int k = channel_c(x, it); if (k >= 0) ids[n++] = (int16_t)k; continue; }
        static const char *const FK[] = { "each", "ch", NULL };
        if (keys_only(x, it, FK, "creature: group fan") < 0) break;
        const cJSON *each = cJSON_GetObjectItemCaseSensitive(it, "each"), *ch = cJSON_GetObjectItemCaseSensitive(it, "ch");
        if (x->fan || !cJSON_IsString(each) || !cJSON_IsArray(ch) || c->nfan >= 16) { x->err = "creature: group fan"; break; }
        int g = -1;
        for (int k = 0; k < c->ngroup; k++) if (!strcmp(c->group_name[k], each->valuestring)) g = k;
        if (g < 0) { x->err = "creature: unknown group"; break; }
        fan_t *f = &c->fan[c->nfan];
        f->group = (uint8_t)g; f->nlocal = 0; f->l.i0 = (uint16_t)c->nch; f->l.n = 0;
        x->fan = f; x->fan_i = c->nfan++;
        for (const cJSON *ci = ch->child; ci && !x->err; ci = ci->next) {
            if (!cJSON_IsArray(ci)) x->err = "creature: group channels";
            else if (channel_c(x, ci) >= 0) f->l.n++;
        }
        x->fan = NULL;
        ids[n++] = (int16_t)(-(f - c->fan) - 1);
    }
    l->i0 = (uint16_t)c->nitem; l->n = (uint16_t)n;
    for (int k = 0; k < n && !x->err; k++) item_push(x, ids[k]);
    CP_FREE(ids);
    return x->err ? -1 : 0;
}

static bool key_name(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > 24) return false;
    for (size_t i = 0; i < n; i++) { char ch = s[i]; if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return false; }
    return true;
}
static int rgb_of(cx_t *x, const cJSON *v, uint8_t *rgb) {
    uint32_t c;
    if (!cJSON_IsString(v) || !pkg_color(v->valuestring, &c)) FAIL(x, "creature: colour");
    rgb[0] = (uint8_t)(c >> 16); rgb[1] = (uint8_t)(c >> 8); rgb[2] = (uint8_t)c;
    return 0;
}
static int pal_of(const cp_body_t *b, const char *s) {
    for (int i = 0; i < b->npal; i++) if (!strcmp(b->pal_name[i], s)) return i;
    return -1;
}
static int vec3(cx_t *x, const cJSON *o, const char *k, float *out, const char *what) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return v ? nums(x, v, 3, -100, 100, out, what) : 0;
}
static int optnum(cx_t *x, const cJSON *o, const char *k, double lo, double hi, float def, float *out, const char *what) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    *out = def;
    return v ? numf(x, v, lo, hi, out, what) : 0;
}

/* body: the lab's v2 rig (palette, bones, parts) */
static int body_c(cx_t *x, const cJSON *bj) {
    cp_body_t *b = &x->c->body;
    static const char *const BK[] = { "palette", "bones", "parts", NULL };
    TRY(keys_only(x, bj, BK, "creature: body"));
    const cJSON *pal = cJSON_GetObjectItemCaseSensitive(bj, "palette"), *bones = cJSON_GetObjectItemCaseSensitive(bj, "bones"), *parts = cJSON_GetObjectItemCaseSensitive(bj, "parts");
    if (!cJSON_IsObject(pal) || !cJSON_IsArray(bones) || !cJSON_IsArray(parts)) FAIL(x, "creature: body");
    for (const cJSON *e = pal->child; e; e = e->next) {
        if (b->npal >= CP_PALETTE_MAX || !key_name(e->string)) FAIL(x, "creature: palette");
        strcpy(b->pal_name[b->npal], e->string);
        TRY(rgb_of(x, e, b->pal_rgb[b->npal++]));
    }
    static const char *const BO[] = { "id", "parent", "pos", "rot", "d", "sit", "fold", "wave", NULL };
    for (const cJSON *e = bones->child; e; e = e->next) {
        TRY(keys_only(x, e, BO, "creature: bone"));
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(e, "id"), *par = cJSON_GetObjectItemCaseSensitive(e, "parent");
        if (b->nbones >= CP_BONES_MAX || !cJSON_IsString(id) || !key_name(id->valuestring) || !strcmp(id->valuestring, "root") || bone_of(x->c, id->valuestring) >= 0) FAIL(x, "creature: bone id");
        cp_bone_t *B = &b->bones[b->nbones];
        memset(B, 0, sizeof(*B));
        strcpy(B->id, id->valuestring);
        B->parent = -1;
        if (par) {
            if (!cJSON_IsString(par)) FAIL(x, "creature: bone parent");
            int p = bone_of(x->c, par->valuestring);
            if (p < 0) FAIL(x, "creature: bone parent");   /* parents come first */
            B->parent = (int16_t)(p == b->nbones ? -1 : p);
        }
        TRY(vec3(x, e, "pos", B->pos, "creature: bone pos"));
        TRY(vec3(x, e, "rot", B->rot, "creature: bone rot"));
        TRY(optnum(x, e, "d", 0, 100, 0, &B->d, "creature: bone d"));
        b->nbones++;
    }
    static const char *const PK[] = { "type", "bone", "pos", "rot", "color", "size", "seg", "r", "r2", "h", "path", "plain", NULL };
    static const char *const TYPES[] = { "ellipsoid", "box", "cone", "cylinder", "tube" };
    for (const cJSON *e = parts->child; e; e = e->next) {
        TRY(keys_only(x, e, PK, "creature: part"));
        if (b->nparts >= CP_PARTS_MAX) FAIL(x, "creature: too many parts");
        cp_part_t *P = &b->parts[b->nparts];
        memset(P, 0, sizeof(*P));
        const cJSON *ty = cJSON_GetObjectItemCaseSensitive(e, "type"), *bo = cJSON_GetObjectItemCaseSensitive(e, "bone"), *co = cJSON_GetObjectItemCaseSensitive(e, "color");
        P->type = 5;
        for (int k = 0; k < 5; k++) if (cJSON_IsString(ty) && !strcmp(ty->valuestring, TYPES[k])) P->type = (uint8_t)k;
        if (P->type == 5) FAIL(x, "creature: part type");
        P->bone = -1;
        if (bo) { int bi = cJSON_IsString(bo) ? bone_of(x->c, bo->valuestring) : -1; if (bi < 0) FAIL(x, "creature: part bone"); P->bone = (int16_t)(bi == b->nbones ? -1 : bi); }
        if (!cJSON_IsString(co)) FAIL(x, "creature: part color");
        P->pal = (int16_t)pal_of(b, co->valuestring);
        if (P->pal < 0) TRY(rgb_of(x, co, P->rgb));
        else memcpy(P->rgb, b->pal_rgb[P->pal], 3);
        TRY(vec3(x, e, "pos", P->pos, "creature: part pos"));
        TRY(vec3(x, e, "rot", P->rot, "creature: part rot"));
        const cJSON *seg = cJSON_GetObjectItemCaseSensitive(e, "seg");
        float sg[2] = { 0, 0 };
        if (P->type == CP_ELLIPSOID) { sg[0] = 12; sg[1] = 8; if (seg) TRY(nums(x, seg, 2, 3, 32, sg, "creature: seg")); }
        else if (seg) TRY(numf(x, seg, 1, 64, &sg[0], "creature: seg"));
        else sg[0] = P->type == CP_CONE ? 8 : P->type == CP_CYLINDER ? 10 : 32;
        P->seg[0] = (uint8_t)sg[0]; P->seg[1] = (uint8_t)sg[1];
        if (P->type == CP_ELLIPSOID || P->type == CP_BOX) {
            const cJSON *sz = cJSON_GetObjectItemCaseSensitive(e, "size");
            if (!sz) FAIL(x, "creature: part size");
            TRY(nums(x, sz, 3, 0, 100, P->size, "creature: part size"));
        } else {
            const cJSON *r = cJSON_GetObjectItemCaseSensitive(e, "r");
            if (!r) FAIL(x, "creature: part r");
            TRY(numf(x, r, 0, 100, &P->r, "creature: part r"));
            TRY(optnum(x, e, "r2", 0, 100, P->r, &P->r2, "creature: part r2"));
            if (P->type != CP_TUBE) {
                if (!cJSON_GetObjectItemCaseSensitive(e, "h")) FAIL(x, "creature: part h");
                TRY(optnum(x, e, "h", 0, 100, 0, &P->h, "creature: part h"));
                if (P->seg[0] < 3) FAIL(x, "creature: seg");
            } else {
                const cJSON *path = cJSON_GetObjectItemCaseSensitive(e, "path");
                int n = cJSON_GetArraySize(path);
                if (!cJSON_IsArray(path) || n < 2 || n > CP_PATH_MAX) FAIL(x, "creature: tube path");
                for (const cJSON *pt = path->child; pt; pt = pt->next) TRY(nums(x, pt, 3, -100, 100, P->path[P->npath++], "creature: tube path"));
            }
        }
        b->nparts++;
    }
    return 0;
}

static int groups_c(cx_t *x, const cJSON *gj) {
    cp_t *c = x->c;
    if (!gj) return 0;
    if (!cJSON_IsObject(gj)) FAIL(x, "creature: groups");
    static const char *const GK[] = { "bones", "attr", NULL };
    for (const cJSON *e = gj->child; e; e = e->next) {
        TRY(keys_only(x, e, GK, "creature: group"));
        if (c->ngroup >= 8 || !key_name(e->string)) FAIL(x, "creature: groups");
        group_t *g = &c->group[c->ngroup];
        strcpy(c->group_name[c->ngroup++], e->string);
        const cJSON *bones = cJSON_GetObjectItemCaseSensitive(e, "bones"), *attr = cJSON_GetObjectItemCaseSensitive(e, "attr");
        if (!cJSON_IsArray(bones) || cJSON_GetArraySize(bones) > CP_GROUP_MAX) FAIL(x, "creature: group bones");
        for (const cJSON *b = bones->child; b; b = b->next) {
            int bi = cJSON_IsString(b) ? bone_of(c, b->valuestring) : -1;
            if (bi < 0) FAIL(x, "creature: group bone");
            g->bone[g->nb++] = (uint16_t)bi;
        }
        g->attr = CP_MALLOC(sizeof(float) * CP_ATTRS_MAX * CP_GROUP_MAX);
        if (!g->attr) FAIL(x, "oom");
        if (!attr) continue;
        if (!cJSON_IsObject(attr)) FAIL(x, "creature: group attr");
        for (const cJSON *a = attr->child; a; a = a->next) {
            if (g->nattr >= CP_ATTRS_MAX || !key_name(a->string) || !strcmp(a->string, "i")) FAIL(x, "creature: group attr");
            strcpy(g->attr_name[g->nattr], a->string);
            TRY(nums(x, a, g->nb, -1e6, 1e6, &g->attr[g->nattr * CP_GROUP_MAX], "creature: group attr"));
            g->nattr++;
        }
    }
    return 0;
}

static int block_c(cx_t *x, const cJSON *bj, block_t *b, const char *flag) {
    const char *const K[] = { "ch", flag, NULL };
    TRY(keys_only(x, bj, K, "creature: block"));
    const cJSON *f = flag ? cJSON_GetObjectItemCaseSensitive(bj, flag) : NULL;
    if (f && !cJSON_IsBool(f) && !cJSON_IsNumber(f)) FAIL(x, "creature: block");
    b->replace = !f || truthy(f);   /* gestures: replace (default true); moods: now (default false) */
    b->now = f && truthy(f);
    return list_c(x, cJSON_GetObjectItemCaseSensitive(bj, "ch"), &b->l);
}

static int events_c(cx_t *x, const cJSON *ej) {
    cp_t *c = x->c;
    if (!ej) return 0;
    if (!cJSON_IsObject(ej)) FAIL(x, "creature: events");
    static const char *const EK[] = { "first", "gap", "len", "rate", "gate", "w", "stop", "ch", "pick", NULL };
    for (const cJSON *e = ej->child; e; e = e->next) {
        TRY(keys_only(x, e, EK, "creature: event"));
        if (c->nev >= CP_EVENTS_MAX || !key_name(e->string)) FAIL(x, "creature: events");
        event_t *E = &c->ev[c->nev++];
        memset(E, 0, sizeof(*E));
        E->namelen = (int)strlen(e->string);
        const cJSON *pick = cJSON_GetObjectItemCaseSensitive(e, "pick"), *len = cJSON_GetObjectItemCaseSensitive(e, "len");
        const cJSON *lens[CP_EVENTS_MAX];
        if (pick) {
            if (!cJSON_IsObject(pick) || cJSON_HasObjectItem(e, "ch")) FAIL(x, "creature: event pick");
            static const char *const MK[] = { "ch", "len", NULL };
            for (const cJSON *m = pick->child; m; m = m->next) {
                TRY(keys_only(x, m, MK, "creature: event pick"));
                if (E->npick >= CP_EVENTS_MAX) FAIL(x, "creature: event pick");
                lens[E->npick] = cJSON_HasObjectItem(m, "len") ? cJSON_GetObjectItemCaseSensitive(m, "len") : len;
                TRY(list_c(x, cJSON_GetObjectItemCaseSensitive(m, "ch"), &E->pick[E->npick++].l));
            }
            if (!E->npick) FAIL(x, "creature: event pick");
        } else {
            lens[0] = len;
            TRY(list_c(x, cJSON_GetObjectItemCaseSensitive(e, "ch"), &E->pick[0].l));
            E->npick = 1;
        }
        const cJSON *gate = cJSON_GetObjectItemCaseSensitive(e, "gate"), *w = cJSON_GetObjectItemCaseSensitive(e, "w"), *stop = cJSON_GetObjectItemCaseSensitive(e, "stop");
        if (gate) { E->has_gate = true; TRY(terms_c(x, gate, &E->gate)); }
        if (w) { E->has_w = true; TRY(terms_c(x, w, &E->w)); }
        if (stop) { E->has_stop = true; TRY(terms_c(x, stop, &E->stop)); }
        const cJSON *gap = cJSON_GetObjectItemCaseSensitive(e, "gap");
        if (gap) TRY(param(x, gap, &E->gap)); else E->gap = (prm_t){ .k = P_NUM };
        for (int k = 0; k < E->npick; k++) {
            if (lens[k]) TRY(param(x, lens[k], &E->pick[k].len));
            else E->pick[k].len = (prm_t){ .k = P_NUM, .a = 1 };
        }
        float f;
        TRY(optnum(x, e, "first", 0, 1e6, 0, &f, "creature: event first")); E->first = E->next = f;
        TRY(optnum(x, e, "rate", 0, 1e3, 0, &f, "creature: event rate")); E->rate = f; E->has_rate = f != 0;
    }
    return 0;
}

static int tints_c(cx_t *x, const cJSON *tj) {
    cp_t *c = x->c;
    if (!tj) return 0;
    if (!cJSON_IsArray(tj) || cJSON_GetArraySize(tj) > 8) FAIL(x, "creature: tints");
    c->tint = CP_MALLOC(sizeof(tint_t) * 8);
    if (!c->tint) FAIL(x, "oom");
    static const char *const TK[] = { "ch", "to", "neg", "wave", NULL };
    for (const cJSON *e = tj->child; e; e = e->next) {
        TRY(keys_only(x, e, TK, "creature: tint"));
        tint_t *T = &c->tint[c->ntint++];
        memset(T, 0, sizeof(*T));
        for (int i = 0; i < CP_PALETTE_MAX; i++) T->to[i] = T->neg[i] = -1;
        const cJSON *ch = cJSON_GetObjectItemCaseSensitive(e, "ch"), *wave = cJSON_GetObjectItemCaseSensitive(e, "wave");
        if (!cJSON_IsString(ch)) FAIL(x, "creature: tint ch");
        int s = slot(x, ch->valuestring); TRY(s); T->ch = (uint16_t)s;
        if (wave) { T->wave = true; TRY(nums(x, wave, 3, -1e3, 1e3, T->wave_v, "creature: tint wave")); }
        const char *sides[2] = { "to", "neg" };
        for (int k = 0; k < 2; k++) {
            const cJSON *m = cJSON_GetObjectItemCaseSensitive(e, sides[k]);
            if (!m) continue;
            if (!cJSON_IsObject(m)) FAIL(x, "creature: tint");
            for (const cJSON *pe = m->child; pe; pe = pe->next) {
                int i = pal_of(&c->body, pe->string);
                if (i < 0) FAIL(x, "creature: tint palette name");
                TRY(rgb_of(x, pe, T->rgb[k][i]));
                (k ? T->neg : T->to)[i] = (int16_t)i;
            }
        }
    }
    return 0;
}

static int rv(cx_t *x, const cJSON *v, rv_t *r, float def, const char *what) {
    r->range = false; r->lo = r->hi = def;
    if (!v) return 0;
    if (cJSON_IsArray(v)) { float a[2]; TRY(nums(x, v, 2, -1e3, 1e3, a, what)); r->range = true; r->lo = a[0]; r->hi = a[1]; return 0; }
    TRY(numf(x, v, -1e3, 1e3, &r->lo, what)); r->hi = r->lo;
    return 0;
}

static int ambient_c(cx_t *x, const cJSON *aj) {
    cp_t *c = x->c;
    if (!aj) return 0;
    static const char *const AK[] = { "kinds", "emit", "motes", "current", NULL };
    TRY(keys_only(x, aj, AK, "creature: ambient"));
    c->amb = true;
    const cJSON *kinds = cJSON_GetObjectItemCaseSensitive(aj, "kinds"), *emit = cJSON_GetObjectItemCaseSensitive(aj, "emit");
    const cJSON *motes = cJSON_GetObjectItemCaseSensitive(aj, "motes"), *cur = cJSON_GetObjectItemCaseSensitive(aj, "current");
    char names[KINDS_MAX][25];
    if (kinds) {
        if (!cJSON_IsObject(kinds)) FAIL(x, "creature: ambient kinds");
        static const char *const BK[] = { "type", "rise", "acc", "f", "wob", "glass", "pop", "big", "color", "shine", NULL };
        static const char *const LK[] = { "type", "vx", "pts", "life", "sink", "spin", "color", NULL };
        for (const cJSON *k = kinds->child; k; k = k->next) {
            if (c->nkind >= KINDS_MAX || !key_name(k->string)) FAIL(x, "creature: ambient kinds");
            strcpy(names[c->nkind], k->string);
            kind_t *P = &c->kind[c->nkind++];
            const cJSON *ty = cJSON_GetObjectItemCaseSensitive(k, "type");
            if (cJSON_IsString(ty) && !strcmp(ty->valuestring, "bubble")) {
                TRY(keys_only(x, k, BK, "creature: bubble"));
                P->type = K_BUBBLE;
                TRY(nums(x, cJSON_GetObjectItemCaseSensitive(k, "rise"), 3, -10, 10, P->rise, "creature: bubble rise"));
                TRY(optnum(x, k, "acc", -10, 10, 0, &P->acc, "creature: bubble acc"));
                TRY(rv(x, cJSON_GetObjectItemCaseSensitive(k, "f"), &P->f, 0, "creature: bubble f"));
                TRY(rv(x, cJSON_GetObjectItemCaseSensitive(k, "wob"), &P->wob, 0, "creature: bubble wob"));
                TRY(optnum(x, k, "glass", 0, 4, 0.82f, &P->glass, "creature: bubble glass"));
                TRY(optnum(x, k, "big", 0, 10, 0, &P->big, "creature: bubble big"));
                const cJSON *pop = cJSON_GetObjectItemCaseSensitive(k, "pop"), *col = cJSON_GetObjectItemCaseSensitive(k, "color"), *sh = cJSON_GetObjectItemCaseSensitive(k, "shine");
                if (cJSON_GetArraySize(pop) != 5 || cJSON_GetArraySize(col) != 2 || cJSON_GetArraySize(sh) != 2) FAIL(x, "creature: bubble");
                const int pi[4] = { 0, 2, 3, 4 };
                for (int j = 0; j < 4; j++) TRY(numf(x, cJSON_GetArrayItem(pop, pi[j]), 0, 100, &P->pop[j], "creature: bubble pop"));
                TRY(rgb_of(x, cJSON_GetArrayItem(pop, 1), P->pop_rgb));
                TRY(rgb_of(x, cJSON_GetArrayItem(col, 0), P->col[0])); TRY(rgb_of(x, cJSON_GetArrayItem(col, 1), P->col[1]));
                TRY(numf(x, cJSON_GetArrayItem(sh, 0), 0, 10, &P->shine, "creature: bubble shine")); TRY(rgb_of(x, cJSON_GetArrayItem(sh, 1), P->shine_rgb));
            } else if (cJSON_IsString(ty) && !strcmp(ty->valuestring, "blot")) {
                TRY(keys_only(x, k, LK, "creature: blot"));
                P->type = K_BLOT;
                float pts;
                TRY(rv(x, cJSON_GetObjectItemCaseSensitive(k, "vx"), &P->vx, 0, "creature: blot vx"));
                TRY(optnum(x, k, "pts", 1, BLOT_PTS, 1, &pts, "creature: blot pts")); P->pts = (int)pts;
                TRY(optnum(x, k, "life", 0, 60, 1, &P->life, "creature: blot life"));
                TRY(optnum(x, k, "sink", -10, 10, 0, &P->sink, "creature: blot sink"));
                TRY(optnum(x, k, "spin", -100, 100, 0, &P->spin, "creature: blot spin"));
                TRY(rgb_of(x, cJSON_GetObjectItemCaseSensitive(k, "color"), P->blot_rgb));
            } else FAIL(x, "creature: ambient kind type");
        }
    }
    if (emit) {
        if (!cJSON_IsObject(emit)) FAIL(x, "creature: ambient emit");
        static const char *const EK[] = { "p", "at", "every", "on", "rate", "n", "dx", "dy", "r", "rp", "stack", "stagger", "twin", "impulse", NULL };
        static const char *const ATS[] = { "floor", "side", "top", "bottom", "center" };
        for (const cJSON *e = emit->child; e; e = e->next) {
            TRY(keys_only(x, e, EK, "creature: emit"));
            if (c->nemit >= EMITS_MAX) FAIL(x, "creature: emit");
            emit_t *E = &c->emit[c->nemit++];
            memset(E, 0, sizeof(*E));
            const cJSON *p = cJSON_GetObjectItemCaseSensitive(e, "p"), *at = cJSON_GetObjectItemCaseSensitive(e, "at");
            E->kind = 0xFF; E->at = 0xFF;
            for (int k = 0; k < c->nkind; k++) if (cJSON_IsString(p) && !strcmp(p->valuestring, names[k])) E->kind = (uint8_t)k;
            for (int k = 0; k < 5; k++) if (cJSON_IsString(at) && !strcmp(at->valuestring, ATS[k])) E->at = (uint8_t)k;
            if (E->kind == 0xFF || E->at == 0xFF) FAIL(x, "creature: emit p / at");
            const cJSON *every = cJSON_GetObjectItemCaseSensitive(e, "every"), *on = cJSON_GetObjectItemCaseSensitive(e, "on"), *rate = cJSON_GetObjectItemCaseSensitive(e, "rate");
            E->mode = every ? E_EVERY : on ? E_ON : rate ? E_RATE : E_NONE;
            if (every) TRY(rv(x, every, &E->every, 0, "creature: emit every"));
            else if (on) { if (!cJSON_IsString(on)) FAIL(x, "creature: emit on"); int s = slot(x, on->valuestring); TRY(s); E->on = (uint16_t)s; }
            else if (rate) {
                if (cJSON_GetArraySize(rate) != 3 || !cJSON_IsString(rate->child)) FAIL(x, "creature: emit rate");
                int s = slot(x, rate->child->valuestring); TRY(s); E->rate_ch = (uint16_t)s;
                TRY(numf(x, rate->child->next, 0, 1e3, &E->rate_per, "creature: emit rate"));
                TRY(numf(x, rate->child->next->next, -1e3, 1e3, &E->rate_min, "creature: emit rate"));
            }
            TRY(rv(x, cJSON_GetObjectItemCaseSensitive(e, "n"), &E->n, 1, "creature: emit n"));
            TRY(rv(x, cJSON_GetObjectItemCaseSensitive(e, "dx"), &E->dx, 0, "creature: emit dx"));
            TRY(rv(x, cJSON_GetObjectItemCaseSensitive(e, "dy"), &E->dy, 0, "creature: emit dy"));
            TRY(rv(x, cJSON_GetObjectItemCaseSensitive(e, "r"), &E->r, 0, "creature: emit r"));
            TRY(optnum(x, e, "rp", 0, 10, 0, &E->rp, "creature: emit rp"));
            E->has_rp = E->rp != 0;
            if (E->has_rp && !E->r.range) FAIL(x, "creature: emit rp needs an r range");
            TRY(optnum(x, e, "stack", -10, 10, 0, &E->stack, "creature: emit stack"));
            TRY(optnum(x, e, "stagger", 0, 10, 0, &E->stagger, "creature: emit stagger"));
            TRY(optnum(x, e, "twin", 0, 1, 0, &E->twin, "creature: emit twin"));
            const cJSON *imp = cJSON_GetObjectItemCaseSensitive(e, "impulse");
            if (imp) { E->has_imp = true; TRY(nums(x, imp, 2, 0, 100, E->imp, "creature: emit impulse")); }
        }
    }
    if (motes) {
        static const char *const MK[] = { "n", "fall", "drift", "size", "color", NULL };
        TRY(keys_only(x, motes, MK, "creature: motes"));
        float n;
        TRY(optnum(x, motes, "n", 0, MOTES_MAX, 0, &n, "creature: motes n")); c->nmotes = (int)n;
        TRY(nums(x, cJSON_GetObjectItemCaseSensitive(motes, "fall"), 2, -10, 10, c->m_fall, "creature: motes fall"));
        TRY(optnum(x, motes, "drift", -10, 10, 0, &c->m_drift, "creature: motes drift"));
        TRY(nums(x, cJSON_GetObjectItemCaseSensitive(motes, "size"), 2, 0, 10, c->m_size, "creature: motes size"));
        const cJSON *col = cJSON_GetObjectItemCaseSensitive(motes, "color");
        if (cJSON_GetArraySize(col) != 2) FAIL(x, "creature: motes color");
        TRY(rgb_of(x, cJSON_GetArrayItem(col, 0), c->m_rgb[0])); TRY(rgb_of(x, cJSON_GetArrayItem(col, 1), c->m_rgb[1]));
        c->has_motes = true;
    }
    if (cur) {
        static const char *const CK[] = { "ch", "r", NULL };
        TRY(keys_only(x, cur, CK, "creature: current"));
        const cJSON *ch = cJSON_GetObjectItemCaseSensitive(cur, "ch");
        if (!cJSON_IsArray(ch) || cJSON_GetArraySize(ch) != 4) FAIL(x, "creature: current");
        int k = 0;
        for (const cJSON *v = ch->child; v; v = v->next) { if (!cJSON_IsString(v)) FAIL(x, "creature: current"); int s = slot(x, v->valuestring); TRY(s); c->cur_ch[k++] = (uint16_t)s; }
        TRY(optnum(x, cur, "r", 0, 4, 0.18f, &c->cur_r, "creature: current r"));
        c->has_current = true;
    }
    return 0;
}

static int walk_c(cx_t *x, const cJSON *wj) {
    if (!wj) { x->c->walk.l = (list_t){ (uint16_t)x->c->nitem, 0 }; return 0; }
    static const char *const WK[] = { "hz", "stride", "pace", "burst", "radial", "quad", "surge", "ch", NULL };
    TRY(keys_only(x, wj, WK, "creature: walk"));
    float v;
    TRY(optnum(x, wj, "hz", 0, 100, 1, &v, "creature: walk hz"));
    TRY(optnum(x, wj, "stride", 0, 100, 0, &v, "creature: walk stride"));
    TRY(optnum(x, wj, "pace", 0, 100, 0, &v, "creature: walk pace"));
    const cJSON *quad = cJSON_GetObjectItemCaseSensitive(wj, "quad");
    if (quad) {
        static const char *const QK[] = { "pitch", "rise", "neck", NULL };
        TRY(keys_only(x, quad, QK, "creature: walk quad"));
        for (const cJSON *q = quad->child; q; q = q->next) TRY(numf(x, q, -10, 10, &v, "creature: walk quad"));
    }
    return list_c(x, cJSON_GetObjectItemCaseSensitive(wj, "ch"), &x->c->walk.l);
}

static const char *compile(cp_t *c, const cJSON *cr) {
    cx_t x = { .c = c };
    static const char *const CK[] = { "body", "walk", "groups", "events", "base", "gestures", "moods", "post", "tints", "ambient", NULL };
    if (keys_only(&x, cr, CK, "creature: unknown key") < 0) return x.err;
    for (int k = 0; k < 18; k++) slot(&x, POSE[k]);
    slot(&x, "t"); slot(&x, "dt"); slot(&x, "act");
    char nm[CP_NAME_MAX + 1];
    for (int k = 0; k < 7; k++) { snprintf(nm, sizeof(nm), "walk.%s", WALK[k]); slot(&x, nm); }
    static const char *const PRE[5] = { "is", "w", "w6", "now", "m" };
    for (int p = 0; p < 5; p++) for (int m = 0; m < CP_N_MOODS; m++) { snprintf(nm, sizeof(nm), "%s.%s", PRE[p], MOODS[m]); slot(&x, nm); }
    for (int e = 0; e < 4; e++) { snprintf(nm, sizeof(nm), "eye.%s", EYES[e]); slot(&x, nm); }
    if (x.err || c->nslot != S_FIXED) return x.err ? x.err : "creature: slots";
    for (int k = 0; k < 32; k++) c->gest_of[k] = -1;
    if (body_c(&x, cJSON_GetObjectItemCaseSensitive(cr, "body")) < 0 || groups_c(&x, cJSON_GetObjectItemCaseSensitive(cr, "groups")) < 0) return x.err;
    /* the JS order: walk, base, post, gestures, moods, events, then the walk's surge */
    if (walk_c(&x, cJSON_GetObjectItemCaseSensitive(cr, "walk")) < 0) return x.err;
    if (list_c(&x, cJSON_GetObjectItemCaseSensitive(cr, "base"), &c->base.l) < 0) return x.err;
    if (list_c(&x, cJSON_GetObjectItemCaseSensitive(cr, "post"), &c->post.l) < 0) return x.err;
    const cJSON *gj = cJSON_GetObjectItemCaseSensitive(cr, "gestures"), *mj = cJSON_GetObjectItemCaseSensitive(cr, "moods");
    if (gj) {
        if (!cJSON_IsObject(gj) || cJSON_GetArraySize(gj) > 32) return "creature: gestures";
        c->gest = CP_MALLOC(sizeof(block_t) * 32);
        if (!c->gest) return "oom";
        for (const cJSON *g = gj->child; g; g = g->next) {
            block_t *b = &c->gest[c->ngest];
            if (block_c(&x, g, b, "replace") < 0) return x.err;
            b->act = cute_act_lookup(g->string);   /* an action this firmware lacks: compiled, never played */
            if (b->act >= 0 && b->act < 32) c->gest_of[b->act] = c->ngest;
            c->ngest++;
        }
    }
    if (mj) {
        if (!cJSON_IsObject(mj) || cJSON_GetArraySize(mj) > 16) return "creature: moods";
        for (const cJSON *m = mj->child; m; m = m->next) {
            block_t *b = &c->mood[c->nmood++];
            if (block_c(&x, m, b, "now") < 0) return x.err;
            b->mood = mood_of(m->string);         /* a mood this firmware lacks: never weighted */
        }
    }
    if (events_c(&x, cJSON_GetObjectItemCaseSensitive(cr, "events")) < 0) return x.err;
    const cJSON *walk = cJSON_GetObjectItemCaseSensitive(cr, "walk"), *surge = cJSON_GetObjectItemCaseSensitive(walk, "surge");
    terms_t st;   /* the walk engine's (the device stands still): checked, not played */
    if (surge && terms_c(&x, surge, &st) < 0) return x.err;
    if (tints_c(&x, cJSON_GetObjectItemCaseSensitive(cr, "tints")) < 0 || ambient_c(&x, cJSON_GetObjectItemCaseSensitive(cr, "ambient")) < 0) return x.err;
    /* the mood weights and clocks are kept from frame to frame, like any name starting w. w6. m. */
    for (int s = 0; s < c->nslot; s++) {
        const char *n = c->slot[s].name;
        if (!strncmp(n, "w.", 2) || !strncmp(n, "w6.", 3) || !strncmp(n, "m.", 2)) c->slot[s].keep = 1;
    }
    c->slot[S_T].wide = 1;
    for (int m = 0; m < CP_N_MOODS; m++) c->slot[S_M + m].wide = 1;
    for (int f = 0; f < c->nfac; f++) if (c->fac[f].src == X_CH && c->slot[c->fac[f].si].wide) c->fac[f].src = X_WIDE;
    return x.err;
}

cp_t *cp_create(const char *json, size_t len, const char **err) {
    const char *e = NULL;
    if (!json || len > CP_FILE_MAX) { *err = "too_big"; return NULL; }
    pkg_t *pk = pkg_parse(json, len, &e);   /* the header, as the store checks it */
    if (!pk) { *err = e; return NULL; }
    if (pk->kind != PKG_CREATURE) { pkg_unref(pk); *err = "package: not a creature"; return NULL; }
    cp_t *c = CP_MALLOC(sizeof(cp_t));
    if (!c) { pkg_unref(pk); *err = "oom"; return NULL; }
    memset(c, 0, sizeof(*c));
    strcpy(c->id, pk->id); strcpy(c->name, pk->name); snprintf(c->version, sizeof(c->version), "%s", pk->version);
    pkg_unref(pk);
    cJSON *j = cJSON_ParseWithLength(json, len);
    e = j ? compile(c, cJSON_GetObjectItemCaseSensitive(j, "creature")) : "package: json";
    cJSON_Delete(j);
    if (!e) {
        c->C = CP_MALLOC(sizeof(float) * (size_t)c->nslot); c->Cd = CP_MALLOC(sizeof(double) * (size_t)c->nslot);
        c->st = CP_MALLOC(sizeof(st_t) * (size_t)(c->nst + 1));
        if (!c->C || !c->Cd || !c->st) e = "oom";
    }
    if (e) { cp_destroy(c); *err = e; return NULL; }
    memset(c->C, 0, sizeof(float) * (size_t)c->nslot); memset(c->Cd, 0, sizeof(double) * (size_t)c->nslot);
    memset(c->st, 0, sizeof(st_t) * (size_t)(c->nst + 1));
    for (int m = 0; m < CP_N_MOODS; m++) {
        c->C[S_W + m] = c->C[S_W6 + m] = m ? 0.f : 1.f;
        c->mclock[m] = m ? 99 : 0;
    }
    c->B.w = 1; c->seq = 1;
    *err = NULL;
    return c;
}

void cp_destroy(cp_t *c) {
    if (!c) return;
    for (int g = 0; g < c->ngroup; g++) CP_FREE(c->group[g].attr);
    CP_FREE(c->slot); CP_FREE(c->C); CP_FREE(c->Cd); CP_FREE(c->fac); CP_FREE(c->term); CP_FREE(c->ch); CP_FREE(c->item);
    CP_FREE(c->mix); CP_FREE(c->st); CP_FREE(c->hist); CP_FREE(c->gest); CP_FREE(c->tint);
    CP_FREE(c);
}

const char *cp_id(const cp_t *c) { return c->id; }
const char *cp_name(const cp_t *c) { return c->name; }
const char *cp_version(const cp_t *c) { return c->version; }
const cp_body_t *cp_body(const cp_t *c) { return &c->body; }
int cp_bone_find(const cp_t *c, const char *id) { return bone_of(c, id); }
