/*
 * SPDX-License-Identifier: MIT
 *
 * weather.c: the weather on the glass (weather.h): what was told and its easing, the streaks, the rings, the drops on
 * the glass, the wet floor's puddles. The looks follow the rain options page (A drizzle, B rain, C downpour thinned to
 * fit the frame, D's drops on the glass, E's moonlit rain).
 */
#include "weather.h"

#include <string.h>

const char *const WX_NAMES[WX_KINDS] = { "clear", "drizzle", "rain", "downpour" };

/* a kind's row; the level's look is the straight line between two rows. Rates a second: drops that land on the floor
 * (each a bump, a splash, a ring), streaks that end in the distance, streaks in front of the cat. The downpour is the
 * page's C thinned to fit the frame: 30 drops landing (not 45), 60 % of them bumping the water, 100 far streaks (not
 * 160). */
typedef struct { float land, far, front, slant, len, bri, bumpk, beads, runner; } wx_look_t;
static const wx_look_t LOOK[WX_KINDS] = {
    /*            land  far  front slant  len     bri    bumpk beads runner/s */
    [WX_CLEAR]    = { 0.f,  0.f,  0.f, 0.05f, 0.036f, 0.75f, 1.f,  0.f,  0.f },
    [WX_DRIZZLE]  = { 5.f,  10.f, 0.f, 0.05f, 0.036f, 0.75f, 1.f,  5.f,  1.f / 40.f },
    [WX_RAIN]     = { 12.f, 30.f, 0.f, 0.14f, 0.08f,  0.80f, 1.f,  10.f,  1.f / 14.f },
    [WX_DOWNPOUR] = { 12.f, 20.f, 8.f,  0.36f, 0.11f,  0.80f, 0.6f, 13.f, 1.f / 9.f },
};
#define WX_MOON_FAR     60.f                /* moonlit: this many more far streaks a second, all in the moon's light     */
#define WX_DROP_S       0.12f               /* a drop's bump: fluid_sim_impulse's strength (a light tap's 0.15 x 0.8)     */
#define WX_DROP_R       0.045f              /* ... and radius                                                             */
#define WX_RING_LIFE    0.53f               /* s a ring spreads and fades                                                 */
#define WX_SPLASH       (2.f / 30.f)        /* s a splash shows                                                           */
#define WX_STAGE_PX     466.f               /* the page's px (the looks were tuned on the 466 px glass)                    */
#define WX_BRI_MAX      0.8f                /* a streak's dot at most this of RAIN: luma ~150, the cat's eyes ~205        */
#define WX_GLASS_R      0.485f              /* the glass's radius, stage                                                  */
#define WX_SPAWN_DT     0.1f                /* the longest step for the falling (the time the screen was off: clocks only) */
/* the face: drops on the glass keep off its place (stage; the cat's head where it sits) */
#define WX_FACE_X 0.5f
#define WX_FACE_Y 0.29f
#define WX_FACE_RX 0.21f
#define WX_FACE_RY 0.17f

static const uint32_t RAIN = 0xb2bce4u, RING = 0xa5afe6u, GLASS = 0x96a0c8u, SHINE = 0xc8cce4u;

