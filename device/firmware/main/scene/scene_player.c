/*
 * SPDX-License-Identifier: MIT
 *
 * scene_player.c — a package's layout on screen (scene_player.h).
 * Plain C: built into the firmware and by test/host/scene_test.c.
 */
#include "scene_player.h"

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

static bool scalar_ok(const cJSON *v) {
    return (cJSON_IsNumber(v) && isfinite(v->valuedouble)) || (cJSON_IsString(v) && pkg_text_ok(v->valuestring, PKG_STR_MAX));
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
        case PP_TEXT:   if (!cJSON_IsString(v) || !pkg_text_ok(v->valuestring, PKG_STR_MAX)) return false; break;
        default:
            if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) > SCENE_LIST_MAX) return false;
            for (const cJSON *e = v->child; e; e = e->next) {
                int n = 0;
                if (!cJSON_IsObject(e)) return false;
                for (const cJSON *f = e->child; f; f = f->next, n++) if (n >= 8 || !name_ok(f->string) || !scalar_ok(f)) return false;
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
              num(b, &f[PF_ROT], el, PF_ROT, 0.f, &o->rot);
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
    if (it->k == PK_TEXT) {
        const char *s = f[PF_STR].src == PV_STR ? f[PF_STR].s : bound_text(b, &f[PF_STR], el);
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
    snprintf(sc->pkg_id, sizeof(sc->pkg_id), "%s", p->id);
    snprintf(sc->layout, sizeof(sc->layout), "%s", l->name);
    sc->dur = l->dur;
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

/* ── the player ────────────────────────────────────────────────────────────── */

static void start(scene_player_t *p, scene_t *sc) { p->cur = sc; p->t = 0.f; p->started = sc != NULL; }

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

typedef struct { fluid_sim_t *s; float soft; } ctx_t;

static inline void paint(fluid_sim_t *s, int k, float v, uint32_t rgb) {
    if (v <= 0.004f || !s->mask[k] || v * FQ_UNIT <= s->T[k]) return;
    s->T[k] = fq(v, FQ_UNIT);
    s->tcol[k] = fluid_pack_rgba8((rgb >> 16 & 255) / 255.f, (rgb >> 8 & 255) / 255.f, (rgb & 255) / 255.f, 0.f);
}

/* the cells whose centres lie in a glass-space box (y up) */
static void cells(const fluid_sim_t *s, float x0, float y0, float x1, float y1, int *i0, int *i1, int *j0, int *j1) {
    *i0 = (int)floorf((x0 + 1.f) * 0.5f * s->W - 0.5f); *i1 = (int)ceilf((x1 + 1.f) * 0.5f * s->W - 0.5f);
    *j0 = (int)floorf((1.f - y1) * 0.5f * s->H - 0.5f); *j1 = (int)ceilf((1.f - y0) * 0.5f * s->H - 0.5f);
    if (*i0 < 0) *i0 = 0;
    if (*j0 < 0) *j0 = 0;
    if (*i1 > s->W - 1) *i1 = s->W - 1;
    if (*j1 > s->H - 1) *j1 = s->H - 1;
}

static void segment(const ctx_t *c, float ax, float ay, float bx, float by, float hw, uint32_t rgb) {
    fluid_sim_t *s = c->s;
    const float reach = hw + c->soft, dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy, il2 = l2 > 1e-9f ? 1.f / l2 : 0.f;
    int i0, i1, j0, j1;
    cells(s, fminf(ax, bx) - reach, fminf(ay, by) - reach, fmaxf(ax, bx) + reach, fmaxf(ay, by) + reach, &i0, &i1, &j0, &j1);
    for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
        const float px = s->xs[i] - ax, py = -s->ys[j] - ay;
        float u = (px * dx + py * dy) * il2;
        u = u < 0.f ? 0.f : u > 1.f ? 1.f : u;
        const float ex = px - u * dx, ey = py - u * dy, d = sqrtf(ex * ex + ey * ey);
        if (d < reach) paint(s, j * s->W + i, SCENE_INK * (1.f - fluid_smooth(hw, reach, d)), rgb);
    }
}

/* ring / arc: |r − distance| within w/2, at angles a0..a1 (a1 − a0 ≥ 360: all of it) */
static void ring(const ctx_t *c, const scene_item_t *it, bool arc) {
    fluid_sim_t *s = c->s;
    const float hw = it->w * 0.5f, reach = hw + c->soft, ro = it->r + reach;
    float span = it->a1 - it->a0;
    const bool whole = !arc || span >= 360.f;
    span = fmodf(fmodf(span, 360.f) + 360.f, 360.f);
    int i0, i1, j0, j1;
    cells(s, -ro, -ro, ro, ro, &i0, &i1, &j0, &j1);
    for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
        const float x = s->xs[i], y = -s->ys[j], d = fabsf(sqrtf(x * x + y * y) - it->r);
        if (d >= reach) continue;
        if (!whole) {
            float a = fmodf(fluid_atan2(y, x) / DEG - it->a0, 360.f);
            if (a < 0.f) a += 360.f;
            if (a > span) continue;
        }
        paint(s, j * s->W + i, SCENE_INK * (1.f - fluid_smooth(hw, reach, d)), it->rgb);
    }
}

