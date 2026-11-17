/*
 * SPDX-License-Identifier: MIT
 *
 * scene_player.c: a package's layout on screen (scene_player.h).
 * Plain C: built into the firmware and by test/host/scene_test.c.
 */
#include "scene_player.h"
#include "glyph_font.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define SCENE_CALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#define SCENE_FREE(p)   heap_caps_free(p)
#else
#define SCENE_CALLOC(n) calloc(1, (n))
#define SCENE_FREE(p)   free(p)
#endif

#define DEG 0.017453293f

/* ── build: a layout + params → resolved items ──────────────────────────────── */

typedef struct { const pkg_t *p; const pkg_layout_t *l; const cJSON *pv[PKG_PARAMS_MAX]; } bind_t;

static bool name_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > PKG_KEY_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

/* text values reach PKG_SAY_MAX (a spot's reading); a text item still takes SCENE_LETTERS */
static bool scalar_ok(const cJSON *v) {
    return (cJSON_IsNumber(v) && isfinite(v->valuedouble)) || (cJSON_IsString(v) && pkg_text_ok(v->valuestring, PKG_SAY_MAX));
}

/* every param declared, present, of its type; list elements flat objects of numbers / text */
static bool check_params(bind_t *b, const cJSON *params) {
    if (params && !cJSON_IsObject(params)) return false;
    for (const cJSON *c = params ? params->child : NULL; c; c = c->next) {
        int i = -1;
        for (int q = 0; q < b->l->nparams && i < 0; q++) if (!strcmp(c->string, b->l->params[q].name)) i = q;
        if (i < 0 || b->pv[i]) return false;                  /* unknown, or twice */
        b->pv[i] = c;
    }
    for (int i = 0; i < b->l->nparams; i++) {
        const cJSON *v = b->pv[i];
        switch (b->l->params[i].type) {
        case PP_NUMBER: if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble)) return false; break;
        case PP_TEXT:   if (!cJSON_IsString(v) || !pkg_text_ok(v->valuestring, PKG_SAY_MAX)) return false; break;
        default:
            if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) > SCENE_LIST_MAX) return false;
            for (const cJSON *e = v->child; e; e = e->next) {
                int n = 0;
                if (!cJSON_IsObject(e)) return false;
                for (const cJSON *f = e->child; f; f = f->next, n++) if (n >= SCENE_FIELDS || !name_ok(f->string) || !scalar_ok(f)) return false;
            }
        }
    }
    return true;
}

/* the JSON a bound value comes from: a param, or the element's field; NULL = a literal / unset */
static const cJSON *source(const bind_t *b, const pkg_val_t *v, const cJSON *el) {
    if (v->src == PV_PARAM) return b->pv[v->idx];
    if (v->src == PV_ELEM) return cJSON_GetObjectItemCaseSensitive(el, v->s);
    return NULL;
}

static bool num(const bind_t *b, const pkg_val_t *v, const cJSON *el, int pf, float dflt, float *out) {
    if (v->src == PV_NONE) { *out = dflt; return true; }
    if (v->src == PV_NUM) { *out = v->num; return true; }
    const cJSON *j = source(b, v, el);
    if (!cJSON_IsNumber(j)) return false;
    *out = (float)j->valuedouble;
    return pkg_num_ok(pf, *out);
}

static const char *bound_text(const bind_t *b, const pkg_val_t *v, const cJSON *el) {
    const cJSON *j = source(b, v, el);
    return cJSON_IsString(j) ? j->valuestring : NULL;
}

/* a literal or bound text field's value (NULL: unset or not text) */
static const char *str_of(const bind_t *b, const pkg_val_t *v, const cJSON *el) {
    return v->src == PV_STR ? v->s : v->src == PV_NONE ? NULL : bound_text(b, v, el);
}

/* s into the scene's spot text: its offset and length (max: the field's own limit) */
static bool say_add(scene_t *sc, const char *s, size_t max, uint16_t *off, uint16_t *n) {
    const size_t len = s ? strlen(s) : 0;
    if (!len || len > max || sc->nsay + (int)len > SCENE_SAY_CHARS) return false;
    *off = (uint16_t)sc->nsay; *n = (uint16_t)len;
    memcpy(sc->say + sc->nsay, s, len); sc->nsay += (int)len;
    return true;
}

/* name's highlight group in sc (1..), added if new; 0: not a name, or too many */
static int group_of(scene_t *sc, const char *name, size_t len) {
    if (!len || len > PKG_KEY_MAX) return 0;
    for (size_t i = 0; i < len; i++) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    for (int g = 0; g < sc->ngroups; g++) if (strlen(sc->groups[g]) == len && !memcmp(sc->groups[g], name, len)) return g + 1;
    if (sc->ngroups >= SCENE_GROUPS) return 0;
    memcpy(sc->groups[sc->ngroups], name, len);
    sc->groups[sc->ngroups][len] = '\0';
    return ++sc->ngroups;
}

static bool resolve(scene_t *sc, const bind_t *b, const pkg_item_t *it, const cJSON *el) {
    if (sc->nitems >= SCENE_ITEMS_MAX) return false;
    scene_item_t *o = &sc->items[sc->nitems];
    *o = (scene_item_t){ .k = it->k, .asset = -1, .rgb = 0xffffff };
    const pkg_val_t *f = it->f;
    float n;
    bool ok = num(b, &f[PF_ON], el, PF_ON, 0.f, &o->on) && num(b, &f[PF_OFF], el, PF_OFF, 0.f, &o->off) &&
              num(b, &f[PF_R], el, PF_R, 0.f, &o->r) && num(b, &f[PF_W], el, PF_W, 0.03f, &o->w) &&
              num(b, &f[PF_A0], el, PF_A0, 0.f, &o->a0) && num(b, &f[PF_A1], el, PF_A1, 360.f, &o->a1) &&
              num(b, &f[PF_LEN], el, PF_LEN, 0.1f, &o->len) && num(b, &f[PF_N], el, PF_N, 12.f, &n) &&
              num(b, &f[PF_SIZE], el, PF_SIZE, 0.12f, &o->size) && num(b, &f[PF_LIT], el, PF_LIT, 1.f, &o->lit) &&
              num(b, &f[PF_SIDE], el, PF_SIDE, 1.f, &o->side) && num(b, &f[PF_SPIN], el, PF_SPIN, 0.f, &o->spin) &&
              num(b, &f[PF_ROT], el, PF_ROT, 0.f, &o->rot) && num(b, &f[PF_GROW], el, PF_GROW, PKG_GROW, &o->grow);
    if (!ok) return false;
    o->n = (int)n;
    if (it->polar) {
        float a, r;
        if (!num(b, &f[PF_PA], el, PF_PA, 0.f, &a) || !num(b, &f[PF_PR], el, PF_PR, 0.f, &r)) return false;
        o->x = r * cosf(a * DEG); o->y = r * sinf(a * DEG);
    } else if (!num(b, &f[PF_X], el, PF_X, 0.f, &o->x) || !num(b, &f[PF_Y], el, PF_Y, 0.f, &o->y)) return false;
    if (f[PF_COLOR].src == PV_COLOR) o->rgb = f[PF_COLOR].rgb;
    else if (f[PF_COLOR].src != PV_NONE && !pkg_color(bound_text(b, &f[PF_COLOR], el), &o->rgb)) return false;
    if (f[PF_ASSET].src == PV_ASSET) o->asset = f[PF_ASSET].idx;
    else if (f[PF_ASSET].src != PV_NONE) {
        const char *name = bound_text(b, &f[PF_ASSET], el);
        for (int i = 0; name && i < b->p->nassets && o->asset < 0; i++) if (!strcmp(name, b->p->assets[i].name)) o->asset = (int16_t)i;
        if (o->asset < 0) return false;                         /* names nothing in the package */
    }
    if (it->k == PK_BALL && o->asset >= 0 && b->p->assets[o->asset].kind != PA_IMAGE) return false;
    if (it->k == PK_SPOT) {
        if (!say_add(sc, str_of(b, &f[PF_SAY], el), PKG_SAY_MAX, &o->str, &o->nstr)) return false;
        const char *id = str_of(b, &f[PF_ID], el);
        if (id && !say_add(sc, id, PKG_STR_MAX, &o->sid, &o->nsid)) return false;
    }
    const char *grp = str_of(b, &f[PF_GROUP], el), *lights = str_of(b, &f[PF_LIGHTS], el);
    if (grp && !(o->grp = (uint8_t)group_of(sc, grp, strlen(grp)))) return false;
    o->when_lit = f[PF_WHEN].src != PV_NONE;
    for (int n = 0; lights && *lights; n++) {           /* group names, spaces between */
        while (*lights == ' ') lights++;
        const size_t len = strcspn(lights, " ");
        if (!len) break;
        if (n >= SCENE_LIGHTS || !(o->lights[n] = (uint8_t)group_of(sc, lights, len))) return false;
        lights += len;
    }
    if (it->k == PK_TEXT) {
        const char *s = str_of(b, &f[PF_STR], el);
        size_t len = s ? strlen(s) : 0;
        if (!len || sc->nchars + (int)len > SCENE_LETTERS) return false;
        o->str = (uint16_t)sc->nchars; o->nstr = (uint16_t)len;
        memcpy(sc->chars + sc->nchars, s, len); sc->nchars += (int)len;
    }
    if (it->npts) {
        if (sc->npts + it->npts > SCENE_PTS_MAX) return false;
        o->pts = (uint16_t)sc->npts; o->npts = it->npts;
        for (int i = 0; i < it->npts; i++, sc->npts++)
            if (!num(b, &it->pts[i][0], el, PF_X, 0.f, &sc->pts[sc->npts][0]) || !num(b, &it->pts[i][1], el, PF_Y, 0.f, &sc->pts[sc->npts][1]))
                return false;
    }
    sc->nitems++;
    return true;
}

