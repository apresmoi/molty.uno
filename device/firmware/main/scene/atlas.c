/*
 * SPDX-License-Identifier: MIT
 *
 * atlas.c: the sky atlas's store, its levels and its drawing (atlas.h). The drawing follows the approved prototype
 * (research/design-record-staging/sky-atlas/board/draw.js) in glass units: the card's drawing centred at y 0.12 at
 * 0.82 of its own units, the name, the numbers and what shows it under it, the kind above.
 */
#include "atlas.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#define ATLAS_BSS EXT_RAM_BSS_ATTR              /* the store's index in PSRAM too: internal RAM is the network's */
#define ATLAS_CALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#define ATLAS_FREE(p)   heap_caps_free(p)
/* the link task loads, the fluid task draws, the sense task presses the centre: a few pointer copies under a short
 * critical section; nothing is freed or made inside one */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define lock()   taskENTER_CRITICAL(&s_mux)
#define unlock() taskEXIT_CRITICAL(&s_mux)
#else
#define ATLAS_CALLOC(n) calloc(1, (n))
#define ATLAS_FREE(p)   free(p)
#define lock()   ((void)0)
#define unlock() ((void)0)
#define ATLAS_BSS
#endif

#define DEG 0.017453293f
#define ATLAS_RETIRED 24                /* lists a burst of loads may replace before the next frame frees them */
#define PI_F 3.14159265f

/* the glass's colours (draw.js): the words, the rings, the figure's lines, a dim word */
#define INK_RGB   0xc8cce0u
#define GRID_RGB  0x565b72u
#define LINE_RGB  0x6f84b8u
#define DIM_RGB   0x8a90a8u
#define GOLD_RGB  0xffd88au
#define WHITE_RGB 0xffffffu
/* sizes the panel shows (a stroke under 0.016 is the grid's hairline, 1 px, which the AMOLED barely shows) */
#define STROKE    0.016f
#define STAR_MIN  0.009f                /* a star's least radius: 2 px                                         */
#define CARD_Y    0.12f                 /* the drawing's centre and its scale (draw.js drawObject)              */
#define CARD_K    0.82f
#define DOME_Y    0.12f
#define DOME_R    0.46f

static const uint32_t STAR_RGB[8] = { 0xa9c4ff, 0xcfdcff, 0xf4f6ff, 0xfff3dc, 0xffe2a8, 0xffc183, 0x7fe0d0, 0xff8a7a };
uint32_t atlas_star_rgb(int colour) { return STAR_RGB[colour & 7]; }

/* ── the prototype's models (data/make.mjs), in the same order of draws: the same dots ─────────────────────────── */
typedef struct { uint32_t s; } rng_t;
static float rnd(rng_t *r) { r->s = (r->s * 1103515245u + 12345u) & 0x7fffffffu; return (float)((double)r->s / 2147483647.0); }
static float gauss(rng_t *r) { float u = 0.f; for (int i = 0; i < 6; i++) u += rnd(r); return (u - 3.f) / 1.41421356f; }
static int jround(float v) { return (int)floorf(v + 0.5f); }   /* JavaScript's Math.round */
static int clampi(int v, int a, int b) { return v < a ? a : v > b ? b : v; }

uint32_t atlas_seed(const char *id) {
    uint64_t a = 7;
    for (const char *c = id; c && *c; c++) a = a * 31u + (uint8_t)*c;
    return (uint32_t)(a & 0xffffu);
}

static int put(atlas_pt_t *out, int n, int max, float x, float y, int level, int colour) {
    if (n < max) out[n] = (atlas_pt_t){ x * ATLAS_FIT, y * ATLAS_FIT, (uint8_t)clampi(level, 0, 15), (uint8_t)colour };
    return n + 1;
}
static int galaxy(rng_t *g, int hubble, float ratio, float pa, atlas_pt_t *out, int max) {
    const bool ell = hubble == 1, spiral = hubble == 0 || hubble == 4, irr = hubble == 3 || hubble == 4;
    const float sp = sinf(pa * DEG), cp = cosf(pa * DEG);
    int n = 0;
    for (int i = 0; i < 420; i++) {
        float x, y;
        int l;
        if (ell || rnd(g) < (spiral ? 0.25f : 0.45f)) {
            const float r = fabsf(gauss(g)) * (ell ? 0.4f : 0.15f), a = rnd(g) * 2.f * PI_F;
            x = r * cosf(a); y = r * sinf(a); l = jround(15.f - 20.f * r);
        } else if (irr) {
            x = gauss(g) * 0.35f + 0.15f * sinf((float)i); y = gauss(g) * 0.22f; l = 6 + jround(5.f * rnd(g));
        } else {
            const float r = 0.12f + -logf(1.f - rnd(g) * 0.97f) * 0.3f;
            if (r > 1.f) { i--; continue; }
            float a = rnd(g) * 2.f * PI_F;
            if (spiral && rnd(g) < 0.6f) { const float arm = rnd(g) < 0.5f ? 0.f : PI_F; a = arm + logf(r / 0.12f) / 0.32f + gauss(g) * 0.22f; }
            x = r * cosf(a); y = r * sinf(a); l = jround(11.f - 8.f * r);
        }
        y *= ratio;                                       /* the tilt; the major axis toward pa, north through east */
        n = put(out, n, max, -x * sp + y * cp, x * cp + y * sp, clampi(l, 1, 15), l > 10 ? 4 : 2);
    }
    return n;
}
static int globular(rng_t *g, atlas_pt_t *out, int max) {
    int n = 0;
    for (int i = 0; i < 360; i++) {
        const float u = fmaxf(1e-3f, rnd(g)), r = 0.16f / sqrtf(powf(u, -2.f / 3.f) - 1.f);
        if (!(r <= 1.f)) { i--; continue; }
        const float a = rnd(g) * 2.f * PI_F, x = r * cosf(a), y = r * sinf(a);
        const int l = clampi(jround(15.f - 12.f * r + 3.f * gauss(g)), 2, 15);
        n = put(out, n, max, x, y, l, rnd(g) < 0.7f ? 4 : 5);
    }
    return n;
}
static int nebula(rng_t *g, bool pn, float ratio, atlas_pt_t *out, int max) {
    float lobes[5][3];
    for (int q = 0; q < 5; q++) { lobes[q][0] = gauss(g) * 0.38f; lobes[q][1] = gauss(g) * 0.38f * ratio; lobes[q][2] = 0.24f + 0.26f * rnd(g); }
    int n = 0;
    for (int i = 0; i < (pn ? 300 : 420); i++) {
        float x, y;
        int l;
        if (pn) { const float a = rnd(g) * 2.f * PI_F, r = 0.55f + 0.12f * gauss(g); x = r * cosf(a); y = r * sinf(a) * ratio; l = 6 + jround(6.f * rnd(g)); }
        else { const float *L = lobes[(int)floorf(rnd(g) * 5.f) % 5]; x = L[0] + gauss(g) * L[2]; y = L[1] + gauss(g) * L[2] * ratio; l = jround(4.f + 8.f * rnd(g)); }
        if (hypotf(x, y) > 1.f) { i--; continue; }
        n = put(out, n, max, x, y, l, pn ? 6 : 7);
    }
    if (pn) n = put(out, n, max, 0.f, 0.f, 9, 2);
    return n;
}
static int open_cluster(rng_t *g, float size, atlas_pt_t *out, int max) {
    const int count = jround(fminf(90.f, 18.f + 0.9f * (size > 0.f ? size : 15.f)));
    int n = 0;
    for (int i = 0; i < count; i++) {
        const float r = powf(sqrtf(rnd(g)), 1.4f) * 0.95f, a = rnd(g) * 2.f * PI_F, mag = 2.f + 9.f * sqrtf(rnd(g));
        const int l = clampi(jround(15.f * (11.5f - mag) / 9.5f), 1, 15);
        n = put(out, n, max, r * cosf(a), r * sinf(a), l, (int)floorf(rnd(g) * 8.f) < 6 ? 2 : 5);
    }
    return n;
}
int atlas_model(int type, int hubble, float ratio, float pa, float size, uint32_t seed, atlas_pt_t *out, int max) {
    rng_t g = { seed };
    int n = 0;
    switch (type) {
    case AM_GALAXY:    n = galaxy(&g, hubble, ratio > 0.f ? fmaxf(0.12f, ratio) : 0.6f, pa, out, max); break;
    case AM_GLOBULAR:  n = globular(&g, out, max); break;
    case AM_NEBULA:    n = nebula(&g, false, ratio > 0.f ? fmaxf(0.3f, ratio) : 0.75f, out, max); break;
    case AM_PLANETARY: n = nebula(&g, true, ratio > 0.f ? fmaxf(0.3f, ratio) : 0.75f, out, max); break;
    case AM_OPEN:      n = open_cluster(&g, size, out, max); break;
    default: break;
    }
    return n < max ? n : max;
}

