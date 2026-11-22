/*
 * SPDX-License-Identifier: MIT
 *
 * sky.c: the sky behind the creature (sky.h): the moon's phase and distance, its cells, the falling star.
 */
#include "sky.h"
#include "sky_moon_map.h"

#include <math.h>
#include <string.h>

#define DEG 0.017453292519943295

static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static float sstep(float a, float b, float x) { const float t = clampf((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t); }
/* dot (col 0..1, row 0..3) -> its braille bit (fluid_sim.h fluid_braille_bit) */
static inline int bbit(int col, int row) { return row < 3 ? col * 3 + row : 6 + col; }

/* ── the moon: Meeus, Astronomical Algorithms (2nd ed. 1998), ch. 47 (the moon) and ch. 25 (the sun, low accuracy) ── */

/* table 47.A, its first rows: D, M, M', F multiples, then the longitude's (1e-6 deg) and the distance's (1e-3 km) terms */
static const struct { int8_t d, m, mp, f; int32_t l, r; } TERMS[] = {
    { 0, 0, 1, 0, 6288774, -20905355 }, { 2, 0, -1, 0, 1274027, -3699111 }, { 2, 0, 0, 0, 658314, -2955968 },
    { 0, 0, 2, 0, 213618, -569925 },    { 0, 1, 0, 0, -185116, 48888 },     { 0, 0, 0, 2, -114332, -3149 },
    { 2, 0, -2, 0, 58793, 246158 },     { 2, -1, -1, 0, 57066, -152138 },   { 2, 0, 1, 0, 53322, -170733 },
    { 2, -1, 0, 0, 45758, -204586 },    { 0, 1, -1, 0, -40923, -129620 },   { 1, 0, 0, 0, -34720, 108743 },
    { 0, 1, 1, 0, -30383, 104755 },     { 2, 0, 0, -2, 15327, 10321 },      { 0, 0, 1, 2, -12528, 0 },
    { 0, 0, 1, -2, 10980, 79661 },      { 4, 0, -1, 0, 10675, -34782 },     { 0, 0, 3, 0, 10034, -23210 },
    { 4, 0, -2, 0, 8548, -21636 },      { 2, 1, -1, 0, -7888, 24208 },      { 2, 1, 0, 0, -6766, 30824 },
    { 1, 0, -1, 0, -5163, -8379 },      { 1, 1, 0, 0, 4987, -16675 },       { 2, -1, 1, 0, 4036, -12831 },
    { 2, 0, 2, 0, 3994, -10445 },       { 4, 0, 0, 0, 3861, -11650 },       { 2, 0, -3, 0, 3665, 14403 },
    { 0, 1, -2, 0, -2689, -7003 },
};

sky_moon_t sky_moon_at(double t) {
    const double T = (t / 86400.0 + 2440587.5 - 2451545.0) / 36525.0;   /* Julian centuries from J2000 (UT for TT: ~1 min) */
    const double Lp = 218.3164477 + 481267.88123421 * T - 0.0015786 * T * T;
    const double D  = 297.8501921 + 445267.1114034 * T - 0.0018819 * T * T;
    const double M  = 357.5291092 + 35999.0502909 * T - 0.0001536 * T * T;
    const double Mp = 134.9633964 + 477198.8675055 * T + 0.0087414 * T * T;
    const double F  = 93.2720950 + 483202.0175233 * T - 0.0036539 * T * T;
    const double E  = 1.0 - 0.002516 * T - 0.0000074 * T * T;          /* the earth's orbit's eccentricity on M's terms */
    double sl = 0.0, sr = 0.0;
    for (size_t q = 0; q < sizeof(TERMS) / sizeof(TERMS[0]); q++) {
        const double a = (TERMS[q].d * D + TERMS[q].m * M + TERMS[q].mp * Mp + TERMS[q].f * F) * DEG;
        const double e = TERMS[q].m == 0 ? 1.0 : TERMS[q].m == 1 || TERMS[q].m == -1 ? E : E * E;
        sl += TERMS[q].l * e * sin(a);
        sr += TERMS[q].r * e * cos(a);
    }
    const double lmoon = Lp + sl * 1e-6;
    const double C = (1.914602 - 0.004817 * T) * sin(M * DEG) + (0.019993 - 0.000101 * T) * sin(2 * M * DEG) + 0.000289 * sin(3 * M * DEG);
    const double lsun = 280.46646 + 36000.76983 * T + C;
    double el = fmod(lmoon - lsun, 360.0);
    if (el < 0) el += 360.0;
    sky_moon_t m;
    m.elong = el;
    m.lit = (float)((1.0 - cos(el * DEG)) * 0.5);
    m.waxing = el < 180.0;
    m.age = (float)(el / 360.0 * 29.530588853);
    m.km = (float)(385000.56 + sr * 1e-3);
    return m;
}

float sky_size_k(float km) {
    static const float K[SKY_SIZE_N] = SKY_SIZE_KM, V[SKY_SIZE_N] = SKY_SIZE_K;
    if (km <= K[0]) return V[0];
    for (int q = 1; q < SKY_SIZE_N; q++)
        if (km <= K[q]) return V[q - 1] + (V[q] - V[q - 1]) * (km - K[q - 1]) / (K[q] - K[q - 1]);
    return V[SKY_SIZE_N - 1];
}

int sky_moon_rdots(float km, int W) { return (int)lrintf(sky_size_k(km) * SKY_MOON_R * 2.f * (float)W); }

static int map_at(float nx, float ny) {     /* the face's grey 0..15 at (nx, ny) on the unit disc, north up */
    const int i = (int)((nx * 0.5f + 0.5f) * SKY_MAP_N), j = (int)((0.5f - ny * 0.5f) * SKY_MAP_N);
    if (i < 0 || j < 0 || i >= SKY_MAP_N || j >= SKY_MAP_N) return 0;
    const int k = j * SKY_MAP_N + i;
    return k & 1 ? SKY_MOON_MAP[k >> 1] >> 4 : SKY_MOON_MAP[k >> 1] & 15;
}

/* a cell's dots in a fixed order of thresholds (2 x 4 ordered dither): the terminator's soft edge thins out evenly */
static const uint8_t ORDER[2][4] = { { 0, 6, 2, 4 }, { 5, 3, 7, 1 } };

int sky_moon_cells(const sky_moon_t *m, int W, int H, sky_cell_t *out, int cap, int *rdots) {
    const int rd = sky_moon_rdots(m->km, W);
    if (rdots) *rdots = rd;
    if (m->lit < SKY_LIT_MIN || rd <= 0) return 0;
    const int DW = 2 * W, DH = 4 * H;
    const float r = (float)rd / DW;                         /* stage fraction (the stage is square) */
    const float c = 2.f * m->lit - 1.f, side = m->waxing ? 1.f : -1.f, lx = side * sqrtf(fmaxf(0.f, 1.f - c * c)), lz = c;
    const int i0 = (int)floorf((SKY_MOON_X - r) * W) - 1, i1 = (int)floorf((SKY_MOON_X + r) * W) + 1;
    const int j0 = (int)floorf((SKY_MOON_Y - r) * H) - 1, j1 = (int)floorf((SKY_MOON_Y + r) * H) + 1;
    int n = 0;
    for (int j = j0 < 0 ? 0 : j0; j <= j1 && j < H; j++) for (int i = i0 < 0 ? 0 : i0; i <= i1 && i < W; i++) {
        uint8_t bits = 0;
        float lsum = 0.f; int on = 0;
        for (int col = 0; col < 2; col++) for (int row = 0; row < 4; row++) {
            const float nx = (((float)(2 * i + col) + 0.5f) / DW - SKY_MOON_X) / r, ny = -((((float)(4 * j + row) + 0.5f) / DH - SKY_MOON_Y) / r);
            const float rr = nx * nx + ny * ny;
            if (rr > 1.f) continue;
            const float L = sstep(-0.08f, 0.12f, nx * lx + sqrtf(1.f - rr) * lz);
            if (L * 8.f <= (float)ORDER[col][row] + 0.5f) continue;
            bits |= (uint8_t)(1u << bbit(col, row)); lsum += L; on++;
        }
        if (!bits || n >= cap) continue;
        /* its colour: the face's grey at the cell's middle (pulled onto the disc), lit as the globe scene lights it */
        float nx = (((float)(2 * i) + 1.f) / DW - SKY_MOON_X) / r, ny = -((((float)(4 * j) + 2.f) / DH - SKY_MOON_Y) / r);
        const float rr = nx * nx + ny * ny;
        if (rr > 0.9f) { const float k = 0.94f / sqrtf(rr); nx *= k; ny *= k; }
        const float g = (float)map_at(nx, ny) / 15.f * (0.18f + 0.82f * lsum / on) * SKY_GAIN * 255.f;
        const uint32_t R = (uint32_t)lrintf(g), B = (uint32_t)lrintf(g * 0.975f);
        out[n++] = (sky_cell_t){ .k = (uint16_t)(j * W + i), .bits = bits, .rgb = R << 16 | R << 8 | B };
    }
    return n;
}

/* ── the falling star ─────────────────────────────────────────────────────── */

static uint32_t rnd(sky_star_t *st) { uint32_t x = st->rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return st->rng = x; }
static float frnd(sky_star_t *st, float a, float b) { return a + (b - a) * (float)(rnd(st) >> 8) * (1.f / 16777216.f); }
static float gap(sky_star_t *st) { return frnd(st, SKY_STAR_GAP_MIN_S, SKY_STAR_GAP_MAX_S); }

/* the segment (x0, y0) + s (ux, uy), s in [0, len], keeps d from (px, py)? */
static bool clear_of(float x0, float y0, float ux, float uy, float len, float px, float py, float d) {
    float s = (px - x0) * ux + (py - y0) * uy;
    s = clampf(s, 0.f, len);
    const float dx = x0 + s * ux - px, dy = y0 + s * uy - py;
    return dx * dx + dy * dy > d * d;
}
#define SKY_STAR_IN 0.45f                         /* its path inside this much of the glass (the world's dust fades past it) */
static bool inside(float x, float y, float r) { return (x - 0.5f) * (x - 0.5f) + (y - 0.5f) * (y - 0.5f) < r * r; }

/* a new path: from the upper sky down at a slant, left or right of the cat (its column clear), inside the glass, above
 * the horizon, clear of the largest moon, and not the last one's again */
static void pick(sky_star_t *st) {
    for (int tries = 0; tries < 64; tries++) {
        const bool right = rnd(st) & 1;
        const float y0 = frnd(st, 0.10f, 0.26f), edge = 0.5f - sqrtf(SKY_STAR_IN * SKY_STAR_IN - (y0 - 0.5f) * (y0 - 0.5f));   /* the glass there */
        const float x0 = right ? frnd(st, 0.68f, 1.f - edge) : frnd(st, edge, 0.32f);
        const float a = frnd(st, 22.f, 55.f) * (float)DEG, dir = (rnd(st) & 1) ? 1.f : -1.f, len = frnd(st, SKY_STAR_LEN_MIN, SKY_STAR_LEN_MAX);
        const float ux = dir * cosf(a), uy = sinf(a), x1 = x0 + len * ux, y1 = y0 + len * uy;
        const bool ok = y1 <= 0.40f && inside(x0, y0, SKY_STAR_IN + 1e-4f) && inside(x1, y1, SKY_STAR_IN) &&
                        (right ? x1 >= 0.66f && x1 <= 0.94f : x1 >= 0.06f && x1 <= 0.34f) &&
                        clear_of(x0, y0, ux, uy, len, SKY_MOON_X, SKY_MOON_Y, SKY_MOON_R + 0.03f) &&
                        (st->n == 0 || fabsf(x0 - st->px0) + fabsf(y0 - st->py0) > 0.08f || (ux > 0.f) != (st->pux > 0.f));
        if (!ok) continue;
        st->x0 = x0; st->y0 = y0; st->ux = ux; st->uy = uy; st->len = len;
        st->dur = frnd(st, SKY_STAR_DUR_MIN_S, SKY_STAR_DUR_MAX_S);
        st->t = 0.f; st->n++;
        st->px0 = x0; st->py0 = y0; st->pux = ux;
        return;
    }
    /* (none found in so many, 1 in 5000: a short one high on the left, where one always fits, the other way from the
     * last one) */
    const bool left = st->n == 0 || st->pux > 0.f;
    st->x0 = left ? frnd(st, 0.28f, 0.31f) : frnd(st, 0.20f, 0.22f); st->y0 = frnd(st, 0.17f, 0.19f);
    const float a = frnd(st, 30.f, 40.f) * (float)DEG;
    st->ux = left ? -cosf(a) : cosf(a); st->uy = sinf(a); st->len = SKY_STAR_LEN_MIN * 0.75f;
    st->dur = frnd(st, SKY_STAR_DUR_MIN_S, SKY_STAR_DUR_MAX_S);
    st->t = 0.f; st->n++;
    st->px0 = st->x0; st->py0 = st->y0; st->pux = st->ux;
}

static uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = a * 0x9E3779B1u ^ b * 0x85EBCA77u ^ c * 0xC2B2AE3Du;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}