scene_t *scene_build(pkg_t *p, const char *layout, const cJSON *params, float ttl_s, const char **err) {
    const pkg_layout_t *l = p && layout ? pkg_layout(p, layout) : NULL;
    if (!l) { *err = "no_layout"; return NULL; }
    bind_t b = { p, l, { 0 } };
    if (!check_params(&b, params)) { *err = "bad_params"; return NULL; }
    scene_t *sc = SCENE_CALLOC(sizeof(scene_t));
    if (!sc) { *err = "oom"; return NULL; }
    for (int i = 0; i < l->nitems; i++) {
        const pkg_item_t *it = &l->items[i];
        bool ok = true;
        if (it->each < 0) ok = resolve(sc, &b, it, NULL);
        else for (const cJSON *el = b.pv[it->each]->child; el && ok; el = el->next) ok = resolve(sc, &b, it, el);
        if (!ok) { SCENE_FREE(sc); *err = "bad_params"; return NULL; }
    }
    pkg_ref(p);
    sc->pkg = p;
    sc->focus = -1; sc->grow = 1.f;
    static unsigned serial;
    sc->serial = ++serial ? serial : ++serial;
    snprintf(sc->pkg_id, sizeof(sc->pkg_id), "%s", p->id);
    snprintf(sc->layout, sizeof(sc->layout), "%s", l->name);
    sc->dur = l->dur;
    sc->caption_y = l->caption_y;
    sc->ttl = ttl_s > 0.f ? fminf(ttl_s, SCENE_TTL_MAX) : SCENE_TTL_DEFAULT;
    *err = NULL;
    return sc;
}

void scene_free(scene_t *sc) {
    if (!sc) return;
    pkg_unref(sc->pkg);
    SCENE_FREE(sc);
}

float scene_end(const scene_t *sc) { return sc->dur > 0.f && sc->dur < sc->ttl ? sc->dur : sc->ttl; }

int scene_spot_at(const scene_t *sc, float t, float x, float y) {
    int best = -1;
    float bd = 0.f;
    for (int n = 0; sc && n < sc->nitems; n++) {
        const scene_item_t *it = &sc->items[n];
        if (it->k != PK_SPOT || t < it->on || (it->off > 0.f && t >= it->off)) continue;
        float dx = x - it->x, dy = y - it->y;
        if (it->npts == 2) {                            /* along a segment */
            const float *a = sc->pts[it->pts], *e = sc->pts[it->pts + 1], vx = e[0] - a[0], vy = e[1] - a[1], l2 = vx * vx + vy * vy;
            float u = l2 > 1e-9f ? ((x - a[0]) * vx + (y - a[1]) * vy) / l2 : 0.f;
            u = u < 0.f ? 0.f : u > 1.f ? 1.f : u;
            dx = x - a[0] - u * vx; dy = y - a[1] - u * vy;
        }
        const float d = dx * dx + dy * dy;
        if (d <= it->r * it->r && (best < 0 || it->r < sc->items[best].r || (it->r == sc->items[best].r && d < bd))) { best = n; bd = d; }
    }
    return best;
}

/* ── the player ────────────────────────────────────────────────────────────── */

static void start(scene_player_t *p, scene_t *sc) { p->cur = sc; p->t = 0.f; p->started = sc != NULL; p->gen++; }

void scene_clear(scene_player_t *p) {
    scene_free(p->cur); p->cur = NULL;
    for (int i = 0; i < p->nq; i++) scene_free(p->q[i]);
    p->nq = 0;
}

void scene_play(scene_player_t *p, scene_t *sc) { scene_clear(p); start(p, sc); }

bool scene_queue(scene_player_t *p, scene_t *sc) {
    if (!p->cur) { start(p, sc); return true; }
    if (p->nq >= SCENE_QUEUE_MAX) return false;
    p->q[p->nq++] = sc;
    return true;
}

void scene_tick(scene_player_t *p, float dt) {
    if (!p->cur || (p->t += dt) < scene_end(p->cur)) return;
    scene_free(p->cur);
    scene_t *next = NULL;
    if (p->nq) { next = p->q[0]; memmove(p->q, p->q + 1, sizeof(p->q[0]) * (size_t)--p->nq); }
    start(p, next);
}

bool scene_take_start(scene_player_t *p) { bool s = p->started; p->started = false; return s; }

/* ── fill ──────────────────────────────────────────────────────────────────── */

/* The canvas the items paint: the sim's grid split into its braille dots (2 x 4 a cell), so
 * a line or a glyph lands on the dots it covers instead of a whole cell's blur. scene_fill
 * folds it back into the cells (the fluid's target) and their dots (scene_fill's dots). */
typedef struct {
    int W, H, N, stage;
    float *xs, *ys;                    /* dot centres in -1..1                             */
    uint8_t *mask;                     /* 1: inside the glass; | 2: its ink is a kept dot   */
    int16_t *T;
    uint32_t *tcol;
    uint8_t keep;                      /* the item painting now keeps its dots in high definition */
} canvas_t;
typedef struct { canvas_t *s; float soft; scene_shapes_t *hd; bool paint; } ctx_t;   /* paint: the canvas too, not just shapes */

/* Painter's order, as the reference player (packages/tools/play.mjs): an item covers what is
 * under it by `a` (its coverage, 0..1, the soft edge), leaving ink v in colour rgb. A black
 * disc leaves no ink (an eclipse, a transit); a ball covers its whole disc, dark side too. */
static inline void paint(canvas_t *s, int k, float a, float v, uint32_t rgb) {
    if (a <= 0.004f || !s->mask[k]) return;
    if (a >= 0.5f) s->mask[k] = (uint8_t)(1 | s->keep << 1);
    if (a >= 0.996f) {                                  /* covered: most dots, no blend */
        s->T[k] = fq(v, FQ_UNIT);
        s->tcol[k] = (rgb >> 16 & 255) | (rgb & 0xff00) | (rgb & 255) << 16;
        return;
    }
    const float t = s->T[k] * (1.f / FQ_UNIT);
    s->T[k] = fq(t + (v - t) * a, FQ_UNIT);
    const uint32_t c = s->tcol[k];
    const float r = (c & 255) / 255.f, g = (c >> 8 & 255) / 255.f, b = (c >> 16 & 255) / 255.f;
    s->tcol[k] = fluid_pack_rgba8(r + ((rgb >> 16 & 255) / 255.f - r) * a, g + ((rgb >> 8 & 255) / 255.f - g) * a,
                                  b + ((rgb & 255) / 255.f - b) * a, 0.f);
}

/* the scene in high definition: one more shape (glass units to stage px, y down) */
static inline float spx(const ctx_t *c, float v) { return v * 0.5f * c->s->stage; }
/* a stroke's half width at the panel's pixels: a hairline (no wider than a dot) is one fine
 * anti-aliased pixel */