/* ── the store ─────────────────────────────────────────────────────────────────────────────────────────────────── */
typedef struct {
    char id[ATLAS_ID + 1], name[ATLAS_NAME + 1], facts[ATLAS_FACTS + 1];
    uint8_t see, dk;
    uint8_t *pts, *lines;               /* AD_STARS, AD_DOTS, AD_RADIANT: 3 bytes a point, 2 a line (PSRAM)    */
    uint16_t npts, nlines;
    int16_t focus;                      /* the star ringed (a bright star in its constellation), -1            */
    uint8_t mtype, hubble;              /* AD_MODEL                                                            */
    float ratio, pa, size;
    float lit, side, tilt;              /* AD_GLOBE: lit fraction, the side lit, Saturn's rings' tilt (0 none) */
    uint32_t tint;
    float rx, ry;                       /* AD_RADIANT: the radiant in its constellation's drawing             */
    float moon;                         /* the full Moon's radius at the drawing's scale, 0 none              */
    /* tonight: its path from the load, a point every step minutes (az / 2, alt + 90), its times after the load */
    uint8_t npath, path[ATLAS_PATH][2];
    uint16_t step;
    int16_t rise, high, set;
    int8_t high_alt;
    bool never, where;
} obj_t;
typedef struct { char id[ATLAS_ID + 1], name[ATLAS_SHORT + 1]; uint32_t tint; int count, up; obj_t *obj; int nobj; } kind_t;


static ATLAS_BSS struct {
    kind_t kinds[ATLAS_KINDS];
    int nkinds;
    bool place;                         /* the agent knows where its human is: the where face shows          */
    int clock;                          /* ... minutes of the place's day at the load                          */
    float since;                        /* s since the kinds came                                              */
    float orphan;                       /* s with no atlas on screen                                           */
    bool shown;                         /* a screen has shown it                                               */
    /* where it is: the finger's (target) and the glass's (shown: the target once the glass caught up) */
    int level, kind, index, face, s_level, s_kind, s_index, s_face;
    uint32_t serial;                    /* the store's: bumped with any load                                   */
    uint32_t key;
    /* lists a load replaced: the fluid task may still draw from one this frame, so it frees them at its next */
    struct { obj_t *obj; int n; } retired[ATLAS_RETIRED];
    int nretired;
} S;

/* the card's dots (a model made, or a picture's sent): PSRAM, made once a card (the fluid task's) */
static atlas_pt_t *s_dots;
static int s_ndots;
static uint32_t s_dots_of, s_store;

static void free_list(obj_t *obj, int n) {
    for (int i = 0; obj && i < n; i++) { ATLAS_FREE(obj[i].pts); ATLAS_FREE(obj[i].lines); }
    ATLAS_FREE(obj);
}
/* (locked) kind k's list goes to the retired, false when they are full */
static bool retire(kind_t *k) {
    if (!k->obj) return true;
    if (S.nretired >= ATLAS_RETIRED) return false;
    S.retired[S.nretired].obj = k->obj; S.retired[S.nretired++].n = k->nobj;
    k->obj = NULL; k->nobj = 0;
    return true;
}
/* the fluid task: frees what was retired (and with all, the store too) */
static void reap(bool all) {
    static ATLAS_BSS struct { obj_t *obj; int n; } out[ATLAS_RETIRED + ATLAS_KINDS];   /* (static: the fluid task's stack is small) */
    int n = 0;
    lock();
    for (int q = 0; q < S.nretired; q++) { out[n].obj = S.retired[q].obj; out[n++].n = S.retired[q].n; }
    S.nretired = 0;
    if (all) {
        for (int k = 0; k < S.nkinds; k++) { out[n].obj = S.kinds[k].obj; out[n++].n = S.kinds[k].nobj; S.kinds[k].obj = NULL; S.kinds[k].nobj = 0; }
        S.nkinds = 0; S.shown = false; S.place = false;
    }
    unlock();
    for (int q = 0; q < n; q++) free_list(out[q].obj, out[q].n);
}
void atlas_clear(void) {
    reap(true);
    lock();
    S.orphan = 0.f;
    S.level = S.s_level = ATLAS_AT_KINDS; S.kind = S.s_kind = 0; S.index = S.s_index = 0; S.face = S.s_face = ATLAS_DRAWING;
    unlock();
}
size_t atlas_bytes(void) {
    size_t n = s_dots ? sizeof(atlas_pt_t) * ATLAS_DOTS : 0;
    lock();
    for (int k = 0; k < S.nkinds; k++) {
        n += sizeof(obj_t) * (size_t)S.kinds[k].nobj;
        for (int i = 0; i < S.kinds[k].nobj; i++) n += (size_t)S.kinds[k].obj[i].npts * 3 + (size_t)S.kinds[k].obj[i].nlines * 2;
    }
    unlock();
    return n;
}
int atlas_kind_index(const char *id) {
    lock();
    int at = -1;
    for (int k = 0; k < S.nkinds; k++) if (!strcmp(S.kinds[k].id, id)) at = k;
    unlock();
    return at;
}