/* this frame's streak as cells: each point of its path lit by when the head passed it (a flare, then out at the end),
 * fading behind the head as it runs; once the head is out the trail left fades whole over SKY_STAR_FADE_S; a dot shows when that beats its own threshold, so the old trail breaks into sparks */
static int star_cells(sky_t *sk, int W, int H) {
    const sky_star_t *st = &sk->star;
    sky_cell_t *out = sk->star_c;
    const int DW = 2 * W, DH = 4 * H;
    const float t = st->t, head = st->len * clampf(t / st->dur, 0.f, 1.f);
    const float out_k = t <= st->dur ? 1.f : 1.f - clampf((t - st->dur) / SKY_STAR_FADE_S, 0.f, 1.f);
    sky_dot_t *dot = sk->sdot;
    int nd = 0;
    const float step = 0.35f / DW;
    for (float s = 0.f; s <= head; s += step) {
        const float f = s / st->len, tp = st->dur * f;
        const float b = SKY_STAR_HEAD * sstep(0.f, 0.12f, f) * (1.f - 0.75f * sstep(0.55f, 1.f, f)) * expf(-(fminf(t, st->dur) - tp) / SKY_STAR_TAU_S) * out_k;
        if (b < 0.04f) continue;
        const int X = (int)((st->x0 + s * st->ux) * DW), Y = (int)((st->y0 + s * st->uy) * DH);
        int q = nd - 1;
        while (q >= 0 && q >= nd - 3 && !(dot[q].X == X && dot[q].Y == Y)) q--;
        if (q >= 0 && q >= nd - 3) { if (b > dot[q].b) dot[q].b = b; continue; }
        if (nd < SKY_STAR_DOTS) dot[nd++] = (sky_dot_t){ (int16_t)X, (int16_t)Y, b };
    }
    int n = 0;
    for (int q = 0; q < nd; q++) {
        const float thr = 0.05f + 0.3f * (float)(hash3((uint32_t)dot[q].X, (uint32_t)dot[q].Y, (uint32_t)st->n) >> 8) * (1.f / 16777216.f);
        if (dot[q].b < thr || dot[q].X < 0 || dot[q].Y < 0 || dot[q].X >= DW || dot[q].Y >= DH) continue;
        const uint16_t k = (uint16_t)((dot[q].Y >> 2) * W + (dot[q].X >> 1));
        const uint8_t bit = (uint8_t)(1u << bbit(dot[q].X & 1, dot[q].Y & 3));
        int c = n - 1;
        while (c >= 0 && out[c].k != k) c--;
        if (c < 0) {
            if (n >= SKY_STAR_CELLS) continue;
            c = n++;
            out[c] = (sky_cell_t){ .k = k };
        }
        out[c].bits |= bit;
        const float b = dot[q].b;
        const uint32_t was = out[c].rgb, l = (uint32_t)lrintf(b * 255.f);
        if (l > (was >> 24)) {                              /* (the brightest dot's level, kept in the top byte while made) */
            const uint32_t R = (uint32_t)lrintf((SKY_STAR_RGB >> 16 & 255) * b), G = (uint32_t)lrintf((SKY_STAR_RGB >> 8 & 255) * b), B = (uint32_t)lrintf((SKY_STAR_RGB & 255) * b);
            out[c].rgb = l << 24 | R << 16 | G << 8 | B;
        }
    }
    for (int c = 0; c < n; c++) out[c].rgb &= 0xffffffu;
    return n;
}