#define HD_HAIR 0.5f
static inline float hd_hw(const ctx_t *c, float hw) {
    return hw <= 1.f / (float)(c->s->W < c->s->H ? c->s->W : c->s->H) ? HD_HAIR : fmaxf(HD_HAIR, spx(c, hw));
}
/* the cells a tapped spot's drawing covers (scene_shapes_t.still): marked while its items are painted */
static uint8_t *s_mark;                                /* W x H: 2 under it, then 1 round it */
static bool s_mark_on;                                 /* the item painting now is the tapped spot's */
static void mark_shape(const canvas_t *cv, const fluid_shape_t *sh) {
    const int W = cv->W / 2, H = cv->H / 4;
    const float cw = (float)cv->stage / W, ch = (float)cv->stage / H, reach = 0.5f * sqrtf(cw * cw + ch * ch);
    int16_t b[4];
    fluid_shape_box(sh, 0, 0, b);
    int i0 = (int)floorf((b[0] - reach) / cw), i1 = (int)floorf((b[1] + reach) / cw), j0 = (int)floorf((b[2] - reach) / ch), j1 = (int)floorf((b[3] + reach) / ch);
    i0 = i0 < 0 ? 0 : i0; j0 = j0 < 0 ? 0 : j0; i1 = i1 > W - 1 ? W - 1 : i1; j1 = j1 > H - 1 ? H - 1 : j1;
    for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
        const float px = (i + 0.5f) * cw, py = (j + 0.5f) * ch;
        float d;
        switch (sh->k) {
        case FLUID_SHAPE_SEG: {
            const float vx = sh->x1 - sh->x, vy = sh->y1 - sh->y, l2 = vx * vx + vy * vy;
            float u = l2 > 1e-6f ? ((px - sh->x) * vx + (py - sh->y) * vy) / l2 : 0.f;
            u = u < 0.f ? 0.f : u > 1.f ? 1.f : u;
            d = hypotf(px - sh->x - u * vx, py - sh->y - u * vy) - sh->hw;
            break;
        }
        case FLUID_SHAPE_RING: d = fabsf(hypotf(px - sh->x, py - sh->y) - sh->r) - sh->hw; break;
        case FLUID_SHAPE_DISC: d = hypotf(px - sh->x, py - sh->y) - sh->r; break;
        default: d = fmaxf(fabsf(px - sh->x) - 0.5f * sh->w, fabsf(py - sh->y) - 0.5f * sh->h); break;
        }
        if (d <= reach) s_mark[j * W + i] = 2;
    }
}
static void shape(const ctx_t *c, fluid_shape_t sh) {
    if (s_mark && s_mark_on && !(sh.k == FLUID_SHAPE_DISC && !(sh.rgb & 0xffffffu))) mark_shape(c->s, &sh);
    scene_shapes_t *hd = c->hd;
    if (!hd || hd->n < 0) return;
    if (hd->n >= FLUID_MAX_SHAPES) { hd->n = -1; return; }
    hd->shapes[hd->n++] = sh;
}
static void seg_shape(const ctx_t *c, float ax, float ay, float bx, float by, float hw, uint32_t rgb) {
    shape(c, (fluid_shape_t){ .k = FLUID_SHAPE_SEG, .rgb = FLUID_RGB | rgb, .x = spx(c, ax + 1.f), .y = spx(c, 1.f - ay),
                              .x1 = spx(c, bx + 1.f), .y1 = spx(c, 1.f - by), .hw = hd_hw(c, hw) });
}
/* a letter or a symbol (its box, stage px): a symbol fills it, a letter 0.8 x 0.72 of it, centred */
static void letter_shape(const ctx_t *c, const fluid_letter_t *L) {
    const uint8_t *gl = L->mask ? L->mask : gc_font5x7(L->ch);
    if (!gl || L->ch == ' ') return;
    shape(c, (fluid_shape_t){ .k = L->mask ? FLUID_SHAPE_MASK : FLUID_SHAPE_CHAR, .acc = L->acc, .mw = L->mask ? L->mw : 5,
                              .mh = L->mask ? L->mh : 7, .rgb = L->rgb, .x = L->x + L->w * 0.5f, .y = L->y + L->h * 0.5f,
                              .w = L->mask ? L->w : L->w * 0.8f, .h = L->mask ? L->h : L->h * 0.72f, .co = 1.f, .mask = gl });
}

/* the cells whose centres lie in a glass-space box (y up) */
static void cells(const canvas_t *s, float x0, float y0, float x1, float y1, int *i0, int *i1, int *j0, int *j1) {
    *i0 = (int)floorf((x0 + 1.f) * 0.5f * s->W - 0.5f); *i1 = (int)ceilf((x1 + 1.f) * 0.5f * s->W - 0.5f);
    *j0 = (int)floorf((1.f - y1) * 0.5f * s->H - 0.5f); *j1 = (int)ceilf((1.f - y0) * 0.5f * s->H - 0.5f);
    if (*i0 < 0) *i0 = 0;
    if (*j0 < 0) *j0 = 0;
    if (*i1 > s->W - 1) *i1 = s->W - 1;
    if (*j1 > s->H - 1) *j1 = s->H - 1;
}

/* A stroke no wider than a dot is a hairline: it walks the dot lattice as drawille does, one
 * dot a step along its major axis and 8-connected (a DDA in dot coordinates: each step's dot
 * the nearest to the line), solid. Along a path (a curve, a ring) a dot whose neighbours touch
 * each other is a corner and drops, so a bend is no thicker than a straight run. */
static inline float hair(const canvas_t *s) { return 1.f / (float)(s->W < s->H ? s->W : s->H); }   /* half a dot */
typedef struct { canvas_t *s; uint32_t rgb; int n, skip, ax, ay, bx, by; float px, py; } hair_t;   /* a: drawn, b: pending, p: the pen */
static inline int rnd(float v) { return (int)floorf(v + 0.5f); }

static void hair_put(hair_t *h, int x, int y) {
    if (h->skip) { h->skip--; return; }
    if (x >= 0 && y >= 0 && x < h->s->W && y < h->s->H) paint(h->s, y * h->s->W + x, 1.f, SCENE_INK, h->rgb);
}
static void hair_dot(hair_t *h, int x, int y) {
    if (h->n && x == h->bx && y == h->by) return;
    if (h->n == 2 && abs(x - h->ax) <= 1 && abs(y - h->ay) <= 1) {     /* b is a corner (or a spike back to a) */
        if (x == h->ax && y == h->ay) h->n = 1;
        h->bx = x; h->by = y;
        return;
    }
    if (h->n) { hair_put(h, h->bx, h->by); h->ax = h->bx; h->ay = h->by; }
    h->n += h->n < 2;
    h->bx = x; h->by = y;
}
/* the pen to (x, y) (glass units, y up) */
static void hair_to(hair_t *h, float x, float y) {
    const float X = (x + 1.f) * 0.5f * h->s->W - 0.5f, Y = (1.f - y) * 0.5f * h->s->H - 0.5f;
    if (h->n) {
        const float dx = X - h->px, dy = Y - h->py;
        const bool xm = fabsf(dx) >= fabsf(dy);
        const float a = xm ? h->px : h->py, b = xm ? h->py : h->px, da = xm ? dx : dy, k = da != 0.f ? (xm ? dy : dx) / da : 0.f;
        const int m1 = rnd(a + da), st = m1 >= rnd(a) ? 1 : -1;
        for (int m = rnd(a); ; m += st) {
            const int o = rnd(b + k * (m - a));
            if (xm) hair_dot(h, m, o);
            else hair_dot(h, o, m);
            if (m == m1) break;
        }
    } else hair_dot(h, rnd(X), rnd(Y));
    h->px = X; h->py = Y;
}
static void hair_end(hair_t *h) { if (h->n) hair_put(h, h->bx, h->by); h->n = 0; }

static void segment(const ctx_t *c, float ax, float ay, float bx, float by, float hw, uint32_t rgb) {
    canvas_t *s = c->s;
    seg_shape(c, ax, ay, bx, by, hw, rgb);
    if (!c->paint) return;
    if (hw <= hair(s)) { hair_t h = { .s = s, .rgb = rgb }; hair_to(&h, ax, ay); hair_to(&h, bx, by); hair_end(&h); return; }
    const float reach = hw + c->soft, dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy, il2 = l2 > 1e-9f ? 1.f / l2 : 0.f;
    int i0, i1, j0, j1;
    cells(s, fminf(ax, bx) - reach, fminf(ay, by) - reach, fmaxf(ax, bx) + reach, fmaxf(ay, by) + reach, &i0, &i1, &j0, &j1);
    for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
        const float px = s->xs[i] - ax, py = -s->ys[j] - ay;
        float u = (px * dx + py * dy) * il2;
        u = u < 0.f ? 0.f : u > 1.f ? 1.f : u;
        const float ex = px - u * dx, ey = py - u * dy, d = sqrtf(ex * ex + ey * ey);
        if (d < reach) paint(s, j * s->W + i, 1.f - fluid_smooth(hw, reach, d), SCENE_INK, rgb);
    }
}

/* ring / arc: |r − distance| within w/2, at angles a0..a1 (a1 − a0 ≥ 360: all of it). Each row
 * visits only its band, |x| between the inner and the outer circle: a wheel's ring is most of
 * the screen's bounding box but a sliver of its dots. A hairline walks it a dot a step (a
 * whole ring from one step before its start to one after, the first dot left to the end). */