static void disc(const ctx_t *c, float cx, float cy, float r, uint32_t rgb) {
    fluid_sim_t *s = c->s;
    const float ro = r + c->soft * 0.5f;
    int i0, i1, j0, j1;
    cells(s, cx - ro, cy - ro, cx + ro, cy + ro, &i0, &i1, &j0, &j1);
    for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
        const float x = s->xs[i] - cx, y = -s->ys[j] - cy, d = sqrtf(x * x + y * y);
        if (d < ro) paint(s, j * s->W + i, SCENE_INK * (1.f - fluid_smooth(r - c->soft * 0.5f, ro, d)), rgb);
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

static inline uint32_t shade(uint32_t rgb, float k) {
    return (uint32_t)((rgb >> 16 & 255) * k) << 16 | (uint32_t)((rgb >> 8 & 255) * k) << 8 | (uint32_t)((rgb & 255) * k);
}

/* a sphere lit from one side (lit = the lit fraction of the face, side = which side), wearing
 * an image as an equirectangular map that turns `spin` degrees per second */
static void ball(const ctx_t *c, const scene_item_t *it, const pkg_asset_t *img, float t) {
    fluid_sim_t *s = c->s;
    if (it->r <= 0.f) return;
    const float r = it->r, ro = r + c->soft * 0.5f, cphi = 2.f * it->lit - 1.f, sphi = sqrtf(fmaxf(0.f, 1.f - cphi * cphi));
    const float lx = it->side * sphi, lz = cphi, turn = it->spin * t * DEG;
    int i0, i1, j0, j1;
    cells(s, it->x - ro, it->y - ro, it->x + ro, it->y + ro, &i0, &i1, &j0, &j1);
    for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
        const float x = s->xs[i] - it->x, y = -s->ys[j] - it->y, d = sqrtf(x * x + y * y);
        if (d >= ro) continue;
        const float edge = 1.f - fluid_smooth(r - c->soft * 0.5f, ro, d);
        float nx = x / r, ny = y / r, rr = nx * nx + ny * ny;
        if (rr > 0.999f) { const float k = 0.9995f / sqrtf(rr); nx *= k; ny *= k; rr = nx * nx + ny * ny; }
        const float nz = sqrtf(1.f - rr), lum = 0.18f + 0.82f * fluid_smooth(-0.08f, 0.12f, nx * lx + nz * lz);
        uint32_t rgb = it->rgb;
        if (img) {
            float u = (atan2f(nx, nz) + turn) * (0.5f / 3.1415927f) + 0.5f;
            u -= floorf(u);
            const int ix = (int)(u * img->w) % img->w, iy = (int)((0.5f - asinf(ny) / 3.1415927f) * img->h);
            const int q = px4(img, ix, iy < 0 ? 0 : iy >= img->h ? img->h - 1 : iy);
            if (q) rgb = img->pal[q];
        }
        paint(s, j * s->W + i, SCENE_INK * edge * (0.3f + 0.7f * lum), shade(rgb, lum));
    }
}

/* an asset `size` wide, turned rot degrees: each cell's coverage from 3 × 3 samples */
static void glyph(const ctx_t *c, const scene_item_t *it, const pkg_asset_t *a) {
    fluid_sim_t *s = c->s;
    const float w = it->size, h = it->size * a->h / a->w, ext = 0.5f * sqrtf(w * w + h * h);
    const float co = cosf(it->rot * DEG), si = sinf(it->rot * DEG), cw = 2.f / s->W, ch = 2.f / s->H;
    int i0, i1, j0, j1;
    cells(s, it->x - ext, it->y - ext, it->x + ext, it->y + ext, &i0, &i1, &j0, &j1);
    for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
        int cov = 0; float cr = 0.f, cg = 0.f, cb = 0.f;
        for (int q = 0; q < 9; q++) {
            const float dx = s->xs[i] + cw * ((q % 3) - 1) / 3.f - it->x, dy = -s->ys[j] + ch * ((q / 3) - 1) / 3.f - it->y;
            const float u = (co * dx + si * dy) / w + 0.5f, v = 0.5f - (-si * dx + co * dy) / h;
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
        paint(s, j * s->W + i, SCENE_INK * cov / 9.f, rgb);
    }
}

/* the reader's letter boxes (fluid_text_set_labels): ink under each letter, and the letters
 * themselves for the crisp overlay; size = the type size in glass units */