/* ── the sky ──────────────────────────────────────────────────────────────── */

void sky_init(sky_t *s, uint32_t seed) {
    memset(s, 0, sizeof(*s));
    s->on = true;
    s->day = -1;
    uint32_t x = seed ^ 0x736b7931u;                        /* ("sky1": its own stream, never the creature's) */
    x = hash3(x, 0x2545F491u, 7u);
    s->star.rng = x ? x : 0x736b7931u;
    s->star.t = -1.f;
    s->star.wait = gap(&s->star);
}

void sky_set(sky_t *s, bool on) {
    if (s->on == on) return;
    s->on = on;
    s->day = -1;
    s->nmoon = s->nstar = 0;
    s->star.t = -1.f;
}

void sky_frame(sky_t *s, double utc, int W, int H, float dt, bool moon, bool star) {
    s->nstar = s->nmoon_shown = 0;
    s->show = false;
    if (!s->on || utc <= 0.0) return;
    const int64_t day = (int64_t)floor(utc / 86400.0);
    if (day != s->day || W != s->W || H != s->H) {          /* once a day (and for a new grid): the moon then, at noon */
        s->day = day; s->W = W; s->H = H;
        s->moon = sky_moon_at((double)day * 86400.0 + 43200.0);
        s->nmoon = sky_moon_cells(&s->moon, W, H, s->moon_c, SKY_MOON_CELLS, &s->rdots);
    }
    sky_star_t *st = &s->star;
    if (!star) st->t = -1.f;                                /* (one showing goes out at once) */
    else if (st->t < 0.f) {
        st->wait -= dt;
        if (st->wait <= 0.f) pick(st);
    } else if ((st->t += dt) > st->dur + SKY_STAR_FADE_S) {
        st->t = -1.f;
        st->wait = gap(st);
    }
    if (star && st->t >= 0.f) s->nstar = star_cells(s, W, H);
    s->nmoon_shown = moon ? s->nmoon : 0;
    s->show = s->nmoon_shown > 0 || s->nstar > 0;
}