static const char *str_of(const cJSON *o, const char *k) { const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(o, k)); return s ? s : ""; }
static double num_of(const cJSON *o, const char *k, double d) { const cJSON *v = cJSON_GetObjectItem(o, k); return cJSON_IsNumber(v) ? v->valuedouble : d; }
static bool rgb_of(const char *s, uint32_t *rgb) { return pkg_color(s, rgb); }
/* the glass's letters: printable ASCII, upper case (the 5x7 font has no lower case) */
static bool words(const char *s, char *out, int max) {
    const int n = (int)strlen(s);
    if (n > max) return false;
    for (int i = 0; i < n; i++) { if (s[i] < 32 || s[i] > 126) return false; out[i] = s[i] >= 'a' && s[i] <= 'z' ? (char)(s[i] - 32) : s[i]; }
    out[n] = 0;
    return true;
}
/* base64 into a new PSRAM buffer of len bytes (a multiple of unit, at most max units) */
static int b64v(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1; }
static uint8_t *b64(const char *s, int unit, int max, int *count, bool *bad) {
    const size_t n = strlen(s);
    *count = 0;
    if (!n) return NULL;
    if (n % 4) { *bad = true; return NULL; }
    const int pad = (s[n - 1] == '=') + (s[n - 2] == '='), len = (int)(n / 4 * 3) - pad;
    if (len % unit || len / unit > max) { *bad = true; return NULL; }
    uint8_t *out = ATLAS_CALLOC((size_t)len);
    if (!out) { *bad = true; return NULL; }
    int o = 0;
    for (size_t i = 0; i < n; i += 4) {
        int v[4];
        for (int q = 0; q < 4; q++) { v[q] = s[i + q] == '=' ? 0 : b64v(s[i + q]); if (v[q] < 0) { ATLAS_FREE(out); *bad = true; return NULL; } }
        const uint32_t w = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)v[2] << 6 | (uint32_t)v[3];
        for (int q = 0; q < 3 && o < len; q++) out[o++] = (uint8_t)(w >> (16 - 8 * q));
    }
    *count = len / unit;
    return out;
}
static const char *const MODELS[] = { "", "galaxy", "globular", "nebula", "planetary", "open" };
static const char *const HUBBLE[] = { "spiral", "elliptical", "lenticular", "irregular", "spiral-irregular" };
static const char *const SEES[] = { "eye", "binoculars", "telescope" };
static int name_index(const char *s, const char *const *names, int n) { for (int i = 0; i < n; i++) if (!strcmp(s, names[i])) return i; return -1; }

/* one object of atlas.load: false on anything out of shape */
static bool parse_obj(const cJSON *j, obj_t *o) {
    *o = (obj_t){ .focus = -1, .rise = -1, .high = -1, .set = -1 };
    if (!cJSON_IsObject(j) || !pkg_id_ok(str_of(j, "id")) || strlen(str_of(j, "id")) > ATLAS_ID) return false;
    snprintf(o->id, sizeof o->id, "%s", str_of(j, "id"));
    if (!words(str_of(j, "name"), o->name, ATLAS_NAME) || !o->name[0] || !words(str_of(j, "facts"), o->facts, ATLAS_FACTS)) return false;
    const int see = name_index(str_of(j, "see"), SEES, 3);
    if (see < 0) return false;
    o->see = (uint8_t)see;
    const cJSON *d = cJSON_GetObjectItem(j, "d");
    const char *k = str_of(d, "k");
    bool bad = false;
    int n;
    o->moon = (float)num_of(d, "moon", 0.);
    if (!strcmp(k, "stars") || !strcmp(k, "dots") || !strcmp(k, "radiant")) {
        const bool dots = !strcmp(k, "dots");
        o->dk = dots ? AD_DOTS : !strcmp(k, "stars") ? AD_STARS : AD_RADIANT;
        o->pts = b64(str_of(d, "p"), 3, dots ? ATLAS_DOTS : ATLAS_STARS, &n, &bad); o->npts = (uint16_t)n;
        o->lines = b64(str_of(d, "l"), 2, ATLAS_LINES, &n, &bad); o->nlines = (uint16_t)n;
        if (bad || !o->npts) return false;
        for (int q = 0; q < o->nlines; q++) if (o->lines[2 * q] >= o->npts || o->lines[2 * q + 1] >= o->npts) return false;
        o->focus = (int16_t)num_of(d, "focus", -1.);
        if (o->focus >= o->npts) return false;
        const cJSON *at = cJSON_GetObjectItem(d, "at");
        if (o->dk == AD_RADIANT && (cJSON_GetArraySize(at) != 2 || !cJSON_IsNumber(cJSON_GetArrayItem(at, 0)) || !cJSON_IsNumber(cJSON_GetArrayItem(at, 1)))) return false;
        if (o->dk == AD_RADIANT) { o->rx = (float)cJSON_GetArrayItem(at, 0)->valuedouble; o->ry = (float)cJSON_GetArrayItem(at, 1)->valuedouble; }
    } else if (!strcmp(k, "model")) {
        const int m = name_index(str_of(d, "m"), MODELS, 6), h = name_index(str_of(d, "hubble"), HUBBLE, 5);
        if (m < 1) return false;
        o->dk = AD_MODEL; o->mtype = (uint8_t)m; o->hubble = (uint8_t)(h < 0 ? 0 : h);
        o->ratio = (float)num_of(d, "ratio", 0.); o->pa = (float)num_of(d, "pa", 0.); o->size = (float)num_of(d, "size", 0.);
        if (!(o->ratio >= 0.f && o->ratio <= 1.f) || !(o->size >= 0.f && o->size <= 20000.f) || !(fabsf(o->pa) <= 360.f)) return false;
    } else if (!strcmp(k, "globe")) {
        o->dk = AD_GLOBE;
        o->lit = (float)num_of(d, "lit", 1.); o->side = num_of(d, "side", 1.) < 0. ? -1.f : 1.f; o->tilt = (float)num_of(d, "rings", 0.);
        if (!(o->lit >= 0.f && o->lit <= 1.f) || !rgb_of(str_of(d, "tint"), &o->tint)) return false;
    } else return false;
    /* tonight, with a place */
    const cJSON *w = cJSON_GetObjectItem(j, "w");
    if (w) {
        uint8_t *p = b64(str_of(w, "p"), 2, ATLAS_PATH, &n, &bad);
        if (bad || !n) { ATLAS_FREE(p); return false; }
        memcpy(o->path, p, (size_t)n * 2); o->npath = (uint8_t)n; ATLAS_FREE(p);
        o->step = (uint16_t)num_of(w, "step", 45.);
        const cJSON *t = cJSON_GetObjectItem(w, "t");
        if (cJSON_GetArraySize(t) != 3 || o->step < 1) return false;
        for (int q = 0; q < 3; q++) if (!cJSON_IsNumber(cJSON_GetArrayItem(t, q)) || fabs(cJSON_GetArrayItem(t, q)->valuedouble) > 2880.) return false;
        o->rise = (int16_t)cJSON_GetArrayItem(t, 0)->valuedouble; o->high = (int16_t)cJSON_GetArrayItem(t, 1)->valuedouble; o->set = (int16_t)cJSON_GetArrayItem(t, 2)->valuedouble;
        o->high_alt = (int8_t)num_of(w, "ha", 0.);
        o->never = cJSON_IsTrue(cJSON_GetObjectItem(w, "never"));
        o->where = true;
    }
    return true;
}