static void text(const ctx_t *c, const scene_t *sc, const scene_item_t *it, fluid_letter_t *letters, int *nl) {
    fluid_sim_t *s = c->s;
    const float fs = it->size * s->stage * 0.5f, adv = fs * 0.6f, lh = fs * 1.05f;
    const float x0 = (it->x + 1.f) * 0.5f * s->stage - it->nstr * adv * 0.5f, y0 = (1.f - it->y) * 0.5f * s->stage - lh * 0.5f;
    const char *str = sc->chars + it->str;
    for (int q = 0; q < it->nstr && *nl < SCENE_LETTERS; q++)
        letters[(*nl)++] = (fluid_letter_t){ str[q], false, x0 + q * adv, y0, adv, lh, fs, 0.f, FLUID_RGB | it->rgb };
    const float cwp = (float)s->stage / s->W, chp = (float)s->stage / s->H;
    int i0 = (int)floorf(x0 / cwp), i1 = (int)ceilf((x0 + it->nstr * adv) / cwp), j0 = (int)floorf(y0 / chp), j1 = (int)ceilf((y0 + lh) / chp);
    i0 = i0 < 0 ? 0 : i0; j0 = j0 < 0 ? 0 : j0; i1 = i1 >= s->W ? s->W - 1 : i1; j1 = j1 >= s->H ? s->H - 1 : j1;
    for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
        const float px = (i + 0.5f) * cwp, py = (j + 0.5f) * chp;
        if (py <= y0 || py >= y0 + lh) continue;
        const int li = (int)floorf((px - x0) / adv);
        if (li >= 0 && li < it->nstr && str[li] != ' ') paint(s, j * s->W + i, 0.62f, it->rgb);
    }
}

/* Catmull-Rom through the points, 8 segments a span */
static void curve(const ctx_t *c, const float (*p)[2], int n, float hw, uint32_t rgb) {
    float px = p[0][0], py = p[0][1];
    for (int i = 0; i + 1 < n; i++) {
        const float *a = p[i > 0 ? i - 1 : 0], *b = p[i], *d = p[i + 1], *e = p[i + 2 < n ? i + 2 : n - 1];
        for (int q = 1; q <= 8; q++) {
            const float u = q / 8.f, u2 = u * u, u3 = u2 * u;
            float xy[2];
            for (int k = 0; k < 2; k++)
                xy[k] = 0.5f * (2.f * b[k] + (d[k] - a[k]) * u + (2.f * a[k] - 5.f * b[k] + 4.f * d[k] - e[k]) * u2 +
                                (3.f * b[k] - a[k] - 3.f * d[k] + e[k]) * u3);
            segment(c, px, py, xy[0], xy[1], hw, rgb);
            px = xy[0]; py = xy[1];
        }
    }
}

void scene_fill(fluid_sim_t *s, const scene_t *sc, float t, fluid_letter_t *letters, int *nletters) {
    FLUID_PROF_BEGIN(c0);
    const uint32_t bg = fluid_pack_rgba8(0.6f, 0.68f, 0.92f, 0.f);
    for (int k = 0; k < s->N; k++) { s->T[k] = 0; s->tcol[k] = bg; }
    *nletters = 0;
    const ctx_t c = { s, 1.5f / s->H };                       /* soft edge: ¾ of a cell row */
    for (int n = 0; sc && n < sc->nitems; n++) {
        const scene_item_t *it = &sc->items[n];
        if (t < it->on || (it->off > 0.f && t >= it->off)) continue;
        const pkg_asset_t *a = it->asset >= 0 ? &sc->pkg->assets[it->asset] : NULL;
        switch (it->k) {
        case PK_RING:  ring(&c, it, false); break;
        case PK_ARC:   ring(&c, it, true); break;
        case PK_TICKS:
            for (int q = 0; q < it->n; q++) {
                const float a0 = (it->a0 + q * 360.f / it->n) * DEG, cx = cosf(a0), cy = sinf(a0);
                segment(&c, it->r * cx, it->r * cy, (it->r + it->len) * cx, (it->r + it->len) * cy, it->w * 0.5f, it->rgb);
            }
            break;
        case PK_LINE:
            for (int q = 0; q + 1 < it->npts; q++)
                segment(&c, sc->pts[it->pts + q][0], sc->pts[it->pts + q][1], sc->pts[it->pts + q + 1][0], sc->pts[it->pts + q + 1][1], it->w * 0.5f, it->rgb);
            break;
        case PK_CURVE: curve(&c, sc->pts + it->pts, it->npts, it->w * 0.5f, it->rgb); break;
        case PK_DISC:  disc(&c, it->x, it->y, it->r, it->rgb); break;
        case PK_BALL:  ball(&c, it, a, t); break;
        case PK_GLYPH: if (a) glyph(&c, it, a); break;
        case PK_TEXT:  text(&c, sc, it, letters, nletters); break;
        default: break;
        }
    }
    FLUID_PROF_END(FP_TARGET, c0);
}