static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static float fminf_(float a, float b) { return a < b ? a : b; }
static float fmaxf_(float a, float b) { return a > b ? a : b; }
static float sstep(float a, float b, float x) { const float t = clampf((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t); }
static int ifloor(float x) { const int i = (int)x; return i - (x < (float)i); }
static inline int bbit(int col, int row) { return row < 3 ? col * 3 + row : 6 + col; }
/* sin of x radians (|x| < 8), a polynomial: no maths library in a frame (the native and WebAssembly builds then agree
 * to the bit) */
static float wsin(float x) {
    const float PI = 3.14159265f;
    while (x > PI) x -= 2.f * PI;
    while (x < -PI) x += 2.f * PI;
    if (x > 0.5f * PI) x = PI - x;
    else if (x < -0.5f * PI) x = -PI - x;
    const float x2 = x * x;
    return x * (1.f - x2 / 6.f * (1.f - x2 / 20.f * (1.f - x2 / 42.f)));
}
static float wcos(float x) { return wsin(x + 1.57079633f); }

static uint32_t rnd(weather_t *w) { uint32_t x = w->rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return w->rng = x; }
static float frnd(weather_t *w, float a, float b) { return a + (b - a) * (float)(rnd(w) >> 8) * (1.f / 16777216.f); }
static uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = a * 0x9E3779B1u ^ b * 0x85EBCA77u ^ c * 0xC2B2AE3Du;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
static float hashf(int a, int b, int c) { return (float)(hash3((uint32_t)a, (uint32_t)b, (uint32_t)c) >> 8) * (1.f / 16777216.f); }

static wx_look_t look_at(float level) {
    const int a = level <= 0.f ? 0 : level >= 3.f ? 2 : (int)level;
    const float t = clampf(level - (float)a, 0.f, 1.f);
    const float *p = &LOOK[a].land, *q = &LOOK[a + 1].land;
    wx_look_t o; float *r = &o.land;
    for (size_t i = 0; i < sizeof(wx_look_t) / sizeof(float); i++) r[i] = p[i] + (q[i] - p[i]) * t;
    return o;
}
static bool in_glass(float x, float y, float r) { return (x - 0.5f) * (x - 0.5f) + (y - 0.5f) * (y - 0.5f) < r * r; }
static bool in_face(float x, float y) {
    const float a = (x - WX_FACE_X) / WX_FACE_RX, b = (y - WX_FACE_Y) / WX_FACE_RY;
    return a * a + b * b < 1.f;
}

/* ── the floor's puddles: a value noise laid in perspective on the floor (the page's), made once a grid ─────────── */

static float vnoise(float x, float y) {
    const int i = ifloor(x), j = ifloor(y);
    const float fx = sstep(0.f, 1.f, x - (float)i), fy = sstep(0.f, 1.f, y - (float)j);
    const float a = hashf(i, j, 5), b = hashf(i + 1, j, 5), c = hashf(i, j + 1, 5), d = hashf(i + 1, j + 1, 5);
    return (a * (1.f - fx) + b * fx) * (1.f - fy) + (c * (1.f - fx) + d * fx) * fy;
}
#define WX_HORIZON 0.495f                   /* creature_dots.h CD_HORIZON_AT */
static void puddles(weather_t *w, int W, int H) {
    w->W = W; w->H = H;
    const float H0 = WX_HORIZON * WX_STAGE_PX;
    for (int j = 0; j < H; j++) for (int i = 0; i < W && j * W + i < WX_GRID_MAX; i++) {
        const float x = ((float)i + 0.5f) / (float)W * WX_STAGE_PX, y = ((float)j + 0.5f) / (float)H * WX_STAGE_PX;
        float p = 0.f;
        if (y > H0) {
            const float u = (x - WX_STAGE_PX * 0.5f) / (0.25f + (y - H0) / 60.f) / 40.f, v = (y - H0) / 14.f;
            p = sstep(0.f, 1.f, clampf((vnoise(u + 3.1f, v) - 0.3f) * 2.2f, 0.f, 1.f));
        }
        w->puddle[j * W + i] = (uint8_t)(p * 255.f + 0.5f);
    }
}

/* ── told ─────────────────────────────────────────────────────────────────── */

void weather_init(weather_t *w, uint32_t seed) {
    memset(w, 0, sizeof(*w));
    w->on = true;
    w->kind = WX_CLEAR;
    w->shake_wait = -1.f;
    uint32_t x = hash3(seed ^ 0x77783175u, 0x2545F491u, 11u);  /* ("wx1u": its own stream, never the sky's or the cat's) */
    w->rng = x ? x : 0x77783175u;
}

void weather_set(weather_t *w, bool on) {
    if (w->on == on) return;
    w->on = on;
    w->kind = WX_CLEAR; w->left = 0.f; w->level = w->wet = 0.f;
    w->nstreak = w->nring = w->nbead = 0;
    w->due_land = w->due_far = w->due_front = w->due_bead = 0.f;
    w->shake = false; w->shake_wait = -1.f;
}

void weather_tell(weather_t *w, int kind, float life) {
    if (kind < 0 || kind >= WX_KINDS) return;
    w->kind = kind;
    w->left = kind == WX_CLEAR ? 0.f : clampf(life > 0.f ? life : WX_LIFE_DEF_S, WX_LIFE_MIN_S, WX_LIFE_MAX_S);
}

int weather_kind(const char *name) {
    for (int k = 0; name && k < WX_KINDS; k++) if (!strcmp(name, WX_NAMES[k])) return k;
    return -1;
}

void weather_shook(weather_t *w) { w->shake = false; w->shake_wait = frnd(w, WX_SHAKE_GAP_MIN_S, WX_SHAKE_GAP_MAX_S); }

/* ── the falling ──────────────────────────────────────────────────────────── */

/* the moon's light on the rain at (x, y): a soft beam from the moon down to the floor, leaning left (the page's E) */
static float beam(float x, float y) {
    if (y < SKY_MOON_Y + SKY_MOON_R) return 0.f;   /* (from under it: the rain passes behind the moon) */
    const float t = clampf((y - SKY_MOON_Y) / (1.f - SKY_MOON_Y), 0.f, 1.f), cx = SKY_MOON_X - 0.107f * t, wd = 0.097f + 0.236f * t;
    const float d = (x - cx) / wd;
    return d * d >= 1.f ? 0.f : 1.f - d * d;
}
static float moonlight(const weather_t *w, float x, float y) { return w->moonlit <= 0.f ? 1.f : (1.f - w->moonlit) + w->moonlit * 1.9f * beam(x, y); }

enum { SP_LAND, SP_FAR, SP_FRONT };
static void spawn(weather_t *w, const wx_look_t *k, int what, bool in_beam) {
    if (w->nstreak >= WX_STREAKS) return;
    wx_streak_t *s = &w->streak[w->nstreak];
    float x = 0.5f, y = 0.5f, z = 0.5f;
    for (int tries = 0; tries < 8; tries++) {
        const float r = frnd(w, 0.f, 1.f);
        if (what == SP_FAR) {
            y = 0.26f + 0.26f * r; z = 0.25f + 0.4f * frnd(w, 0.f, 1.f);
            x = in_beam ? SKY_MOON_X - 0.107f * (y - SKY_MOON_Y) / (1.f - SKY_MOON_Y) + frnd(w, -0.5f, 0.5f) * (0.097f + 0.236f * (y - SKY_MOON_Y) / (1.f - SKY_MOON_Y))
                        : frnd(w, 0.03f, 0.97f);
        } else if (what == SP_FRONT) {
            y = frnd(w, 0.86f, 0.95f); z = 1.f; x = frnd(w, 0.1f, 0.9f);
        } else {
            y = WX_HORIZON + 0.017f + (0.975f - WX_HORIZON - 0.017f) * r * (0.6f + 0.4f * r);   /* (more near the horizon, as seen) */
            z = clampf((y - WX_HORIZON) / (1.f - WX_HORIZON), 0.f, 1.f); x = frnd(w, 0.03f, 0.97f);
        }
        if (in_glass(x, y, WX_GLASS_R - 0.02f)) break;
    }
    if (!in_glass(x, y, WX_GLASS_R - 0.02f)) return;
    const float sl = k->slant, ux = wsin(sl), uy = wcos(sl);
    s->v = 0.9f + 1.6f * z;
    s->len = k->len * (what == SP_FRONT ? 1.5f : 0.45f + 0.75f * z) * frnd(w, 0.8f, 1.2f);   /* (in front: the nearest, longest) */
    s->b = 0.65f + 0.35f * z;
    s->z = z;
    s->lands = what != SP_FAR; s->front = what == SP_FRONT;
    s->bump = what == SP_LAND && frnd(w, 0.f, 1.f) < k->bumpk;   /* (in front: a ring, no bump: the water's budget) */
    const float y0 = -frnd(w, 0.f, 0.2f), L = (y - y0) / uy;   /* from above the glass, through the sky, to its end */
    s->x0 = x - ux * L; s->y0 = y0; s->ux = ux; s->uy = uy;
    s->t = s->td = 0.f; s->T = L / s->v;
    s->ph = (uint8_t)(w->nstreak % WX_STREAK_EVERY);
    w->nstreak++;
}

static void land(weather_t *w, const wx_streak_t *s) {
    const float x = s->x0 + s->ux * s->v * s->T, y = s->y0 + s->uy * s->v * s->T;
    if (w->nring < WX_RINGS) w->ring[w->nring++] = (wx_ring_t){ x, y, s->z, 0.f, 0.f, (uint8_t)(w->nring & 1), (uint8_t)s->front };   /* (in front: a splash alone) */
    if (s->bump && w->nbump < WX_BUMPS)
        w->bump[w->nbump++] = (wx_bump_t){ x * 2.f - 1.f, y * 2.f - 1.f, WX_DROP_S * (0.5f + 0.5f * s->z), WX_DROP_R };
}

/* the drops on the glass: a few sit (fading in and out), now and then one runs down leaving a trail */
static void beads(weather_t *w, const wx_look_t *k, float dt, float age) {
    for (int q = 0; q < w->nbead; q++) {
        wx_bead_t *b = &w->bead[q];
        b->age += age;                                      /* (by the clock: the screen off, they dried meanwhile) */
        if (b->vy > 0.f) { b->vy = clampf(b->vy + 0.22f * dt, 0.f, 0.14f); b->y += b->vy * dt; }
        const bool gone = b->age >= b->life + (b->vy > 0.f ? 10.f : 0.f) ||
                          (b->vy > 0.f && (b->y > b->y0 + 0.34f || !in_glass(b->x, b->y, WX_GLASS_R - 0.03f)));
        if (gone) { w->bead[q--] = w->bead[--w->nbead]; continue; }
    }
    w->due_bead += dt / 1.5f;
    if (w->due_bead > 1.f) w->due_bead = 1.f;
    if ((float)w->nbead < k->beads && w->due_bead >= 1.f && w->nbead < WX_BEADS) {
        w->due_bead = 0.f;
        for (int tries = 0; tries < 12; tries++) {
            const float x = frnd(w, 0.07f, 0.93f), y = frnd(w, 0.07f, 0.93f);
            if (!in_glass(x, y, WX_GLASS_R - 0.05f) || in_face(x, y)) continue;
            bool near = false;
            for (int q = 0; q < w->nbead && !near; q++) near = (w->bead[q].x - x) * (w->bead[q].x - x) + (w->bead[q].y - y) * (w->bead[q].y - y) < 0.006f;
            if (near) continue;
            const float r = frnd(w, 0.f, 1.f);
            w->bead[w->nbead++] = (wx_bead_t){ x, y, 0.f, frnd(w, 25.f, 70.f), y, 0.f, (uint8_t)(r < 0.15f ? 0 : r < 0.6f ? 1 : 2) };
            break;
        }
    }
    if (k->runner <= 0.f) return;
    if (w->runner_wait <= 0.f) w->runner_wait = frnd(w, 0.6f, 1.4f) / k->runner;
    if ((w->runner_wait -= dt) > 0.f) return;
    w->runner_wait = frnd(w, 0.6f, 1.4f) / k->runner;
    for (int q = 0; q < w->nbead; q++) {                    /* a grown one away from the cat's column starts to run */
        wx_bead_t *b = &w->bead[q];
        if (b->vy > 0.f || b->size == 0 || b->age < 2.f || (b->x > 0.3f && b->x < 0.7f)) continue;
        b->vy = 0.01f; b->y0 = b->y; b->size = 2;
        break;
    }
}

/* ── its cells ────────────────────────────────────────────────────────────── */

typedef struct { sky_cell_t *c; int n, cap; } layer_t;
static void put(weather_t *w, layer_t *L, int X, int Y, float b, uint32_t rgb) {
    const int W = w->W, H = w->H;
    if (b < 0.04f || X < 0 || Y < 0 || X >= 2 * W || Y >= 4 * H) return;
    if (b > 1.f) b = 1.f;
    const int k = (Y >> 2) * W + (X >> 1);
    int c = (int)w->at[k] - 1;
    if (c < 0) {
        if (L->n >= L->cap) return;
        c = L->n++;
        w->at[k] = (uint16_t)(c + 1);
        L->c[c] = (sky_cell_t){ .k = (uint16_t)k };
        w->lvl[k] = 0;
    }
    L->c[c].bits |= (uint8_t)(1u << bbit(X & 1, Y & 3));
    const int l = (int)(b * 255.f + 0.5f);
    if (l > w->lvl[k]) {
        w->lvl[k] = (uint8_t)l;
        const uint32_t R = (uint32_t)((float)(rgb >> 16 & 255) * b + 0.5f), G = (uint32_t)((float)(rgb >> 8 & 255) * b + 0.5f), B = (uint32_t)((float)(rgb & 255) * b + 0.5f);
        L->c[c].rgb = R << 16 | G << 8 | B;
    }
}
static void done(weather_t *w, const layer_t *L) { for (int c = 0; c < L->n; c++) w->at[L->c[c].k] = 0; }

static void draw_streak(weather_t *w, layer_t *L, const wx_streak_t *s, float bri) {
    const int DW = 2 * w->W, DH = 4 * w->H;
    const float hx = s->x0 + s->ux * s->v * s->td, hy = s->y0 + s->uy * s->v * s->td, yend = s->y0 + s->uy * s->v * s->T;
    /* a dot a row; slanted more, a dot every other row: on the lattice a slant reads as a dotted line, not as steps */
    const int every = 1;
    const float sy = (float)every / (float)DH, sx = s->ux / s->uy * sy;
    const int n = (int)(s->len * (float)DH) / every + 1;
    const float mr2 = (SKY_MOON_R + 0.01f) * (SKY_MOON_R + 0.01f);
    for (int q = 0; q < n; q++) {
        const float x = hx - sx * (float)q, y = hy - sy * (float)q;
        if (y >= yend || y < 0.f) continue;
        if ((x - SKY_MOON_X) * (x - SKY_MOON_X) + (y - SKY_MOON_Y) * (y - SKY_MOON_Y) < mr2) continue;   /* behind the moon */
        put(w, L, (int)(x * (float)DW), (int)(y * (float)DH), fminf_(WX_BRI_MAX, bri * s->b * (1.f - 0.75f * (float)q / (float)n) * moonlight(w, x, y)), RAIN);
    }
}

static void draw_ring(weather_t *w, layer_t *L, const wx_ring_t *r, float bri) {
    const int DW = 2 * w->W, DH = 4 * w->H;
    const int X = (int)(r->x * (float)DW), Y = (int)(r->y * (float)DH);
    if (r->ad < WX_SPLASH) {                                   /* the splash: a dot either side, rising, then wider */
        const int a = r->ad < WX_SPLASH * 0.5f ? 1 : 2;
        const float b = bri * (0.55f + 0.45f * r->z) * (a == 1 ? 0.9f : 0.5f) * moonlight(w, r->x, r->y);
        put(w, L, X - a, Y - a, b, RAIN); put(w, L, X + a, Y - a, b, RAIN);
    }
    const float a = r->ad * 30.f;                             /* (the page's frames) */
    if (a < 1.f) return;
    const float rx = (3.f + 1.9f * a) * (0.55f + 0.6f * r->z) / WX_STAGE_PX, ry = rx * (0.28f + 0.2f * r->z);
    const float life = 1.f - r->ad / WX_RING_LIFE, base = 0.8f * life * life * (0.55f + 0.45f * r->z);
    if (base < 0.04f) return;
    const float per = 6.2831853f * (rx * WX_STAGE_PX * 0.75f + ry * WX_STAGE_PX * 0.25f) / 3.6f;   /* a dot about every 3.6 px */
    const int n = per < 6.f ? 6 : (int)per;
    const float da = 6.2831853f / (float)n, cd = wcos(da), sd = wsin(da);
    float c = 1.f, s = 0.f;
    for (int q = 0; q < n; q++) {
        const float x = r->x + rx * c, y = r->y + ry * s;
        if (y > WX_HORIZON + 0.005f) put(w, L, (int)(x * (float)DW), (int)(y * (float)DH), base * (s > 0.f ? 1.f : 0.7f) * fmaxf_(0.45f, moonlight(w, x, y)), RING);
        const float c2 = c * cd - s * sd; s = s * cd + c * sd; c = c2;
    }
}

static void draw_bead(weather_t *w, layer_t *L, const wx_bead_t *b) {
    const int DW = 2 * w->W, DH = 4 * w->H;
    const float fade = b->vy > 0.f ? clampf((b->y0 + 0.34f - b->y) / 0.08f, 0.f, 1.f) : clampf(b->age, 0.f, 1.f) * clampf((b->life - b->age) * 0.5f, 0.f, 1.f);
    if (fade <= 0.f) return;
    const int X = (int)(b->x * (float)DW), Y = (int)(b->y * (float)DH);
    /* a lens, not a bubble: a bright point up and to the left, the rim's light gathered low, nothing at the top */
    if (b->size == 0) { put(w, L, X, Y, 0.55f * fade, GLASS); }
    else if (b->size == 1) {
        put(w, L, X - 1, Y - 1, 0.7f * fade, SHINE);
        put(w, L, X - 2, Y + 1, 0.45f * fade, GLASS); put(w, L, X - 1, Y + 2, 0.7f * fade, GLASS); put(w, L, X, Y + 2, 0.7f * fade, GLASS); put(w, L, X + 1, Y + 1, 0.45f * fade, GLASS);
    } else {                                                  /* (a big one: its sides too, low, open at the top) */
        put(w, L, X - 1, Y - 2, 0.75f * fade, SHINE);
        put(w, L, X - 3, Y, 0.35f * fade, GLASS); put(w, L, X + 2, Y, 0.35f * fade, GLASS);
        put(w, L, X - 3, Y + 1, 0.5f * fade, GLASS); put(w, L, X + 2, Y + 1, 0.5f * fade, GLASS);
        put(w, L, X - 2, Y + 2, 0.7f * fade, GLASS); put(w, L, X + 1, Y + 2, 0.7f * fade, GLASS);
        put(w, L, X - 1, Y + 3, 0.8f * fade, GLASS); put(w, L, X, Y + 3, 0.8f * fade, GLASS);
    }
    if (b->vy > 0.f)                                          /* a runner's trail: specks back up its way, fading */
        for (int ty = Y - 4; (float)ty > b->y0 * (float)DH; ty -= 3) {
            const float t = ((float)ty / (float)DH - b->y0) / (b->y - b->y0 + 1e-4f);
            const int tx = (int)((b->x + 0.006f * wsin((float)ty * 0.25f)) * (float)DW);
            put(w, L, tx, ty, 0.5f * fade * t, GLASS);
        }
}

/* ── a frame ──────────────────────────────────────────────────────────────── */

void weather_frame(weather_t *w, int W, int H, float dt, bool show, float moon_lit) {
    w->nback = w->nfront = w->nglass = w->nbump = 0;
    w->show = false;
    if (!w->on || W <= 0 || H <= 0 || W * H > WX_GRID_MAX) return;
    if (w->kind != WX_CLEAR && (w->left -= dt) <= 0.f) { w->kind = WX_CLEAR; w->left = 0.f; }
    {   /* the level toward what was told, a step each WX_EASE_S; the floor wets while it rains, dries after */
        const float to = (float)w->kind, step = dt / WX_EASE_S;
        w->level = w->level < to ? fminf_(to, w->level + step) : fmaxf_(to, w->level - step);
        w->wet = clampf(w->level >= 0.5f ? w->wet + dt / WX_WET_S : w->wet - dt / WX_DRY_S, 0.f, 1.f);
    }
    if (w->level <= 0.f && w->wet <= 0.f && !w->nbead && !w->nstreak && !w->nring) return;   /* clear and dry: nothing */
    if (W != w->W || H != w->H) puddles(w, W, H);
    {   /* the shake: armed as a full downpour comes, due after its wait (the caller plays it when the cat is free) */
        if (w->level >= 2.7f) {
            if (w->shake_wait < 0.f) w->shake_wait = frnd(w, WX_SHAKE_FIRST_MIN_S, WX_SHAKE_FIRST_MAX_S);
            else if (!w->shake && (w->shake_wait -= dt) <= 0.f) w->shake = true;
        } else if (w->level < 2.5f) { w->shake = false; w->shake_wait = -1.f; }
    }
    const float sdt = dt < WX_SPAWN_DT ? dt : WX_SPAWN_DT;
    const wx_look_t k = look_at(w->level);
    if (!show) { w->nstreak = w->nring = 0; return; }        /* off the glass: the clocks only (it falls again from the top) */
    /* moonlit: drizzle and rain under a bright moon (not a downpour's) */
    w->moonlit = sstep(WX_MOONLIT_LO, WX_MOONLIT_HI, moon_lit) * (1.f - clampf(w->level - 2.f, 0.f, 1.f)) * clampf(w->level, 0.f, 1.f);
    {   /* the streaks: new ones, then on; landed: a ring, maybe a bump */
        const float on = clampf(w->level, 0.f, 1.f), far_moon = w->moonlit * WX_MOON_FAR * on;
        w->due_land += k.land * sdt; w->due_far += (k.far + far_moon) * sdt; w->due_front += k.front * sdt;
        for (; w->due_land >= 1.f; w->due_land -= 1.f) spawn(w, &k, SP_LAND, false);
        for (; w->due_far >= 1.f; w->due_far -= 1.f) spawn(w, &k, SP_FAR, far_moon > 0.f && frnd(w, 0.f, k.far + far_moon) < far_moon);
        for (; w->due_front >= 1.f; w->due_front -= 1.f) spawn(w, &k, SP_FRONT, false);
        for (int q = 0; q < w->nstreak; q++) {
            wx_streak_t *s = &w->streak[q];
            s->t += sdt;
            if ((w->frame + s->ph) % WX_STREAK_EVERY == 0) s->td = s->t;
            if (s->t < s->T) continue;
            if (s->lands) land(w, s);
            w->streak[q--] = w->streak[--w->nstreak];
        }
        for (int q = 0; q < w->nring; q++) {
            wx_ring_t *r = &w->ring[q];
            r->age += sdt;
            if (r->age < WX_SPLASH || (w->frame + r->ph) % 2 == 0) r->ad = r->age;   /* (a ring grows every other frame, in turns) */
            if (r->age >= (r->splash ? WX_SPLASH : WX_RING_LIFE)) w->ring[q--] = w->ring[--w->nring];
        }
    }
    beads(w, &k, sdt, dt);
    w->frame++;
    /* the cells: behind (streaks, splashes, rings), in front (a downpour's few), on the glass */
    layer_t back = { w->back_c, 0, WX_BACK_CELLS }, front = { w->front_c, 0, WX_FRONT_CELLS }, glass = { w->glass_c, 0, WX_GLASS_CELLS };
    for (int q = 0; q < w->nstreak; q++) if (!w->streak[q].front) draw_streak(w, &back, &w->streak[q], k.bri);
    for (int q = 0; q < w->nring; q++) draw_ring(w, &back, &w->ring[q], k.bri);
    done(w, &back);
    for (int q = 0; q < w->nstreak; q++) if (w->streak[q].front) draw_streak(w, &front, &w->streak[q], k.bri * 0.85f);
    done(w, &front);
    for (int q = 0; q < w->nbead; q++) draw_bead(w, &glass, &w->bead[q]);
    done(w, &glass);
    w->nback = back.n; w->nfront = front.n; w->nglass = glass.n;
    w->show = w->nback || w->nfront || w->nglass || w->wet > 0.f;
}