const char *atlas_load(const cJSON *args) {
    if (!cJSON_IsObject(args)) return "invalid_args";
    const cJSON *kinds = cJSON_GetObjectItem(args, "kinds"), *objs = cJSON_GetObjectItem(args, "objects");
    if (!kinds && !objs) return "invalid_args";
    /* the kinds: a new atlas (what was loaded goes) */
    kind_t ks[ATLAS_KINDS];
    int nk = 0;
    if (kinds) {
        if (!cJSON_IsArray(kinds) || cJSON_GetArraySize(kinds) < 1 || cJSON_GetArraySize(kinds) > ATLAS_KINDS) return "invalid_args";
        const cJSON *k;
        cJSON_ArrayForEach(k, kinds) {
            kind_t *o = &ks[nk++];
            *o = (kind_t){ 0 };
            if (!pkg_id_ok(str_of(k, "id")) || strlen(str_of(k, "id")) > ATLAS_ID || !words(str_of(k, "name"), o->name, ATLAS_SHORT) || !o->name[0] || !rgb_of(str_of(k, "tint"), &o->tint)) return "invalid_args";
            snprintf(o->id, sizeof o->id, "%s", str_of(k, "id"));
            o->count = (int)num_of(k, "count", 0.); o->up = (int)num_of(k, "up", -1.);
        }
    }
    /* a kind's objects, in the wheel's order: made whole before they replace what was there */
    int at = -1, n = 0;
    obj_t *list = NULL;
    if (objs) {
        const char *kid = str_of(args, "kind");
        if (!cJSON_IsArray(objs) || cJSON_GetArraySize(objs) < 1 || cJSON_GetArraySize(objs) > ATLAS_OBJECTS) return "invalid_args";
        for (int q = 0; q < (kinds ? nk : 0); q++) if (!strcmp(ks[q].id, kid)) at = q;
        if (!kinds) at = atlas_kind_index(kid);   /* (the kinds stay put: only a load with kinds replaces them, on this task) */
        if (at < 0) return "invalid_args";
        list = ATLAS_CALLOC(sizeof(obj_t) * (size_t)cJSON_GetArraySize(objs));
        if (!list) return "oom";
        const cJSON *o;
        cJSON_ArrayForEach(o, objs) {
            if (!parse_obj(o, &list[n])) {
                free_list(list, n + 1);
                return "invalid_args";
            }
            n++;
        }
    }
    const int clock = (int)num_of(args, "clock", -2.);
    lock();
    bool room = true;
    for (int k = 0; kinds && k < S.nkinds; k++) room &= S.kinds[k].obj == NULL || S.nretired + (S.nkinds - k) <= ATLAS_RETIRED;
    if (objs) room &= S.kinds[at].obj == NULL || S.nretired < ATLAS_RETIRED;
    if (!room) { unlock(); free_list(list, n); return "busy"; }   /* (loads faster than frames: the agent sends again) */
    if (kinds) {
        for (int k = 0; k < S.nkinds; k++) retire(&S.kinds[k]);
        memcpy(S.kinds, ks, sizeof(kind_t) * (size_t)nk); S.nkinds = nk;
        S.since = 0.f; S.orphan = 0.f; S.shown = false; S.place = clock >= 0 && clock < 1440; S.clock = S.place ? clock : 0;
        S.level = ATLAS_AT_KINDS; S.kind = S.index = 0; S.face = ATLAS_DRAWING;
    } else if (clock != -2) { S.place = clock >= 0 && clock < 1440; S.clock = S.place ? clock : 0; }
    if (objs) {
        retire(&S.kinds[at]);
        S.kinds[at].obj = list; S.kinds[at].nobj = n;
        if (S.kind == at && S.index >= n) S.index = n - 1;
    }
    S.serial++;
    unlock();
    return NULL;
}

const char *atlas_show(const cJSON *args) {
    if (!cJSON_IsObject(args)) return "invalid_args";
    lock();
    const char *err = NULL;
    int k = S.kind, i = (int)num_of(args, "index", 0.);
    const char *kid = str_of(args, "kind"), *face = str_of(args, "face"), *level = str_of(args, "level");
    if (kid[0]) { k = -1; for (int q = 0; q < S.nkinds; q++) if (!strcmp(S.kinds[q].id, kid)) k = q; }
    const bool objects = strcmp(level, "kinds") != 0;
    if (k < 0 || !S.nkinds || (objects && (i < 0 || i >= S.kinds[k].nobj)) || (face[0] && strcmp(face, "drawing") && strcmp(face, "where"))) err = "invalid_args";
    else {
        S.kind = k; S.level = objects ? ATLAS_AT_OBJECTS : ATLAS_AT_KINDS; S.index = objects ? i : 0;
        S.face = !strcmp(face, "where") && S.place ? ATLAS_WHERE : ATLAS_DRAWING;
    }
    unlock();
    return err;
}