static void ring(const ctx_t *c, const scene_item_t *it, bool arc) {
    canvas_t *s = c->s;
    float span = it->a1 - it->a0;
    const bool whole = !arc || span >= 360.f;
    span = whole ? 360.f : fmodf(fmodf(span, 360.f) + 360.f, 360.f);
    const float hw = it->w * 0.5f;
    const float cx = it->x, cy = it->y;                 /* its centre (the glass's, unless set) */
    shape(c, (fluid_shape_t){ .k = FLUID_SHAPE_RING, .rgb = FLUID_RGB | it->rgb, .x = spx(c, cx + 1.f), .y = spx(c, 1.f - cy),
                              .r = spx(c, it->r), .hw = hd_hw(c, hw), .a0 = it->a0, .span = span });
    if (!c->paint) return;
    if (hw <= hair(s)) {
        const int n = 8 + (int)(span * DEG * it->r * 0.5f * (s->W > s->H ? s->W : s->H));
        hair_t h = { .s = s, .rgb = it->rgb };
        for (int q = whole ? -1 : 0; q <= (whole ? n + 1 : n); q++) {
            const float a = (it->a0 + span * q / n) * DEG;
            hair_to(&h, cx + it->r * cosf(a), cy + it->r * sinf(a));
            if (q < 0) h.skip = 1;
        }
        hair_end(&h);
        return;
    }
    const float reach = hw + c->soft, ro = it->r + reach, rin = it->r > reach ? it->r - reach : 0.f;
    int i0, i1, j0, j1;
    cells(s, cx - ro, cy - ro, cx + ro, cy + ro, &i0, &i1, &j0, &j1);
    for (int j = j0; j <= j1; j++) {
        const float y = -s->ys[j] - cy, oo = ro * ro - y * y, ii = rin * rin - y * y;
        if (oo <= 0.f) continue;
        const float xo = sqrtf(oo), xi = ii > 0.f ? sqrtf(ii) : 0.f;
        /* left span -xo..-xi, right span xi..xo (one span -xo..xo when the row misses the hole) */
        for (int side = xi > 0.f ? 0 : 1; side < 2; side++) {
            const float lo = side ? (xi > 0.f ? xi : -xo) : -xo, hi = side ? xo : -xi;
            int a0 = (int)floorf((cx + lo + 1.f) * 0.5f * s->W - 0.5f), a1 = (int)ceilf((cx + hi + 1.f) * 0.5f * s->W - 0.5f);
            if (a0 < i0) a0 = i0;
            if (a1 > i1) a1 = i1;
            for (int i = a0; i <= a1; i++) {
                const float x = s->xs[i] - cx, d = fabsf(sqrtf(x * x + y * y) - it->r);
                if (d >= reach) continue;
                if (!whole) {
                    float a = fmodf(fluid_atan2(y, x) / DEG - it->a0, 360.f);
                    if (a < 0.f) a += 360.f;
                    if (a > span) continue;
                }
                paint(s, j * s->W + i, 1.f - fluid_smooth(hw, reach, d), SCENE_INK, it->rgb);
            }
        }
    }
}
static void disc(const ctx_t *c, float cx, float cy, float r, uint32_t rgb) {
    canvas_t *s = c->s;
    const float ro = r + c->soft * 0.5f;
    shape(c, (fluid_shape_t){ .k = FLUID_SHAPE_DISC, .rgb = FLUID_RGB | rgb, .x = spx(c, cx + 1.f), .y = spx(c, 1.f - cy), .r = spx(c, r) });
    if (!c->paint) return;
    int i0, i1, j0, j1;
    cells(s, cx - ro, cy - ro, cx + ro, cy + ro, &i0, &i1, &j0, &j1);
    for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
        const float x = s->xs[i] - cx, y = -s->ys[j] - cy, d = sqrtf(x * x + y * y);
        if (d < ro) paint(s, j * s->W + i, 1.f - fluid_smooth(r - c->soft * 0.5f, ro, d), rgb ? SCENE_INK : 0.f, rgb);
    }
}

static inline int px4(const pkg_asset_t *a, int x, int y) {
    const int p = y * a->w + x;
    return a->data[p >> 1] >> (p & 1 ? 0 : 4) & 15;
}
static inline int bit(const pkg_asset_t *a, int x, int y) {
    const int p = y * a->w + x;
    return a->data[p >> 3] >> (7 - (p & 7)) & 1;
}

/* asin to ~7e-5 rad (Abramowitz & Stegun 4.4.45): the sphere's map runs it for every dot */
static inline float fast_asin(float x) {
    const float a = fabsf(x), r = 1.5707963f - sqrtf(1.f - a) * (1.5707288f + a * (-0.2121144f + a * (0.0742610f - 0.0187293f * a)));
    return x < 0.f ? -r : r;
}
static inline uint32_t toward_white(uint32_t rgb, float k) {
    uint32_t o = 0;
    for (int q = 16; q >= 0; q -= 8) o |= (uint32_t)((rgb >> q & 255) + (255 - (rgb >> q & 255)) * k) << q;
    return o;
}
static inline uint32_t shade(uint32_t rgb, float k) {
    return (uint32_t)((rgb >> 16 & 255) * k) << 16 | (uint32_t)((rgb >> 8 & 255) * k) << 8 | (uint32_t)((rgb & 255) * k);
}

/* a sphere lit from one side (lit = the lit fraction of the face, side = which side), wearing
 * an image as an equirectangular map that turns `spin` degrees per second */
static void ball(const ctx_t *c, const scene_item_t *it, const pkg_asset_t *img, float t) {
    canvas_t *s = c->s;
    if (it->r <= 0.f) return;
    /* in high definition its dots stay (s->keep), over a black disc: it hides what is behind */
    shape(c, (fluid_shape_t){ .k = FLUID_SHAPE_DISC, .rgb = FLUID_RGB, .x = spx(c, it->x + 1.f), .y = spx(c, 1.f - it->y), .r = spx(c, it->r) });
    if (!c->paint) return;
    const float r = it->r, ro = r + c->soft * 0.5f, cphi = 2.f * it->lit - 1.f, sphi = sqrtf(fmaxf(0.f, 1.f - cphi * cphi));
    const float lx = it->side * sphi, lz = cphi, turn = it->spin * t * DEG;
    int i0, i1, j0, j1;
    cells(s, it->x - ro, it->y - ro, it->x + ro, it->y + ro, &i0, &i1, &j0, &j1);
    /* light, map and colour change slowly over the face: once a cell, at its centre; only the
     * outline is per dot */
    int ckey = -1;
    float ink = 0.f;
    uint32_t rgb = 0;
    for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
        const float x = s->xs[i] - it->x, y = -s->ys[j] - it->y, d = sqrtf(x * x + y * y);
        if (d >= ro) continue;
        const int key = (j >> 2) * s->W + (i >> 1);
        if (key != ckey) {
            ckey = key;
            const float cx = 0.5f * (s->xs[i & ~1] + s->xs[i | 1]) - it->x, cy = -0.5f * (s->ys[j & ~3] + s->ys[j | 3]) - it->y;
            float nx = cx / r, ny = cy / r, rr = nx * nx + ny * ny;
            if (rr > 0.999f) { const float k = 0.9995f / sqrtf(rr); nx *= k; ny *= k; rr = nx * nx + ny * ny; }
            /* the dark side: a faint earthshine, opaque all the same (it hides what is behind) */
            const float nz = sqrtf(1.f - rr), lit = fluid_smooth(-0.08f, 0.12f, nx * lx + nz * lz), lum = 0.18f + 0.82f * lit;
            uint32_t base = it->rgb;
            ink = SCENE_INK * (0.08f + 0.92f * lit);
            if (img) {
                float u = (fluid_atan2(nx, nz) + turn) * (0.5f / 3.1415927f) + 0.5f;
                u -= floorf(u);
                const int ix = (int)(u * img->w) % img->w, iy = (int)((0.5f - fast_asin(ny) / 3.1415927f) * img->h);
                const int q = px4(img, ix, iy < 0 ? 0 : iy >= img->h ? img->h - 1 : iy);
                if (q) base = img->pal[q];
                else ink = 0.f;                                 /* a hole in the image: nothing, as play.mjs */
            }
            rgb = shade(base, lum);
        }
        paint(s, j * s->W + i, 1.f - fluid_smooth(r - c->soft * 0.5f, ro, d), ink, rgb);
    }
}

/* an asset `size` wide, turned rot degrees: each dot's coverage from 2 × 2 samples (one, when
 * the dot is no bigger than an asset pixel) */
/* a small upright symbol (a planet, a sign on the wheel): drawn crisp on the panel, like a
 * letter, over ink in its box (the fluid gathers there, then the symbol shows whole) */