/* ── the levels ────────────────────────────────────────────────────────────────────────────────────────────────── */
bool atlas_active(void) { lock(); const bool on = S.shown && S.nkinds > 0; unlock(); return on; }
void atlas_where(int *level, int *kind, int *index, int *face) {
    lock();
    *level = S.level; *kind = S.kind; *index = S.level == ATLAS_AT_OBJECTS ? S.index : -1; *face = S.face;
    unlock();
}
int atlas_turn(int clicks, bool fast) {
    lock();
    int moved = 0;
    if (S.nkinds && clicks) {
        if (S.level == ATLAS_AT_KINDS) { S.kind = ((S.kind + clicks) % S.nkinds + S.nkinds) % S.nkinds; moved = 1; }
        else {
            const int L = S.kinds[S.kind].nobj;
            if (L > 0) {
                const int step = clicks * (fast ? 5 : 1), was = S.index, to = was + step;
                S.index = ((to % L) + L) % L;
                moved = to >= L || to < 0 || (was != 0 && S.index == 0) ? 2 : 1;   /* (passed the first: a firmer tick) */
            }
        }
    }
    unlock();
    return moved;
}
void atlas_local_centre(void) {
    lock();
    if (S.nkinds) {
        if (S.level == ATLAS_AT_KINDS) { if (S.kinds[S.kind].nobj) { S.level = ATLAS_AT_OBJECTS; S.index = 0; S.face = ATLAS_DRAWING; } }
        else if (S.place) S.face = S.face == ATLAS_DRAWING ? ATLAS_WHERE : ATLAS_DRAWING;   /* (no place: no where face) */
    }
}
void atlas_local_up(void) {
    lock();
    if (S.level == ATLAS_AT_OBJECTS) { S.level = ATLAS_AT_KINDS; S.face = ATLAS_DRAWING; }
    unlock();
}
bool atlas_label(float *y) {
    *y = ATLAS_TOP_Y;
    lock();
    const bool on = S.nkinds && S.level == ATLAS_AT_OBJECTS;
    unlock();
    return on;
}

/* tonight for o at m minutes after the load: where it is (its path), up or not */
static void now_at(const obj_t *o, float m, float *az, float *alt) {
    *az = 0.f; *alt = -90.f;
    if (!o->npath) return;
    float e = m / (float)o->step;
    if (e < 0.f) e = 0.f;
    const int i = (int)e;
    if (i >= o->npath - 1) { *az = o->path[o->npath - 1][0] * 2.f; *alt = o->path[o->npath - 1][1] - 90.f; return; }
    const float f = e - (float)i, a0 = o->path[i][0] * 2.f, a1 = o->path[i + 1][0] * 2.f;
    float d = a1 - a0;
    d -= 360.f * floorf((d + 180.f) / 360.f);
    *az = fmodf(a0 + d * f + 360.f, 360.f);
    *alt = (o->path[i][1] - 90.f) * (1.f - f) + (o->path[i + 1][1] - 90.f) * f;
}
static bool up_now(const obj_t *o, float m) { float az, alt; now_at(o, m, &az, &alt); return o->where && alt > 0.f; }
static uint32_t scale(uint32_t rgb, float k) {
    return (uint32_t)lrintf((rgb >> 16 & 255) * k) << 16 | (uint32_t)lrintf((rgb >> 8 & 255) * k) << 8 | (uint32_t)lrintf((rgb & 255) * k);
}

int atlas_beads(char (*labels)[PKG_KEY_MAX + 1], uint32_t *tints, int max, int *index, float *step) {
    lock();
    int n = 0;
    *index = 0;
    const float m = S.since / 60.f;
    if (S.level == ATLAS_AT_KINDS) {
        *step = 30.f;
        for (int k = 0; k < S.nkinds && n < max; k++, n++) { snprintf(labels[n], PKG_KEY_MAX + 1, "%s", S.kinds[k].name); tints[n] = S.kinds[k].tint; }
        *index = S.kind;
    } else {
        /* the objects round the one chosen (scene_wheel draws a bead every step degrees, 165 each side at most) */
        *step = 15.f;
        const kind_t *k = &S.kinds[S.kind];
        const int L = k->nobj, half = L < max ? L / 2 : (max - 1) / 2;
        for (int r = -half; r <= half && n < max && n < L; r++, n++) {
            const obj_t *o = &k->obj[((S.index + r) % L + L) % L];
            snprintf(labels[n], PKG_KEY_MAX + 1, "%s", o->name);
            tints[n] = up_now(o, m) ? k->tint : 0;      /* lit while it is up, grey while down */
            if (!r) *index = n;
        }
    }
    unlock();
    return n;
}
float atlas_key(void) { lock(); const float k = (float)(S.level * 100000 + S.kind * 1000 + S.index); unlock(); return k; }
void atlas_wheel(scene_t *sc, scene_wheel_t *w, char (*labels)[PKG_KEY_MAX + 1], uint32_t *tints, int max) {
    const int d = scene_dial(sc, 0);
    float step;
    w->face = 0; w->level = 0; w->quiet = true; w->above = w->below = NULL;
    w->nlabels = atlas_beads(labels, tints, max, &w->index, &step);
    w->labels = (const char (*)[PKG_KEY_MAX + 1])labels; w->tints = tints;
    if (d >= 0) sc->items[d].step = step;
}
void atlas_settled(char *spot, int nspot, char *id, int nid, char *label, int nlabel, int *index) {
    lock();
    const kind_t *k = S.nkinds ? &S.kinds[S.kind] : NULL;
    const obj_t *o = k && S.level == ATLAS_AT_OBJECTS && S.index < k->nobj ? &k->obj[S.index] : NULL;
    snprintf(spot, (size_t)nspot, "%s", o ? "object" : "kind");
    snprintf(id, (size_t)nid, "%s", o ? o->id : k ? k->id : "");
    snprintf(label, (size_t)nlabel, "%s", o ? o->name : k ? k->name : "");
    *index = o ? S.index : S.kind;
    unlock();
}