static bool crisp(const ctx_t *c, const scene_item_t *it, const pkg_asset_t *a, fluid_letter_t *letters, int *nl) {
    if (a->kind != PA_MASK || it->rot != 0.f || it->size > SCENE_CRISP_MAX || *nl >= FLUID_MAX_LETTERS) return false;
    canvas_t *s = c->s;
    const float w = it->size, h = it->size * a->h / a->w, k = 0.5f * s->stage;
    const float x0 = (it->x - 0.5f * w + 1.f) * k, y0 = (1.f - it->y - 0.5f * h) * k;
    letters[(*nl)++] = (fluid_letter_t){ '*', true, x0, y0, w * k, h * k, h * k, 0.f, FLUID_RGB | it->rgb, a->data, a->w, a->h };
    letter_shape(c, &letters[*nl - 1]);
    if (!c->paint) return true;
    const float cwp = (float)s->stage / s->W, chp = (float)s->stage / s->H;
    int i0 = (int)floorf(x0 / cwp), i1 = (int)ceilf((x0 + w * k) / cwp), j0 = (int)floorf(y0 / chp), j1 = (int)ceilf((y0 + h * k) / chp);
    i0 = i0 < 0 ? 0 : i0; j0 = j0 < 0 ? 0 : j0; i1 = i1 >= s->W ? s->W - 1 : i1; j1 = j1 >= s->H ? s->H - 1 : j1;
    for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) paint(s, j * s->W + i, 1.f, 0.62f, it->rgb);
    return true;
}

static void glyph(const ctx_t *c, const scene_item_t *it, const pkg_asset_t *a) {
    canvas_t *s = c->s;
    const float w = it->size, h = it->size * a->h / a->w, ext = 0.5f * sqrtf(w * w + h * h);
    const float co = cosf(it->rot * DEG), si = sinf(it->rot * DEG), cw = 2.f / s->W, ch = 2.f / s->H;
    if (a->kind == PA_MASK)
        shape(c, (fluid_shape_t){ .k = FLUID_SHAPE_MASK, .mw = (uint16_t)a->w, .mh = (uint16_t)a->h, .rgb = FLUID_RGB | it->rgb,
                                  .x = spx(c, it->x + 1.f), .y = spx(c, 1.f - it->y), .w = spx(c, w), .h = spx(c, h), .co = co, .si = si, .mask = a->data });
    if (!c->paint) return;
    /* a dot no wider than one of the asset's pixels: its centre says it all */
    const int ns = cw * a->w > w || ch * a->h > h ? 4 : 1;
    int i0, i1, j0, j1;
    cells(s, it->x - ext, it->y - ext, it->x + ext, it->y + ext, &i0, &i1, &j0, &j1);
    /* (u, v) is linear in the dot's position: stepped along each row, offset per sample */
    const float ux = co / w, uy = si / w, vx = si / h, vy = -co / h;
    float qu[4], qv[4];
    for (int q = 0; q < ns; q++) {
        const float ox = ns > 1 ? cw * ((q & 1) - 0.5f) * 0.5f : 0.f, oy = ns > 1 ? ch * ((q >> 1) - 0.5f) * 0.5f : 0.f;
        qu[q] = ux * ox + uy * oy; qv[q] = vx * ox + vy * oy;
    }
    for (int j = j0; j <= j1; j++) {
        const float dy = -s->ys[j] - it->y, dx0 = s->xs[i0] - it->x;
        float ru = 0.5f + ux * dx0 + uy * dy, rv = 0.5f + vx * dx0 + vy * dy;
        for (int i = i0; i <= i1; i++, ru += ux * cw, rv += vx * cw) {
            int cov = 0; float cr = 0.f, cg = 0.f, cb = 0.f;
            for (int q = 0; q < ns; q++) {
                const float u = ru + qu[q], v = rv + qv[q];
                if (u < 0.f || u >= 1.f || v < 0.f || v >= 1.f) continue;
                const int ax = (int)(u * a->w), ay = (int)(v * a->h);
                if (a->kind == PA_MASK) { cov += bit(a, ax, ay); continue; }
                const int p = px4(a, ax, ay);
                if (!p) continue;
                cov++;
                cr += a->pal[p] >> 16 & 255; cg += a->pal[p] >> 8 & 255; cb += a->pal[p] & 255;
            }
            if (!cov) continue;
            const uint32_t rgb = a->kind == PA_MASK ? it->rgb : (uint32_t)(cr / cov) << 16 | (uint32_t)(cg / cov) << 8 | (uint32_t)(cb / cov);
            paint(s, j * s->W + i, (float)cov / ns, SCENE_INK, rgb);
        }
    }
}

/* the reader's letter boxes (fluid_text_set_word): ink under each letter, and the letters
 * themselves (in lrgb) for the crisp overlay; size = the type size in glass units. (x, y) is
 * the run's centre, or letter orp's when orp >= 0 (that letter accented). */
static void text_run(const ctx_t *c, const char *str, int n, float x, float y, float size, uint32_t rgb, uint32_t lrgb,
                     int orp, fluid_letter_t *letters, int *nl) {
    canvas_t *s = c->s;
    const float fs = size * s->stage * 0.5f, adv = fs * 0.6f, lh = fs * 1.05f;
    const float x0 = (x + 1.f) * 0.5f * s->stage - (orp < 0 ? n * 0.5f : orp + 0.5f) * adv, y0 = (1.f - y) * 0.5f * s->stage - lh * 0.5f;
    for (int q = 0; q < n && *nl < FLUID_MAX_LETTERS; q++) {
        letters[(*nl)++] = (fluid_letter_t){ str[q], q == orp, x0 + q * adv, y0, adv, lh, fs, 0.f, lrgb, NULL, 0, 0 };
        letter_shape(c, &letters[*nl - 1]);
    }
    if (!c->paint) return;
    const float cwp = (float)s->stage / s->W, chp = (float)s->stage / s->H;
    int i0 = (int)floorf(x0 / cwp), i1 = (int)ceilf((x0 + n * adv) / cwp), j0 = (int)floorf(y0 / chp), j1 = (int)ceilf((y0 + lh) / chp);
    i0 = i0 < 0 ? 0 : i0; j0 = j0 < 0 ? 0 : j0; i1 = i1 >= s->W ? s->W - 1 : i1; j1 = j1 >= s->H ? s->H - 1 : j1;
    for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
        const float px = (i + 0.5f) * cwp, py = (j + 0.5f) * chp;
        if (py <= y0 || py >= y0 + lh) continue;
        const int li = (int)floorf((px - x0) / adv);
        if (li >= 0 && li < n && str[li] != ' ') paint(s, j * s->W + i, 1.f, 0.62f, rgb);
    }
}
static void text(const ctx_t *c, const scene_t *sc, const scene_item_t *it, fluid_letter_t *letters, int *nl) {
    text_run(c, sc->chars + it->str, it->nstr, it->x, it->y, it->size, it->rgb, FLUID_RGB | it->rgb, -1, letters, nl);
}

/* the subtitle: the reader's word (palette letters, the ORP in the accent on x = 0) at the
 * bottom, its type shrunk until its farthest corner is inside the glass */
static void caption(const ctx_t *c, const scene_caption_t *cap, float y, fluid_letter_t *letters, int *nl) {
    const int orp = cap->orp >= cap->n ? cap->n - 1 : cap->orp;
    const float reach = orp < 0 ? cap->n * 0.5f : fmaxf(orp + 0.5f, cap->n - orp - 0.5f);   /* in advances */
    float size = SCENE_CAPTION_SIZE;
    for (;;) {
        const float e = reach * 0.6f * size, yb = SCENE_CAPTION_Y - 0.525f * size;
        if (e * e + yb * yb <= 0.95f * 0.95f || size < 0.04f) break;
        size *= 0.92f;
    }
    const int n0 = c->hd && c->hd->n >= 0 ? c->hd->n : 0;
    text_run(c, cap->str, cap->n, 0.f, y, size, SCENE_CAPTION_RGB, 0, orp, letters, nl);
    for (int q = n0; c->hd && q < c->hd->n; q++) c->hd->shapes[q].tag = FLUID_TAG_CAPTION;   /* it forms from the fine dust */
}

/* Catmull-Rom through the points (8 segments a span), or a polyline (u = 1 alone: the next
 * point); a hairline is one path */
static void path(const ctx_t *c, const float (*p)[2], int n, bool smooth, float hw, uint32_t rgb) {
    if (n < 2) return;
    hair_t h = { .s = c->s, .rgb = rgb };
    const bool thin = hw <= hair(c->s);
    float px = p[0][0], py = p[0][1];
    if (thin && c->paint) hair_to(&h, px, py);
    for (int i = 0; i + 1 < n; i++) {
        const float *a = p[i > 0 ? i - 1 : 0], *b = p[i], *d = p[i + 1], *e = p[i + 2 < n ? i + 2 : n - 1];
        for (int q = smooth ? 1 : 8; q <= 8; q++) {
            const float u = q / 8.f, u2 = u * u, u3 = u2 * u;
            float xy[2];
            for (int k = 0; k < 2; k++)
                xy[k] = 0.5f * (2.f * b[k] + (d[k] - a[k]) * u + (2.f * a[k] - 5.f * b[k] + 4.f * d[k] - e[k]) * u2 +
                                (3.f * b[k] - a[k] - 3.f * d[k] + e[k]) * u3);
            if (thin) { if (c->paint) hair_to(&h, xy[0], xy[1]); seg_shape(c, px, py, xy[0], xy[1], hw, rgb); }
            else segment(c, px, py, xy[0], xy[1], hw, rgb);
            px = xy[0]; py = xy[1];
        }
    }
    hair_end(&h);
}

/* the canvas for a W x H grid, kept between frames (PSRAM: 7 bytes a dot) */
static canvas_t s_cv;
static const uint8_t *s_cv_mask;                       /* the sim mask it was built from */
static bool canvas_for(const fluid_sim_t *s) {
    const int W = 2 * s->W, H = 4 * s->H;
    if (s_cv.W != W || s_cv.H != H) {
        s_cv_mask = NULL;
        SCENE_FREE(s_cv.xs); SCENE_FREE(s_cv.ys); SCENE_FREE(s_cv.mask); SCENE_FREE(s_cv.T); SCENE_FREE(s_cv.tcol);
        s_cv = (canvas_t){ W, H, W * H, s->stage, SCENE_CALLOC(sizeof(float) * (size_t)W), SCENE_CALLOC(sizeof(float) * (size_t)H),
                           SCENE_CALLOC((size_t)W * H), SCENE_CALLOC(sizeof(int16_t) * (size_t)W * H), SCENE_CALLOC(sizeof(uint32_t) * (size_t)W * H), 0 };
        if (!s_cv.xs || !s_cv.ys || !s_cv.mask || !s_cv.T || !s_cv.tcol) {
            SCENE_FREE(s_cv.xs); SCENE_FREE(s_cv.ys); SCENE_FREE(s_cv.mask); SCENE_FREE(s_cv.T); SCENE_FREE(s_cv.tcol);
            s_cv = (canvas_t){ 0 };
            return false;
        }
        for (int i = 0; i < W; i++) s_cv.xs[i] = ((i + 0.5f) / W) * 2.f - 1.f;
        for (int j = 0; j < H; j++) s_cv.ys[j] = ((j + 0.5f) / H) * 2.f - 1.f;
    }
    s_cv.stage = s->stage;
    if (s_cv_mask != s->mask) {                        /* a new grid: its dots inside the glass */
        s_cv_mask = s->mask;
        for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) s_cv.mask[j * W + i] = s->mask[(j >> 2) * s->W + (i >> 1)];
    }
    return true;
}

/* 4 x 4 Bayer over the dot lattice: a dot shows where its ink beats the threshold, so a soft
 * edge or a shaded sphere dithers and a solid stroke is all dots */
static const uint8_t BAYER[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

/* Is item it inside spot s's disc (all of it: a ring, a line, ticks; else its centre)? */
static bool covered(const scene_t *sc, const scene_item_t *s, const scene_item_t *it) {
    const float sr = s->r * 1.001f, sd = sqrtf(s->x * s->x + s->y * s->y);
    switch (it->k) {
    case PK_SPOT:  return false;
    case PK_RING: case PK_ARC: return sd + it->r <= sr;
    case PK_TICKS: return sd + fmaxf(it->r, it->r + it->len) <= sr;
    case PK_LINE: case PK_CURVE:
        for (int q = 0; q < it->npts; q++) {
            const float *p = sc->pts[it->pts + q];
            if ((p[0] - s->x) * (p[0] - s->x) + (p[1] - s->y) * (p[1] - s->y) > sr * sr) return false;
        }
        return it->npts > 0;
    default: return (it->x - s->x) * (it->x - s->x) + (it->y - s->y) * (it->y - s->y) <= sr * sr;
    }
}

#define GROW_PTS 64                                    /* a grown line's points, at most */
static void paint_items(const ctx_t *c, const scene_t *sc, float t, float grow, float glow, fluid_letter_t *letters, int *nletters) {
    const scene_item_t *fs = sc && sc->focus >= 0 && sc->focus < sc->nitems && grow != 1.f ? &sc->items[sc->focus] : NULL;
    for (int n = 0; sc && n < sc->nitems; n++) {
        const scene_item_t *it = &sc->items[n];
        if (t < it->on || (it->off > 0.f && t >= it->off)) continue;
        scene_item_t g;
        float gp[GROW_PTS][2];
        const float (*pts)[2] = sc->pts + it->pts;
        bool lit = false;
        for (int q = 0; fs && it->grp && q < SCENE_LIGHTS; q++) lit |= fs->lights[q] == it->grp;
        if (it->when_lit && !(lit && glow > 0.f)) continue;   /* only while its group is lit */
        s_mark_on = lit || (fs && !fs->lights[0] && covered(sc, fs, it) && it->npts <= GROW_PTS);
        if (lit) {
            /* a lit group's item: grown about its own centre, brighter (or fading in) */
            const float k = grow, a = glow;
            g = *it;
            g.r *= k; g.size *= k; g.w *= k;
            g.rgb = it->when_lit ? shade(it->rgb, a) : toward_white(it->rgb, a * SCENE_LIT_WHITE);
            it = &g;
        } else if (fs && !fs->lights[0] && covered(sc, fs, it) && it->npts <= GROW_PTS) {
            /* the tapped spot's drawing, grown about the spot's centre */
            const float k = grow;
            g = *it;
            g.x = fs->x + (it->x - fs->x) * k; g.y = fs->y + (it->y - fs->y) * k;
            g.r *= k; g.len *= k; g.size *= k;
            for (int q = 0; q < it->npts; q++) { gp[q][0] = fs->x + (pts[q][0] - fs->x) * k; gp[q][1] = fs->y + (pts[q][1] - fs->y) * k; }
            it = &g; pts = (const float (*)[2])gp;
        }
        const pkg_asset_t *a = it->asset >= 0 ? &sc->pkg->assets[it->asset] : NULL;
        c->s->keep = it->k == PK_BALL || (it->k == PK_GLYPH && a && a->kind != PA_MASK);   /* no shape draws it */
        switch (it->k) {
        case PK_RING:  ring(c, it, false); break;
        case PK_ARC:   ring(c, it, true); break;
        case PK_TICKS:
            for (int q = 0; q < it->n; q++) {
                const float a0 = (it->a0 + q * 360.f / it->n) * DEG, cx = cosf(a0), cy = sinf(a0);
                segment(c, it->r * cx, it->r * cy, (it->r + it->len) * cx, (it->r + it->len) * cy, it->w * 0.5f, it->rgb);
            }
            break;
        case PK_LINE:  path(c, pts, it->npts, false, it->w * 0.5f, it->rgb); break;
        case PK_CURVE: path(c, pts, it->npts, true, it->w * 0.5f, it->rgb); break;
        case PK_DISC:  disc(c, it->x, it->y, it->r, it->rgb); break;
        case PK_BALL:  ball(c, it, a, t); break;
        case PK_GLYPH: if (a && !crisp(c, it, a, letters, nletters)) glyph(c, it, a); break;
        case PK_TEXT:  text(c, sc, it, letters, nletters); break;
        case PK_SPOT:  break;                                  /* a place to tap, not a drawing */
        default: break;
        }
    }
    s_mark_on = false;
}

void scene_grow_tick(scene_grow_t *g, bool reading, float dt) {
    if (g->spot < 0) return;
    g->t += dt;
    if (!reading && g->t >= SCENE_GROW_IN_S && (g->out -= dt / SCENE_GROW_OUT_S) <= 0.f) *g = (scene_grow_t){ -1, 0.f, 0.f };
}

void scene_grow_apply(const scene_grow_t *g, scene_t *sc) {
    if (!sc) return;
    const float u = fminf(1.f, g->t / SCENE_GROW_IN_S), in = 1.f - (1.f - u) * (1.f - u) * (1.f - u);
    sc->focus = g->spot;
    sc->glow = g->spot < 0 ? 0.f : in * fluid_smooth(0.f, 1.f, g->out);
    sc->grow = 1.f + (scene_spot_grow(sc, g->spot) - 1.f) * sc->glow;
}

void scene_hd_tick(scene_hd_t *h, unsigned gen, bool plain, float dt) {
    if (plain) { *h = (scene_hd_t){ 0, false, 1.f }; return; }
    if (h->gen && gen != h->gen) h->out = true;
    if (h->out && (h->dust += dt / SCENE_DUST_S) >= 1.f) *h = (scene_hd_t){ 0, false, 1.f };
    if (!h->out && gen) { h->gen = gen; h->dust = fmaxf(0.f, h->dust - dt / SCENE_DUST_S); }
}

/* a crisp letter or symbol's own shape on the dots under its box (as fluid_draw places it: a
 * symbol fills the box, a letter 0.8 x 0.72 of it, centred), for when it breaks into dust */
static void letter_dots(const fluid_sim_t *s, const fluid_letter_t *L, uint8_t *dots) {
    const uint8_t *gl = L->mask ? NULL : gc_font5x7(L->ch);
    if (L->mask ? !L->mw || !L->mh : !gl) return;
    const float px = (float)s->stage / (2 * s->W), py = (float)s->stage / (4 * s->H);   /* the dot pitch */
    const float gw = L->mask ? L->w : L->w * 0.8f, gh = L->mask ? L->h : L->h * 0.72f;
    const float gx = L->x + (L->w - gw) * 0.5f, gy = L->y + (L->h - gh) * 0.5f;
    const int x0 = (int)ceilf(L->x / px - 0.5f), x1 = (int)floorf((L->x + L->w) / px - 0.5f);
    const int y0 = (int)ceilf(L->y / py - 0.5f), y1 = (int)floorf((L->y + L->h) / py - 0.5f);
    for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < 4 * s->H; y++) for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < 2 * s->W; x++) {
        const float u = ((x + 0.5f) * px - gx) / gw, v = ((y + 0.5f) * py - gy) / gh;
        bool on = false;
        if (u >= 0.f && u < 1.f && v >= 0.f && v < 1.f) {
            if (L->mask) { const int p = (int)(v * L->mh) * L->mw + (int)(u * L->mw); on = L->mask[p >> 3] >> (7 - (p & 7)) & 1; }
            else on = gl[(int)(v * 7.f)] >> (4 - (int)(u * 5.f)) & 1;
        }
        const int k = (y >> 2) * s->W + (x >> 1), b = 1 << fluid_braille_bit(x & 1, y & 3);
        dots[k] = (uint8_t)(on ? dots[k] | b : dots[k] & ~b);
    }
}