/* ── the drawing ───────────────────────────────────────────────────────────────────────────────────────────────── */
int atlas_item(const scene_t *sc) {
    for (int n = 0; sc && n < sc->nfixed; n++) if (sc->items[n].k == PK_ATLAS) return n;
    return -1;
}
static scene_item_t *item(scene_t *sc, uint8_t k) {
    if (sc->nitems >= SCENE_ITEMS_MAX + SCENE_LIVE_MAX) return NULL;
    scene_item_t *o = &sc->items[sc->nitems++];
    *o = (scene_item_t){ .k = k, .asset = -1, .rgb = WHITE_RGB, .lst = -1, .layer = -1, .w = STROKE, .n = 1, .lit = 1.f, .side = 1.f };
    return o;
}
static void disc(scene_t *sc, float x, float y, float r, uint32_t rgb) { scene_item_t *o = item(sc, PK_DISC); if (o) { o->x = x; o->y = y; o->r = r; o->rgb = rgb; } }
static void ring(scene_t *sc, float x, float y, float r, uint32_t rgb) { scene_item_t *o = item(sc, PK_RING); if (o) { o->x = x; o->y = y; o->r = r; o->rgb = rgb; } }
static scene_item_t *line(scene_t *sc, int npts, uint32_t rgb) {
    if (sc->npts + npts > SCENE_PTS_MAX + SCENE_LIVE_PTS) return NULL;
    scene_item_t *o = item(sc, PK_LINE);
    if (o) { o->rgb = rgb; o->pts = (uint16_t)sc->npts; o->npts = (uint16_t)npts; sc->npts += npts; }
    return o;
}
static void seg(scene_t *sc, float x0, float y0, float x1, float y1, uint32_t rgb) {
    scene_item_t *o = line(sc, 2, rgb);
    if (o) { sc->pts[o->pts][0] = x0; sc->pts[o->pts][1] = y0; sc->pts[o->pts + 1][0] = x1; sc->pts[o->pts + 1][1] = y1; }
}
static void text(scene_t *sc, const char *s, float x, float y, float size, uint32_t rgb) {
    const int n = (int)strlen(s);
    if (!n || sc->nchars + n > SCENE_LETTERS) return;
    scene_item_t *o = item(sc, PK_TEXT);
    if (!o) return;
    o->x = x; o->y = y; o->size = size; o->rgb = rgb; o->str = (uint16_t)sc->nchars; o->nstr = (uint16_t)n;
    memcpy(sc->chars + sc->nchars, s, (size_t)n); sc->nchars += n;
}
static float fit(const char *s, float max, float room) { return fminf(max, room / (0.6f * (float)(strlen(s) ? strlen(s) : 1))); }
static atlas_pt_t pt3(const uint8_t *p) { return (atlas_pt_t){ (float)(int8_t)p[0] * (0.6f / 127.f), (float)(int8_t)p[1] * (0.6f / 127.f), (uint8_t)(p[2] >> 3), (uint8_t)(p[2] & 7) }; }
static float star_r(int level, float k) { return fmaxf(STAR_MIN, (0.0045f + 0.0019f * (float)level) * k / CARD_K); }


/* a field of dots on the braille lattice, one item a colour and brightness (the points in sc->pts) */
static void field(scene_t *sc, const atlas_pt_t *p, int n, float cx, float cy, float k, float alpha) {
    for (int c = 0; c < 8; c++) for (int band = 0; band < 3; band++) {
        int m = 0;
        for (int q = 0; q < n; q++) m += p[q].colour == c && (p[q].level < 6 ? 0 : p[q].level < 11 ? 1 : 2) == band;
        if (!m || sc->npts + m > SCENE_PTS_MAX + SCENE_LIVE_PTS) continue;
        scene_item_t *o = item(sc, PK_FIELD);
        if (!o) return;
        o->rgb = scale(STAR_RGB[c], alpha * (0.45f + 0.275f * (float)band)); o->pts = (uint16_t)sc->npts; o->npts = (uint16_t)m;
        for (int q = 0; q < n; q++) {
            if (p[q].colour != c || (p[q].level < 6 ? 0 : p[q].level < 11 ? 1 : 2) != band) continue;
            sc->pts[sc->npts][0] = cx + p[q].x * k; sc->pts[sc->npts][1] = cy + p[q].y * k; sc->npts++;
        }
    }
}
/* stars and their figure's lines */
static void stars(scene_t *sc, const obj_t *o, float cx, float cy, float k, float alpha) {
    const bool dbl = !o->nlines && o->npts <= 4 && o->dk == AD_STARS, dim = o->focus >= 0;
    for (int q = 0; q < o->nlines; q++) {
        const atlas_pt_t a = pt3(o->pts + 3 * o->lines[2 * q]), b = pt3(o->pts + 3 * o->lines[2 * q + 1]);
        const float ax = cx + a.x * k, ay = cy + a.y * k, bx = cx + b.x * k, by = cy + b.y * k, L = hypotf(bx - ax, by - ay);
        const float ga = star_r(a.level, k) + 0.018f, gb = star_r(b.level, k) + 0.018f;
        if (L < 1e-4f || ga + gb >= L) continue;
        const float ux = (bx - ax) / L, uy = (by - ay) / L;
        seg(sc, ax + ux * ga, ay + uy * ga, bx - ux * gb, by - uy * gb, scale(LINE_RGB, alpha * (dim ? 0.55f : 0.85f)));
    }
    for (int q = 0; q < o->npts; q++) {
        const atlas_pt_t p = pt3(o->pts + 3 * q);
        const float a = alpha * (dim && q != o->focus ? 0.42f : 1.f);
        disc(sc, cx + p.x * k, cy + p.y * k, star_r(p.level, k) * (dbl ? 1.5f : 1.f), scale(STAR_RGB[p.colour], a));
    }
    if (o->focus >= 0) {
        const atlas_pt_t p = pt3(o->pts + 3 * o->focus);
        ring(sc, cx + p.x * k, cy + p.y * k, star_r(p.level, k) + 0.035f, scale(WHITE_RGB, 0.8f * alpha));
    }
}
static void draw_object(scene_t *sc, const kind_t *kd, const obj_t *o, float cx, float cy, float k, float alpha) {
    switch (o->dk) {
    case AD_STARS: stars(sc, o, cx, cy, k, alpha); break;
    case AD_DOTS: case AD_MODEL: {
        const uint32_t of = ((uint32_t)(uintptr_t)o ^ o->dk) * 16777619u ^ s_store;
        if (!s_dots && !(s_dots = ATLAS_CALLOC(sizeof(atlas_pt_t) * ATLAS_DOTS))) return;
        if (s_dots_of != of) {
            if (o->dk == AD_MODEL) s_ndots = atlas_model(o->mtype, o->hubble, o->ratio, o->pa, o->size, atlas_seed(o->id), s_dots, ATLAS_DOTS);
            else { s_ndots = o->npts; for (int q = 0; q < o->npts; q++) s_dots[q] = pt3(o->pts + 3 * q); }
            s_dots_of = of;
        }
        if (o->dk == AD_MODEL && o->mtype == AM_OPEN) {    /* an open cluster's model: stars, as the prototype draws it */
            for (int q = 0; q < s_ndots; q++) disc(sc, cx + s_dots[q].x * k, cy + s_dots[q].y * k, star_r(s_dots[q].level, k), scale(STAR_RGB[s_dots[q].colour], alpha));
        } else field(sc, s_dots, s_ndots, cx, cy, k, alpha);
        break;
    }
    case AD_GLOBE: {
        const float r = 0.36f * k / CARD_K;
        scene_item_t *b = item(sc, PK_BALL);
        if (b) { b->x = cx; b->y = cy; b->r = r; b->lit = o->lit; b->side = o->side; b->rgb = scale(o->tint, alpha); }
        if (o->tilt != 0.f) {                             /* Saturn's rings: an ellipse at tonight's tilt, its back half hidden */
            const float t = fmaxf(0.06f, fabsf(sinf(o->tilt * DEG))) * (o->tilt < 0.f ? -1.f : 1.f);
            for (int band = 0; band < 2; band++) {
                const float rr = r * (band ? 2.05f : 1.65f);
                scene_item_t *l = line(sc, 17, scale(0xead9a0u, alpha * (band ? 0.6f : 0.85f)));
                if (!l) break;
                for (int q = 0; q <= 16; q++) { const float th = PI_F + PI_F * (float)q / 16.f; sc->pts[l->pts + q][0] = cx + cosf(th) * rr; sc->pts[l->pts + q][1] = cy + sinf(th) * rr * t; }
            }
        }
        break;
    }
    case AD_RADIANT: {                                    /* its constellation faint, centred between it and the radiant */
        const float kk = k * 0.8f, ox = cx - o->rx * kk * 0.5f, oy = cy - o->ry * kk * 0.5f, rx = ox + o->rx * kk, ry = oy + o->ry * kk;
        stars(sc, o, ox, oy, kk, 0.4f * alpha);
        rng_t g = { 17 };
        const uint32_t tint = kd ? kd->tint : 0xb8f0a0u;
        for (int q = 0; q < 11; q++) {
            const float a = (float)q / 11.f * 2.f * PI_F + rnd(&g) * 0.4f, r0 = 0.07f + 0.2f * rnd(&g), len = 0.07f + 0.14f * rnd(&g), lit = 0.35f + 0.5f * rnd(&g);
            seg(sc, rx + cosf(a) * r0, ry + sinf(a) * r0, rx + cosf(a) * (r0 + len), ry + sinf(a) * (r0 + len), scale(tint, alpha * lit));
        }
        ring(sc, rx, ry, 0.025f, scale(tint, alpha));
        break;
    }
    default: break;
    }
    if (o->moon > 0.f && alpha > 0.9f) {                  /* the full Moon at the same scale, low right */
        const float r = o->moon * k;
        if (r > 0.025f && r < 0.22f) ring(sc, 0.42f - r * 0.4f, -0.3f + r * 0.4f, r, scale(DIM_RGB, 0.8f));
    }
}

static const char *const DIRS[8] = { "NORTH", "NORTH-EAST", "EAST", "SOUTH-EAST", "SOUTH", "SOUTH-WEST", "WEST", "NORTH-WEST" };
static const char *dir_of(float az) { return DIRS[(int)floorf(fmodf(az + 360.f, 360.f) / 45.f + 0.5f) % 8]; }
static void clock_at(int minutes, char *out) { const int m = ((minutes % 1440) + 1440) % 1440; snprintf(out, 6, "%02d:%02d", m / 60, m % 60); }
/* when: highest, sets, rises, from the times after the load and the place's clock then (draw.js when) */
static void when_of(const obj_t *o, float m, int clock, char *out, size_t size) {
    float az, alt;
    now_at(o, m, &az, &alt);
    char c[8];
    if (o->never) snprintf(out, size, "NOT UP HERE");
    else if (alt > 0.f) {
        if (o->high >= 0 && o->high > m && o->high_alt > alt + 1.f) { clock_at(clock + o->high, c); snprintf(out, size, "HIGHEST %s", c); }
        else if (o->set >= 0 && o->set > m) { clock_at(clock + o->set, c); snprintf(out, size, "SETS %s", c); }
        else snprintf(out, size, "UP ALL NIGHT");
    } else if (o->rise >= 0 && o->rise > m) { clock_at(clock + o->rise, c); snprintf(out, size, "RISES %s", c); }
    else snprintf(out, size, "NOT UP TONIGHT");
}
static void dome_xy(float az, float alt, float *x, float *y) {
    const float rr = DOME_R * fminf(1.12f, fmaxf(0.f, (90.f - alt) / 90.f));
    *x = rr * sinf(az * DEG); *y = DOME_Y + rr * cosf(az * DEG);
}
static void where_face(scene_t *sc, const kind_t *k, const obj_t *o, float m, int clock) {
    ring(sc, 0.f, DOME_Y, DOME_R, INK_RGB);
    ring(sc, 0.f, DOME_Y, DOME_R * 2.f / 3.f, scale(GRID_RGB, 0.8f));
    ring(sc, 0.f, DOME_Y, DOME_R / 3.f, scale(GRID_RGB, 0.8f));
    static const char *const NESW[4] = { "N", "E", "S", "W" };
    for (int q = 0; q < 4; q++) { const float a = q * 90.f * DEG, R = DOME_R + 0.06f; text(sc, NESW[q], R * sinf(a), DOME_Y + R * cosf(a), 0.05f, q ? scale(INK_RGB, 0.8f) : GOLD_RGB); }
    float az, alt, x, y;
    now_at(o, m, &az, &alt);
    /* the path from now, while it is up: the agent's points from the one now on */
    const int from = (int)fmaxf(0.f, m / (float)o->step);
    float run[ATLAS_PATH + 1][2];
    int nr = 0, hi = -1;
    for (int q = from; q <= o->npath; q++) {
        float a2, h2;
        if (q == from) { a2 = az; h2 = alt; } else if (q < o->npath) { a2 = o->path[q][0] * 2.f; h2 = o->path[q][1] - 90.f; } else h2 = -1.f, a2 = 0.f;
        if (h2 > 0.f) { dome_xy(a2, h2, &run[nr][0], &run[nr][1]); nr++; if (q > from && (hi < 0 || h2 > o->path[hi][1] - 90.f)) hi = q; }
        if ((h2 <= 0.f || q == o->npath) && nr) {
            if (nr > 1) { scene_item_t *l = line(sc, nr, scale(k->tint, 0.75f)); if (l) memcpy(sc->pts[l->pts], run, sizeof(float) * 2 * (size_t)nr); }
            nr = 0;
        }
    }
    if (hi >= 0 && o->high > m && o->high_alt > alt + 1.f) { dome_xy(o->path[hi][0] * 2.f, o->path[hi][1] - 90.f, &x, &y); ring(sc, x, y, 0.018f, scale(k->tint, 0.8f)); }
    if (alt > 0.f) { dome_xy(az, alt, &x, &y); disc(sc, x, y, 0.03f, k->tint); ring(sc, x, y, 0.055f, scale(k->tint, 0.8f)); }
    else { dome_xy(az, -6.f, &x, &y); ring(sc, x, y, 0.03f, scale(k->tint, 0.6f)); }
    char w[32], wn[24];
    text(sc, o->name, 0.f, -0.48f, fit(o->name, 0.06f, 1.2f), scale(WHITE_RGB, 0.9f));
    if (alt > 0.f) snprintf(w, sizeof w, "%s %d UP", dir_of(az), (int)lrintf(alt)); else snprintf(w, sizeof w, "BELOW %s", dir_of(az));
    text(sc, w, 0.f, -0.595f, fit(w, 0.062f, 1.15f), alt > 0.f ? k->tint : DIM_RGB);
    when_of(o, m, clock, wn, sizeof wn);
    text(sc, wn, 0.f, -0.705f, 0.045f, INK_RGB);
}