/* Cells j0..j1 (all columns) from the canvas: each cell's target is its densest dot (a thin stroke
 * still draws the fluid to it), its colour the ink-weighted mean of its dots, its dots those over
 * Bayer (kept: those a ball or an image left); dots, keep may be NULL */
static void fold(const canvas_t *cv, int W, int j0, int j1, int16_t *T, uint32_t *tcol, uint8_t *dots, uint8_t *keep) {
    const uint32_t bg = fluid_pack_rgba8(0.6f, 0.68f, 0.92f, 0.f);
    for (int j = j0; j <= j1; j++) for (int i = 0; i < W; i++) {
        int tmax = 0, bits = 0, kept = 0;
        float wr = 0.f, wg = 0.f, wb = 0.f, w = 0.f;
        for (int q = 0; q < 8; q++) {
            const int col = q >> 2, row = q & 3, dx = 2 * i + col, dy = 4 * j + row, k = dy * cv->W + dx;
            const int v = cv->T[k];
            if (!v) continue;
            if (v > tmax) tmax = v;
            const float f = v * (1.f / FQ_UNIT);
            const uint32_t cc = cv->tcol[k];
            wr += f * (cc & 255); wg += f * (cc >> 8 & 255); wb += f * (cc >> 16 & 255); w += f;
            if (f > SCENE_INK * (BAYER[dy & 3][dx & 3] + 0.5f) / 16.f) {
                bits |= 1 << fluid_braille_bit(col, row);
                if (cv->mask[k] & 2) kept |= 1 << fluid_braille_bit(col, row);
            }
        }
        const int k = j * W + i;
        T[k] = (int16_t)tmax;
        tcol[k] = tmax ? fluid_pack_rgba8(wr / w / 255.f, wg / w / 255.f, wb / w / 255.f, 0.f) : bg;
        if (dots) dots[k] = (uint8_t)bits;
        if (keep) keep[k] = (uint8_t)kept;
    }
}

/* the items' fill, kept while nothing visible changes (PSRAM): the cells', and the items' letters
 * (their dots) */
typedef struct {
    uint32_t key;
    int N, nl;
    int16_t *T; uint32_t *tcol; uint8_t *dots, *keep;
    fluid_letter_t *letters;
    int16_t *bT; uint32_t *btcol; uint8_t *bmask; int brows;   /* the subtitle's band of the canvas, saved */
} fill_cache_t;
static fill_cache_t s_fc;

static bool fill_cache_for(int N, int brows, int cw) {
    if (s_fc.N != N || s_fc.brows < brows) {
        SCENE_FREE(s_fc.T); SCENE_FREE(s_fc.tcol); SCENE_FREE(s_fc.dots); SCENE_FREE(s_fc.letters);
        SCENE_FREE(s_fc.bT); SCENE_FREE(s_fc.btcol); SCENE_FREE(s_fc.bmask);
        const size_t nb = (size_t)brows * cw;
        s_fc = (fill_cache_t){ 0, N, 0, SCENE_CALLOC(sizeof(int16_t) * (size_t)N), SCENE_CALLOC(sizeof(uint32_t) * (size_t)N), SCENE_CALLOC(2 * (size_t)N), NULL,
                               SCENE_CALLOC(sizeof(fluid_letter_t) * FLUID_MAX_LETTERS),
                               SCENE_CALLOC(sizeof(int16_t) * nb), SCENE_CALLOC(sizeof(uint32_t) * nb), SCENE_CALLOC(nb), brows };
        s_fc.keep = s_fc.dots ? s_fc.dots + N : NULL;
        if (!s_fc.T || !s_fc.tcol || !s_fc.dots || !s_fc.letters || !s_fc.bT || !s_fc.btcol || !s_fc.bmask) { s_fc.N = 0; return false; }
    }
    return true;
}

/* what the items' fill depends on: the build, the grid, the items visible at t, the grown spot
 * and its growth (halfway, whole); 0 when it moves on its own (a turning ball) */
static uint32_t fill_key(const fluid_sim_t *s, const scene_t *sc, float t, float gq) {
    uint32_t h = (2166136261u ^ (sc ? sc->serial : 0)) * 16777619u;
    h = (h ^ (uint32_t)(uintptr_t)s ^ (uint32_t)(uintptr_t)s->mask) * 16777619u;
    h = (h ^ (uint32_t)s->stage ^ (uint32_t)(sc ? sc->focus : -1) << 16 ^ (uint32_t)(gq * 4.f) << 28) * 16777619u;
    for (int n = 0; sc && n < sc->nitems; n++) {
        const scene_item_t *it = &sc->items[n];
        const bool on = t >= it->on && !(it->off > 0.f && t >= it->off);
        if (on && it->k == PK_BALL && it->spin != 0.f) return 0;
        h = (h ^ (uint32_t)on) * 16777619u;
    }
    return h ? h : 1;
}