void atlas_frame(scene_t *sc, float dt, bool behind) {
    const bool shown = sc && atlas_item(sc) >= 0;
    lock();
    S.since += dt;
    bool gone = false;
    if (shown) { S.shown = true; S.orphan = 0.f; }
    else if (S.nkinds && (S.orphan += dt) > (S.shown ? ATLAS_ORPHAN_S : ATLAS_KEEP_S)) gone = true;   /* the screen gone */
    unlock();
    reap(gone);
    if (gone) { ATLAS_FREE(s_dots); s_dots = NULL; s_dots_of = 0; }
    lock();
    if (!shown || !S.nkinds) { unlock(); return; }
    if (!behind) { S.s_level = S.level; S.s_kind = S.kind; S.s_index = S.index; S.s_face = S.face; }
    const kind_t kd = S.kinds[S.s_kind < S.nkinds ? S.s_kind : 0];   /* (a copy: a load replaces the header, never what it points to, this frame) */
    const int s_level = S.s_level, s_index = S.s_index, s_face = S.s_face, clock = S.place ? S.clock : -1;
    const uint32_t store = S.serial;
    const float m = S.since / 60.f;
    unlock();
    const kind_t *k = &kd;
    s_store = store;
    sc->npts = sc->npts_fixed;                            /* (scene_beat put the items back to the layout's; the points too) */
    const bool objects = s_level == ATLAS_AT_OBJECTS && k->nobj > 0;
    const obj_t *o = objects ? &k->obj[s_index < k->nobj ? s_index : 0] : NULL;
    /* what is kept: redrawn in place when the card, the face, the store or (on the where face, the kinds' counts)
     * the minute changes */
    const uint32_t parts[] = { (uint32_t)s_level, (uint32_t)(uintptr_t)kd.obj, (uint32_t)s_index, (uint32_t)s_face, store,
                               (uint32_t)(s_face == ATLAS_WHERE || !objects ? (int)m : 0), (uint32_t)(uintptr_t)o };
    uint32_t key = 2166136261u;
    for (size_t q = 0; q < sizeof parts / sizeof parts[0]; q++) key = (key ^ parts[q]) * 16777619u;
    static uint32_t serials;
    if (key != S.key) { S.key = key; sc->look = sc->serial = 0x80000000u | ++serials; }   /* (a new picture: the change the drawer spreads over frames) */
    if (!objects) {
        /* the kinds: the best known of the one chosen, small, the enter mark, its name, how many (and up now) */
        if (k->nobj) draw_object(sc, k, &k->obj[0], 0.f, 0.42f, 0.34f, 0.85f);
        for (int q = 0; q <= 8; q++) { const float a = q * 45.f * DEG, rr = q < 8 ? 0.062f : 0.f; disc(sc, rr * cosf(a), rr * sinf(a) + 0.04f, q < 8 ? 0.012f : 0.019f, GOLD_RGB); }
        text(sc, k->name, 0.f, -0.2f, fit(k->name, 0.085f, 1.1f), k->tint);
        char cnt[32];
        int up = 0;
        for (int q = 0; q < k->nobj; q++) up += up_now(&k->obj[q], m);
        if (clock >= 0 && k->nobj) snprintf(cnt, sizeof cnt, "%d  %d UP NOW", k->count ? k->count : k->nobj, up);
        else snprintf(cnt, sizeof cnt, "%d", k->count ? k->count : k->nobj);
        text(sc, cnt, 0.f, -0.34f, 0.05f, INK_RGB);
    } else {
        text(sc, k->name, 0.f, ATLAS_TOP_Y, fit(k->name, 0.05f, 0.62f), scale(k->tint, 0.9f));
        if (s_face == ATLAS_WHERE && o->where && clock >= 0) where_face(sc, k, o, m, clock);
        else {
            draw_object(sc, k, o, 0.f, CARD_Y, CARD_K, 1.f);
            text(sc, o->name, 0.f, -0.5f, fit(o->name, 0.075f, 1.25f), WHITE_RGB);
            text(sc, o->facts, 0.f, -0.615f, 0.05f, INK_RGB);
            char st[32];
            static const char *const SEE[3] = { "EYE", "BINOCULARS", "TELESCOPE" };
            const bool up = clock >= 0 && up_now(o, m);
            snprintf(st, sizeof st, "%s%s", SEE[o->see % 3], clock >= 0 && o->where ? (up ? "  UP NOW" : "  DOWN") : "");
            if (sc->nchars + (int)strlen(st) > SCENE_LETTERS) snprintf(st, sizeof st, "%s", SEE[o->see % 3]);   /* (a long name: what shows it, alone) */
            text(sc, st, 0.f, -0.715f, 0.042f, up ? k->tint : DIM_RGB);
        }
    }
}