void scene_fill(fluid_sim_t *s, const scene_t *sc, float t, const scene_caption_t *cap, fluid_letter_t *letters, int *nletters, uint8_t *dots,
                scene_shapes_t *hd) {
    FLUID_PROF_BEGIN(c0);
    *nletters = 0;
    if (hd) hd->n = 0;
    /* the subtitle's band of canvas rows: its letters at their largest, and a dot */
    const int cvh = 4 * s->H, cvw = 2 * s->W;
    const float ch = (float)cvh / 2.f, lh = SCENE_CAPTION_SIZE * 1.05f * 0.5f, yc = 1.f - (sc ? sc->caption_y : SCENE_CAPTION_Y);
    int bj0 = ((int)floorf((yc - lh) * ch) - 1) / 4, bj1 = ((int)ceilf((yc + lh) * ch) + 1) / 4;
    bj0 = bj0 < 0 ? 0 : bj0; bj1 = bj1 >= s->H ? s->H - 1 : bj1;
    if (!canvas_for(s) || !fill_cache_for(s->N, 4 * (bj1 - bj0 + 1), cvw)) {
        const uint32_t bg = fluid_pack_rgba8(0.6f, 0.68f, 0.92f, 0.f);
        for (int k = 0; k < s->N; k++) { s->T[k] = 0; s->tcol[k] = bg; }
        if (dots) memset(dots, 0, (size_t)s->N);
        if (hd && hd->keep) memset(hd->keep, 0, (size_t)s->N);
        if (hd && hd->still) memset(hd->still, 0, (size_t)s->N);
        FLUID_PROF_END(FP_TARGET, c0);
        return;
    }
    canvas_t *cv = &s_cv;
    const float soft = 1.f / cv->H;                           /* soft edge: half a dot row */
    const float glow = sc ? sc->glow : 0.f, gq = glow >= 0.75f ? 1.f : glow >= 0.25f ? 0.5f : 0.f;
    const uint32_t key = fill_key(s, sc, t, gq);
    if (!key || key != s_fc.key) {
        /* the items on the canvas, their dots following a growth halfway or whole */
        const uint32_t bg = fluid_pack_rgba8(0.6f, 0.68f, 0.92f, 0.f);
        for (int k = 0; k < cv->N; k++) { cv->T[k] = 0; cv->tcol[k] = bg; cv->mask[k] &= 1; }
        const ctx_t c = { cv, soft, NULL, true };
        s_fc.nl = 0;
        paint_items(&c, sc, t, 1.f + (scene_spot_grow(sc, sc ? sc->focus : -1) - 1.f) * gq, gq, s_fc.letters, &s_fc.nl);
        fold(cv, s->W, 0, s->H - 1, s_fc.T, s_fc.tcol, s_fc.dots, s_fc.keep);
        for (int n = 0; n < s_fc.nl; n++) letter_dots(s, &s_fc.letters[n], s_fc.dots);
        s_fc.key = key;
    }
    memcpy(s->T, s_fc.T, sizeof(int16_t) * (size_t)s->N);
    memcpy(s->tcol, s_fc.tcol, sizeof(uint32_t) * (size_t)s->N);
    if (dots) memcpy(dots, s_fc.dots, (size_t)s->N);
    if (hd && hd->keep) memcpy(hd->keep, s_fc.keep, (size_t)s->N);
    /* the shapes and letters, as they are now; the cells a tapped spot's drawing covers at full growth (once a tap) */
    if (hd && hd->still) {
        static uint32_t still_key;
        const uint32_t sk = sc && sc->focus >= 0 && glow > 0.f ? (sc->serial * 2654435761u) ^ (uint32_t)(sc->focus + 1) ^ (uint32_t)(uintptr_t)s->mask : 0;
        if (sk != still_key || !sk) {
            memset(hd->still, 0, (size_t)s->N);
            if (sk) {
                static fluid_shape_t tmp[FLUID_MAX_SHAPES];
                static fluid_letter_t tl[FLUID_MAX_LETTERS];
                int tn = 0;
                scene_shapes_t th = { tmp, 0, NULL, NULL };
                const ctx_t cs = { cv, soft, &th, false };
                s_mark = hd->still;
                paint_items(&cs, sc, t, scene_spot_grow(sc, sc->focus), 1.f, tl, &tn);
                s_mark = NULL;
                for (int k = 0; k < s->N; k++) {               /* and one cell round */
                    if (hd->still[k] != 2) continue;
                    const int i = k % s->W, j = k / s->W;
                    for (int q = 0; q < 9; q++) {
                        const int ii = i + q % 3 - 1, jj = j + q / 3 - 1;
                        if (ii >= 0 && jj >= 0 && ii < s->W && jj < s->H && !hd->still[jj * s->W + ii]) hd->still[jj * s->W + ii] = 1;
                    }
                }
            }
            still_key = sk;
        }
    }
    const ctx_t c = { cv, soft, hd, false };
    paint_items(&c, sc, t, sc ? sc->grow : 1.f, glow, letters, nletters);
    if (cap && cap->n > 0) {
        /* the subtitle, over the drawing: painted on the canvas's band (then put back), its cells
         * folded again */
        const int r0 = 4 * bj0, nb = 4 * (bj1 - bj0 + 1) * cv->W, o = r0 * cv->W;
        memcpy(s_fc.bT, cv->T + o, sizeof(int16_t) * (size_t)nb);
        memcpy(s_fc.btcol, cv->tcol + o, sizeof(uint32_t) * (size_t)nb);
        memcpy(s_fc.bmask, cv->mask + o, (size_t)nb);
        const int n0 = *nletters;
        const ctx_t cc = { cv, soft, hd, true };
        caption(&cc, cap, sc ? sc->caption_y : SCENE_CAPTION_Y, letters, nletters);
        fold(cv, s->W, bj0, bj1, s->T, s->tcol, dots, hd ? hd->keep : NULL);
        for (int n = 0; dots && n < s_fc.nl; n++) letter_dots(s, &s_fc.letters[n], dots);
        for (int n = n0; dots && n < *nletters; n++) letter_dots(s, &letters[n], dots);
        memcpy(cv->T + o, s_fc.bT, sizeof(int16_t) * (size_t)nb);
        memcpy(cv->tcol + o, s_fc.btcol, sizeof(uint32_t) * (size_t)nb);
        memcpy(cv->mask + o, s_fc.bmask, (size_t)nb);
    }
    FLUID_PROF_END(FP_TARGET, c0);
}

int scene_full_shapes(fluid_sim_t *s, const scene_t *sc, float t, fluid_shape_t *out, uint32_t *key) {
    *key = 0;
    if (!sc || !canvas_for(s)) return -1;
    static fluid_letter_t tl[FLUID_MAX_LETTERS];
    int tn = 0;
    scene_shapes_t h = { out, 0, NULL, NULL };
    const ctx_t c = { &s_cv, 1.f / s_cv.H, &h, false };
    const bool tapped = sc->focus >= 0 && sc->glow > 0.f;
    paint_items(&c, sc, t, tapped ? scene_spot_grow(sc, sc->focus) : 1.f, tapped ? 1.f : 0.f, tl, &tn);
    /* what they depend on: the build, the grid, the tapped spot, the items visible */
    uint32_t k = (2166136261u ^ sc->serial) * 16777619u;
    k = (k ^ (uint32_t)s->stage ^ (uint32_t)(tapped ? sc->focus + 1 : 0) << 20) * 16777619u;
    for (int n = 0; n < sc->nitems; n++) {
        const scene_item_t *it = &sc->items[n];
        k = (k ^ (uint32_t)(t >= it->on && !(it->off > 0.f && t >= it->off))) * 16777619u;
    }
    *key = k ? k : 1;
    return h.n;
}

int scene_outline(const scene_t *sc, int n, void (*fn)(void *ctx, float x, float y), void *ctx) {
    if (!sc || n < 0 || n >= sc->nitems || sc->items[n].k != PK_SPOT) return 0;
    const scene_item_t *fs = &sc->items[n];
    const float k = scene_spot_grow(sc, n), gap = SCENE_OUTLINE_GAP, step = SCENE_OUTLINE_STEP;
    int cnt = 0;
#define ROUND(cx, cy, rr) do { const float r_ = (rr); const int m_ = r_ * 6.2831853f / step > 6.f ? (int)ceilf(r_ * 6.2831853f / step) : 6; \
        for (int q_ = 0; q_ < m_; q_++, cnt++) fn(ctx, (cx) + r_ * cosf(6.2831853f * q_ / m_), (cy) + r_ * sinf(6.2831853f * q_ / m_)); } while (0)
    for (int i = 0; i < sc->nitems; i++) {
        const scene_item_t *it = &sc->items[i];
        if (it->k == PK_SPOT) continue;
        bool lit = false;
        for (int q = 0; it->grp && q < SCENE_LIGHTS; q++) lit |= fs->lights[q] == it->grp;
        const bool inside = !fs->lights[0] && covered(sc, fs, it);
        if (!lit && !inside) continue;
        /* the item grown: a lit one about its own centre, one inside the spot about the spot's centre */
        const float x = lit ? it->x : fs->x + (it->x - fs->x) * k, y = lit ? it->y : fs->y + (it->y - fs->y) * k;
        switch (it->k) {
        case PK_LINE: case PK_CURVE:
            for (int q = 0; q + 1 < it->npts; q++) {
                float a[2], b[2];
                for (int e = 0; e < 2; e++) {
                    const float *p = sc->pts[it->pts + q + e];
                    float *o = e ? b : a;
                    o[0] = lit ? p[0] : fs->x + (p[0] - fs->x) * k; o[1] = lit ? p[1] : fs->y + (p[1] - fs->y) * k;
                }
                const float l = hypotf(b[0] - a[0], b[1] - a[1]);
                if (l < 1e-6f) continue;
                const float nx = -(b[1] - a[1]) / l * gap, ny = (b[0] - a[0]) / l * gap;
                const int m = (int)ceilf(l / step) > 1 ? (int)ceilf(l / step) : 1;
                for (int sg = -1; sg <= 1; sg += 2)
                    for (int e = 0; e <= m; e++, cnt++) fn(ctx, a[0] + sg * nx + (b[0] - a[0]) * e / m, a[1] + sg * ny + (b[1] - a[1]) * e / m);
            }
            break;
        case PK_RING: case PK_ARC:
            ROUND(x, y, it->r * k + gap);
            if (it->r * k > 2.f * gap) ROUND(x, y, it->r * k - gap);
            break;
        case PK_DISC: if (it->rgb) ROUND(x, y, it->r * k + gap); break;
        case PK_GLYPH: case PK_TEXT: ROUND(x, y, it->size * k * 0.55f + gap); break;
        default: break;
        }
    }
#undef ROUND
    return cnt;
}
