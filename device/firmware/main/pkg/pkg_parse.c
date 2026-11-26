/*
 * SPDX-License-Identifier: MIT
 *
 * pkg_parse.c: a package file into bounded structs, strictly (pkg_parse.h).
 * Plain C: built into the firmware and by test/host/pkg_test.c.
 */
#include "pkg_parse.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define PKG_MALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define PKG_FREE(p)   heap_caps_free(p)
#else
#define PKG_MALLOC(n) malloc(n)
#define PKG_FREE(p)   free(p)
#endif

const char *const PKG_KIND_NAMES[PK_KINDS] = { "ring", "arc", "ticks", "line", "curve", "disc", "ball", "glyph", "text", "spot", "beat", "dial", "centre" };

/* ── arena: 16 KB blocks, linked; the package struct sits at the head of the first ── */

#define BLOCK 16384
typedef struct blk { struct blk *next; size_t used, cap, pad; } blk_t;   /* pad: the data after it stays 8-aligned */
typedef struct { blk_t *head; size_t total; } arena_t;

static void *aalloc(arena_t *a, size_t n) {
    n = (n + 7) & ~(size_t)7;
    blk_t *b = a->head;
    if (!b || b->cap - b->used < n) {
        size_t cap = n > BLOCK ? n : BLOCK;
        blk_t *nb = PKG_MALLOC(sizeof(blk_t) + cap);
        if (!nb) return NULL;
        nb->used = 0; nb->cap = cap;
        if (b) { nb->next = b->next; b->next = nb; }   /* the head keeps the package struct */
        else { nb->next = NULL; a->head = nb; }
        b = nb;
        a->total += cap;
    }
    void *p = (uint8_t *)(b + 1) + b->used;
    b->used += n;
    memset(p, 0, n);
    return p;
}

static void afree(blk_t *b) { while (b) { blk_t *n = b->next; PKG_FREE(b); b = n; } }

static const char *astr(arena_t *a, const char *s) {
    size_t n = strlen(s) + 1;
    char *d = aalloc(a, n);
    if (d) memcpy(d, s, n);
    return d;
}

/* ── small checks ──────────────────────────────────────────────────────────── */

bool pkg_id_ok(const char *id) {
    size_t n = id ? strlen(id) : 0;
    if (n < 1 || n > PKG_ID_MAX) return false;
    for (size_t i = 0; i < n; i++)
        if (!((id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '0' && id[i] <= '9') || id[i] == '-')) return false;
    return true;
}

bool pkg_text_ok(const char *s, size_t max) {
    size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > max) return false;
    for (size_t i = 0; i < n; i++) if (s[i] < 32 || s[i] > 126) return false;
    return true;
}

static bool key_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > PKG_KEY_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

static bool version_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > PKG_VERSION_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    }
    return true;
}

static int hexv(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

bool pkg_color(const char *s, uint32_t *rgb) {
    if (!s || s[0] != '#' || strlen(s) != 7) return false;
    uint32_t v = 0;
    for (int i = 1; i < 7; i++) { int h = hexv(s[i]); if (h < 0) return false; v = v << 4 | (uint32_t)h; }
    *rgb = v;
    return true;
}

bool pkg_num_ok(int pf, float v) {
    if (!isfinite(v)) return false;
    switch (pf) {
    case PF_X: case PF_Y:     return v >= -2.f && v <= 2.f;
    case PF_R: case PF_PR:    return v >= 0.f && v <= 2.f;
    case PF_W:                return v >= 0.f && v <= 0.5f;
    case PF_LEN:              return v >= -1.f && v <= 1.f;
    case PF_N:                return v >= 1.f && v <= 120.f && v == floorf(v);
    case PF_SIZE:             return v > 0.f && v <= 2.f;
    case PF_LIT:              return v >= 0.f && v <= 1.f;
    case PF_SIDE:             return v == -1.f || v == 1.f;
    case PF_SPIN:             return v >= -720.f && v <= 720.f;
    case PF_ON: case PF_OFF:  return v >= 0.f && v <= PKG_DUR_MAX;
    case PF_GROW:             return v >= 1.f && v <= 3.f;
    case PF_ARC:              return v > 0.f && v <= 360.f;
    case PF_R0: case PF_R1:   return v >= 0.f && v <= 2.f;
    case PF_TTL:              return v >= 1.f && v <= PKG_DUR_MAX;
    case PF_STEP:             return v > 0.f && v <= 360.f;
    case PF_BY: case PF_FAST: return v >= -1000.f && v <= 1000.f;
    case PF_MIN: case PF_MAX: case PF_VAL: return v >= -10000.f && v <= 10000.f;
    case PF_FACE:             return v == 0.f || v == 1.f;
    case PF_TICK_MS:          return v == 0.f || (v >= 5.f && v <= 60.f);    /* 0: the motor's own (haptic_plan.h) */
    case PF_TICK:             return v == 0.f || (v >= 25.f && v <= 100.f);  /* percent; under 25 it does not turn */
    default:                  return v >= -720.f && v <= 720.f;   /* angles: a0 a1 rot polar */
    }
}

/* base64 (standard alphabet, '=' padding optional) into exactly n bytes */
static bool b64(const char *s, uint8_t *out, size_t n) {
    size_t o = 0; uint32_t acc = 0; int bits = 0;
    for (; *s && *s != '='; s++) {
        char c = *s; int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else return false;
        acc = acc << 6 | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; if (o >= n) return false; out[o++] = (uint8_t)(acc >> bits); }
    }
    while (*s == '=') s++;
    return !*s && o == n;
}

/* every key of object o is in keys[] (NULL-terminated) */
static bool only(const cJSON *o, const char *const *keys) {
    for (const cJSON *c = o->child; c; c = c->next) {
        bool known = false;
        for (const char *const *k = keys; *k && !known; k++) known = !strcmp(c->string, *k);
        if (!known) return false;
    }
    return true;
}

static int count(const cJSON *o) { int n = 0; for (const cJSON *c = o ? o->child : NULL; c; c = c->next) n++; return n; }

/* ── assets ────────────────────────────────────────────────────────────────── */

static const char *parse_asset(arena_t *a, const cJSON *j, pkg_asset_t *as) {
    static const char *const MASK[] = { "kind", "w", "h", "bits", NULL };
    static const char *const IMAGE[] = { "kind", "w", "h", "px", "palette", NULL };
    if (!cJSON_IsObject(j)) return "asset: not an object";
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(j, "kind");
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(j, "w"), *h = cJSON_GetObjectItemCaseSensitive(j, "h");
    if (!cJSON_IsString(kind)) return "asset: kind";
    if (!strcmp(kind->valuestring, "mask")) as->kind = PA_MASK;
    else if (!strcmp(kind->valuestring, "image")) as->kind = PA_IMAGE;
    else return "asset: unknown kind";
    if (!only(j, as->kind == PA_MASK ? MASK : IMAGE)) return "asset: unknown key";
    if (!cJSON_IsNumber(w) || !cJSON_IsNumber(h) || w->valuedouble != w->valueint || h->valuedouble != h->valueint ||
        w->valueint < 1 || h->valueint < 1 || w->valueint > PKG_ASSET_SIDE || h->valueint > PKG_ASSET_SIDE) return "asset: w / h";
    as->w = (uint16_t)w->valueint; as->h = (uint16_t)h->valueint;
    const size_t px = (size_t)as->w * as->h, n = as->kind == PA_MASK ? (px + 7) / 8 : (px + 1) / 2;
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(j, as->kind == PA_MASK ? "bits" : "px");
    uint8_t *d = aalloc(a, n);
    if (!d) return "oom";
    if (!cJSON_IsString(data) || !b64(data->valuestring, d, n)) return "asset: data length";
    as->data = d;
    if (as->kind == PA_IMAGE) {
        const cJSON *pal = cJSON_GetObjectItemCaseSensitive(j, "palette");
        int np = cJSON_IsArray(pal) ? cJSON_GetArraySize(pal) : 0;
        if (np < 1 || np > PKG_PALETTE_MAX) return "asset: palette";
        int i = 0;
        for (const cJSON *c = pal->child; c; c = c->next, i++)
            if (!cJSON_IsString(c) || !pkg_color(c->valuestring, &as->pal[i])) return "asset: palette colour";
        as->npal = (uint8_t)np;
        for (size_t p = 0; p < px; p++) if (((d[p / 2] >> (p & 1 ? 0 : 4)) & 15) >= np) return "asset: index past the palette";
    }
    return NULL;
}

/* ── items ─────────────────────────────────────────────────────────────────── */

enum { T_NUM, T_COLOR, T_ASSET, T_IMAGE, T_STR, T_SAY, T_LIST };
#define KB(k) (1ull << (k))   /* a kind's bit, or a field's (PF_COUNT > 32) */
#define XY (KB(PK_RING) | KB(PK_ARC) | KB(PK_DISC) | KB(PK_BALL) | KB(PK_GLYPH) | KB(PK_TEXT) | KB(PK_SPOT))
#define ALL ((1u << PK_KINDS) - 1u)
#define WHEEL (KB(PK_BEAT) | KB(PK_DIAL) | KB(PK_CENTRE))   /* the device's own pieces: in no group */
static const struct { const char *key; uint8_t pf, type; uint16_t kinds; } FIELDS[] = {
    { "x", PF_X, T_NUM, XY }, { "y", PF_Y, T_NUM, XY },
    { "r", PF_R, T_NUM, KB(PK_RING) | KB(PK_ARC) | KB(PK_TICKS) | KB(PK_DISC) | KB(PK_BALL) | KB(PK_SPOT) | KB(PK_BEAT) | KB(PK_DIAL) | KB(PK_CENTRE) },
    { "w", PF_W, T_NUM, KB(PK_RING) | KB(PK_ARC) | KB(PK_TICKS) | KB(PK_LINE) | KB(PK_CURVE) },
    { "a0", PF_A0, T_NUM, KB(PK_ARC) | KB(PK_TICKS) }, { "a1", PF_A1, T_NUM, KB(PK_ARC) },
    { "len", PF_LEN, T_NUM, KB(PK_TICKS) }, { "n", PF_N, T_NUM, KB(PK_TICKS) },
    { "size", PF_SIZE, T_NUM, KB(PK_GLYPH) | KB(PK_TEXT) | KB(PK_BEAT) },   /* beat: its hits' dots */
    { "lit", PF_LIT, T_NUM, KB(PK_BALL) }, { "side", PF_SIDE, T_NUM, KB(PK_BALL) }, { "spin", PF_SPIN, T_NUM, KB(PK_BALL) },
    { "rot", PF_ROT, T_NUM, KB(PK_GLYPH) },
    { "on", PF_ON, T_NUM, ALL }, { "off", PF_OFF, T_NUM, ALL },
    { "color", PF_COLOR, T_COLOR, ALL },
    { "img", PF_ASSET, T_IMAGE, KB(PK_BALL) }, { "asset", PF_ASSET, T_ASSET, KB(PK_GLYPH) },
    { "str", PF_STR, T_STR, KB(PK_TEXT) },
    { "say", PF_SAY, T_SAY, KB(PK_SPOT) }, { "id", PF_ID, T_STR, KB(PK_SPOT) | KB(PK_DIAL) },
    /* a highlight: a spot lights groups (names, spaces between), their items grow and brighten,
     * an item `when: "lit"` shows only then */
    { "group", PF_GROUP, T_STR, ALL & ~KB(PK_SPOT) & ~WHEEL }, { "when", PF_WHEN, T_STR, ALL & ~KB(PK_SPOT) & ~WHEEL },
    { "lights", PF_LIGHTS, T_STR, KB(PK_SPOT) },
    { "grow", PF_GROW, T_NUM, KB(PK_SPOT) },          /* what a tap on it lights grows this much */
    /* a spot is also tappable in a ring sector about the glass centre: arc degrees wide, centred on the spot's own
     * direction, from radius r0 (0 unless given) to r1 */
    { "arc", PF_ARC, T_NUM, KB(PK_SPOT) }, { "r0", PF_R0, T_NUM, KB(PK_SPOT) | KB(PK_DIAL) }, { "r1", PF_R1, T_NUM, KB(PK_SPOT) },
    /* a tap on it opens another layout of the package over this one (scene_player.h scene_open_build), for ttl
     * seconds when that layout is a held one (dur 0) */
    { "open", PF_OPEN, T_STR, KB(PK_SPOT) }, { "ttl", PF_TTL, T_NUM, KB(PK_SPOT) },
    /* a dial: grabbed from r0 out, drawn at r; a click every `step` degrees adds `by` (`fast` turning fast) to its value
     * (min..max, from `value`), or, with `labels` (a list param, each element's `label`), steps through them; `sets` the
     * beat player's `tempo` or its `pick`; `face` 0 (while the beat plays) or 1 (while it is stopped); each click ticks
     * the motor `tick_ms` at `tick` percent (0: the motor's own wheel tick) */
    { "step", PF_STEP, T_NUM, KB(PK_DIAL) }, { "by", PF_BY, T_NUM, KB(PK_DIAL) }, { "fast", PF_FAST, T_NUM, KB(PK_DIAL) },
    { "min", PF_MIN, T_NUM, KB(PK_DIAL) }, { "max", PF_MAX, T_NUM, KB(PK_DIAL) }, { "value", PF_VAL, T_NUM, KB(PK_DIAL) },
    { "face", PF_FACE, T_NUM, KB(PK_DIAL) }, { "sets", PF_SETS, T_STR, KB(PK_DIAL) }, { "labels", PF_LABELS, T_LIST, KB(PK_DIAL) },
    { "tick_ms", PF_TICK_MS, T_NUM, KB(PK_DIAL) }, { "tick", PF_TICK, T_NUM, KB(PK_DIAL) },
    /* a centre: a tap within r plays and stops the beat (the layout has one) */
};
#define NFIELDS ((int)(sizeof(FIELDS) / sizeof(FIELDS[0])))
/* what each kind cannot do without */
static const uint64_t REQUIRED[PK_KINDS] = {
    [PK_RING] = KB(PF_R), [PK_ARC] = KB(PF_R) | KB(PF_A0) | KB(PF_A1), [PK_TICKS] = KB(PF_R) | KB(PF_N),
    [PK_DISC] = KB(PF_R), [PK_BALL] = KB(PF_R), [PK_GLYPH] = KB(PF_ASSET) | KB(PF_SIZE), [PK_TEXT] = KB(PF_STR),
    [PK_SPOT] = KB(PF_R) | KB(PF_SAY), [PK_BEAT] = KB(PF_R), [PK_DIAL] = KB(PF_ID), [PK_CENTRE] = KB(PF_R),
};

typedef struct { const pkg_t *p; const pkg_layout_t *l; bool in_each; arena_t *a; } ictx_t;

static int asset_index(const pkg_t *p, const char *name) {
    for (int i = 0; i < p->nassets; i++) if (!strcmp(p->assets[i].name, name)) return i;
    return -1;
}

static int param_index(const pkg_layout_t *l, const char *name) {
    for (int i = 0; i < l->nparams; i++) if (!strcmp(l->params[i].name, name)) return i;
    return -1;
}

/* a field value: a literal of its type, "$param" (declared, of the matching type), or "@field" (inside each) */
static const char *parse_val(const ictx_t *c, const cJSON *j, int pf, int type, pkg_val_t *v) {
    if (cJSON_IsString(j) && j->valuestring[0] == '$') {
        int i = param_index(c->l, j->valuestring + 1);
        if (i < 0) return "binding: undeclared param";
        if (c->l->params[i].type != (type == T_NUM ? PP_NUMBER : type == T_LIST ? PP_LIST : PP_TEXT)) return "binding: param of the wrong type";
        v->src = PV_PARAM; v->idx = (int16_t)i;
        return NULL;
    }
    if (cJSON_IsString(j) && j->valuestring[0] == '@') {
        if (!c->in_each) return "binding: @field outside each";
        if (!key_ok(j->valuestring + 1)) return "binding: field name";
        v->src = PV_ELEM; v->s = astr(c->a, j->valuestring + 1);
        return v->s ? NULL : "oom";
    }
    switch (type) {
    case T_LIST: return "item: labels is a list param ($name)";
    case T_NUM:
        if (!cJSON_IsNumber(j) || !pkg_num_ok(pf, (float)j->valuedouble)) return "item: number out of range";
        v->src = PV_NUM; v->num = (float)j->valuedouble;
        return NULL;
    case T_COLOR:
        if (!cJSON_IsString(j) || !pkg_color(j->valuestring, &v->rgb)) return "item: color";
        v->src = PV_COLOR;
        return NULL;
    case T_ASSET: case T_IMAGE: {
        if (!cJSON_IsString(j)) return "item: asset";
        int i = asset_index(c->p, j->valuestring);
        if (i < 0) return "item: unresolved asset";
        if (type == T_IMAGE && c->p->assets[i].kind != PA_IMAGE) return "item: img must be an image";
        v->src = PV_ASSET; v->idx = (int16_t)i;
        return NULL;
    }
    default:
        if (!cJSON_IsString(j) || !pkg_text_ok(j->valuestring, type == T_SAY ? PKG_SAY_MAX : PKG_STR_MAX)) return "item: str";
        v->src = PV_STR; v->s = astr(c->a, j->valuestring);
        return v->s ? NULL : "oom";
    }
}

static const char *parse_item(ictx_t *c, const cJSON *j, pkg_item_t *it) {
    if (!cJSON_IsObject(j)) return "item: not an object";
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(j, "k");
    if (!cJSON_IsString(k)) return "item: k";
    int kind = -1;
    for (int i = 0; i < PK_KINDS; i++) if (!strcmp(k->valuestring, PKG_KIND_NAMES[i])) kind = i;
    if (kind < 0) return "item: unknown kind";
    it->k = (uint8_t)kind; it->each = -1;
    const cJSON *each = cJSON_GetObjectItemCaseSensitive(j, "each");
    if (each) {
        int i = cJSON_IsString(each) && each->valuestring[0] == '$' ? param_index(c->l, each->valuestring + 1) : -1;
        if (i < 0) return "binding: each needs a declared $param";
        if (c->l->params[i].type != PP_LIST) return "binding: each needs a list param";
        it->each = (int8_t)i;
    }
    c->in_each = it->each >= 0;
    uint64_t have = 0;
    for (const cJSON *f = j->child; f; f = f->next) {
        if (!strcmp(f->string, "k") || !strcmp(f->string, "each")) continue;
        if (!strcmp(f->string, "polar")) {
            if (!(XY & KB(kind))) return "item: unknown key";
            if (!cJSON_IsArray(f) || cJSON_GetArraySize(f) != 2) return "item: polar is [angle, radius]";
            const char *e = parse_val(c, f->child, PF_PA, T_NUM, &it->f[PF_PA]);
            if (!e) e = parse_val(c, f->child->next, PF_PR, T_NUM, &it->f[PF_PR]);
            if (e) return e;
            it->polar = true;
            continue;
        }
        if (!strcmp(f->string, "pts")) {
            if (kind != PK_LINE && kind != PK_CURVE && kind != PK_SPOT) return "item: unknown key";
            int n = cJSON_IsArray(f) ? cJSON_GetArraySize(f) : 0;
            if (n < 2 || n > (kind == PK_SPOT ? 2 : PKG_PTS_MAX)) return "item: pts";
            it->pts = aalloc(c->a, sizeof(pkg_val_t[2]) * (size_t)n);
            if (!it->pts) return "oom";
            int i = 0;
            for (const cJSON *p = f->child; p; p = p->next, i++) {
                if (!cJSON_IsArray(p) || cJSON_GetArraySize(p) != 2) return "item: a point is [x, y]";
                const char *e = parse_val(c, p->child, PF_X, T_NUM, &it->pts[i][0]);
                if (!e) e = parse_val(c, p->child->next, PF_Y, T_NUM, &it->pts[i][1]);
                if (e) return e;
            }
            it->npts = (uint8_t)n;
            continue;
        }
        int q = -1;
        for (int i = 0; i < NFIELDS && q < 0; i++) if (!strcmp(f->string, FIELDS[i].key) && (FIELDS[i].kinds & KB(kind))) q = i;
        if (q < 0) return "item: unknown key";
        const char *e = parse_val(c, f, FIELDS[q].pf, FIELDS[q].type, &it->f[FIELDS[q].pf]);
        if (e) return e;
        have |= KB(FIELDS[q].pf);
    }
    if ((have & REQUIRED[kind]) != REQUIRED[kind]) return "item: a required field is missing";
    if ((kind == PK_LINE || kind == PK_CURVE) && !it->pts) return "item: pts";
    if (it->polar && (have & (KB(PF_X) | KB(PF_Y)))) return "item: polar or x / y, not both";
    if (kind == PK_SPOT && (!(have & KB(PF_ARC)) != !(have & KB(PF_R1)) || ((have & KB(PF_R0)) && !(have & KB(PF_ARC))))) return "item: a sector is arc and r1 (and r0)";
    if ((have & KB(PF_TTL)) && !(have & KB(PF_OPEN))) return "item: ttl is for open";
    if (it->f[PF_WHEN].src != PV_NONE && (it->f[PF_WHEN].src != PV_STR || strcmp(it->f[PF_WHEN].s, "lit"))) return "item: when is \"lit\"";
    const pkg_val_t *sets = &it->f[PF_SETS];
    if (sets->src != PV_NONE && (sets->src != PV_STR || (strcmp(sets->s, "tempo") && strcmp(sets->s, "pick")))) return "item: sets is \"tempo\" or \"pick\"";
    if ((kind == PK_DIAL || kind == PK_CENTRE) && it->each >= 0) return "item: a dial or a centre is not repeated";
    return NULL;
}

/* ── layouts ───────────────────────────────────────────────────────────────── */

static const char *parse_layout(arena_t *a, const pkg_t *p, const cJSON *j, pkg_layout_t *l) {
    static const char *const KEYS[] = { "dur", "params", "items", "caption_y", NULL };
    if (!cJSON_IsObject(j) || !only(j, KEYS)) return "layout: unknown key";
    const cJSON *dur = cJSON_GetObjectItemCaseSensitive(j, "dur"), *cy = cJSON_GetObjectItemCaseSensitive(j, "caption_y");
    if (dur && (!cJSON_IsNumber(dur) || !(dur->valuedouble >= 0 && dur->valuedouble <= PKG_DUR_MAX))) return "layout: dur";
    if (cy && (!cJSON_IsNumber(cy) || !(cy->valuedouble >= -1 && cy->valuedouble <= 1))) return "layout: caption_y";
    l->dur = dur ? (float)dur->valuedouble : 0.f;
    l->caption_y = cy ? (float)cy->valuedouble : PKG_CAPTION_Y;
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(j, "params");
    if (params) {
        if (!cJSON_IsObject(params) || count(params) > PKG_PARAMS_MAX) return "layout: params";
        for (const cJSON *c = params->child; c; c = c->next) {
            if (!key_ok(c->string) || !cJSON_IsString(c) || param_index(l, c->string) >= 0) return "layout: param";
            pkg_param_t *pp = &l->params[l->nparams];
            if (!strcmp(c->valuestring, "number")) pp->type = PP_NUMBER;
            else if (!strcmp(c->valuestring, "text")) pp->type = PP_TEXT;
            else if (!strcmp(c->valuestring, "list")) pp->type = PP_LIST;
            else return "layout: param type";
            if (!(pp->name = astr(a, c->string))) return "oom";
            l->nparams++;
        }
    }
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(j, "items");
    int n = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
    if (n < 1 || n > PKG_ITEMS_MAX) return "layout: items";
    if (!(l->items = aalloc(a, sizeof(pkg_item_t) * (size_t)n))) return "oom";
    ictx_t c = { p, l, false, a };
    for (const cJSON *it = items->child; it; it = it->next) {
        const char *e = parse_item(&c, it, &l->items[l->nitems]);
        if (e) return e;
        l->nitems++;
    }
    int beats = 0, centres = 0, dials[2] = { 0, 0 };
    for (int i = 0; i < l->nitems; i++) {
        const pkg_item_t *it = &l->items[i];
        if (it->k == PK_BEAT && it->each >= 0) return "layout: a beat is not repeated";
        beats += it->k == PK_BEAT; centres += it->k == PK_CENTRE;
        if (it->k == PK_DIAL) dials[it->f[PF_FACE].src == PV_NUM && it->f[PF_FACE].num == 1.f]++;
    }
    if (beats > 1 || centres > 1 || dials[0] > 1 || dials[1] > 1) return "layout: one beat, one centre and one dial a face";
    if (centres > beats) return "layout: a centre plays and stops a beat: the layout has none";
    return NULL;
}

/* ── the file ──────────────────────────────────────────────────────────────── */

static const char *parse_root(arena_t *a, pkg_t *p, const cJSON *j) {
    static const char *const KEYS[] = { "v", "id", "name", "version", "kind", "assets", "layouts", "creature", NULL };
    if (!cJSON_IsObject(j) || !only(j, KEYS)) return "package: unknown key";
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "v"), *id = cJSON_GetObjectItemCaseSensitive(j, "id");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(j, "name"), *ver = cJSON_GetObjectItemCaseSensitive(j, "version");
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(j, "kind");
    if (!cJSON_IsString(v) || strcmp(v->valuestring, "molty-pkg/1")) return "package: v";
    if (!cJSON_IsString(id) || !pkg_id_ok(id->valuestring)) return "package: id";
    if (!cJSON_IsString(name) || !pkg_text_ok(name->valuestring, PKG_TEXT_MAX)) return "package: name";
    if (!cJSON_IsString(ver) || !version_ok(ver->valuestring)) return "package: version";
    if (!cJSON_IsString(kind)) return "package: kind";
    if (!strcmp(kind->valuestring, "scenes")) p->kind = PKG_SCENES;
    else if (!strcmp(kind->valuestring, "creature")) p->kind = PKG_CREATURE;
    else return "package: kind";
    strcpy(p->id, id->valuestring); strcpy(p->name, name->valuestring); strcpy(p->version, ver->valuestring);

    const cJSON *assets = cJSON_GetObjectItemCaseSensitive(j, "assets");
    if (assets) {
        int n = count(assets);
        if (!cJSON_IsObject(assets) || n > PKG_ASSETS_MAX) return "package: assets";
        if (n && !(p->assets = aalloc(a, sizeof(pkg_asset_t) * (size_t)n))) return "oom";
        for (const cJSON *c = assets->child; c; c = c->next) {
            pkg_asset_t *as = &p->assets[p->nassets];
            if (!key_ok(c->string) || asset_index(p, c->string) >= 0) return "asset: name";
            const char *e = parse_asset(a, c, as);
            if (e) return e;
            if (!(as->name = astr(a, c->string))) return "oom";
            p->nassets++;
        }
    }
    const cJSON *layouts = cJSON_GetObjectItemCaseSensitive(j, "layouts"), *creature = cJSON_GetObjectItemCaseSensitive(j, "creature");
    if (p->kind == PKG_CREATURE) {
        /* the creature team's format (docs/PACKAGES.md section 5): kept in the file, not read here */
        return layouts ? "package: a creature has no layouts" : cJSON_IsObject(creature) ? NULL : "package: creature";
    }
    if (creature) return "package: scenes have no creature";
    int n = count(layouts);
    if (!cJSON_IsObject(layouts) || n < 1 || n > PKG_LAYOUTS_MAX) return "package: layouts";
    if (!(p->layouts = aalloc(a, sizeof(pkg_layout_t) * (size_t)n))) return "oom";
    for (const cJSON *c = layouts->child; c; c = c->next) {
        pkg_layout_t *l = &p->layouts[p->nlayouts];
        if (!key_ok(c->string) || pkg_layout(p, c->string)) return "layout: name";
        const char *e = parse_layout(a, p, c, l);
        if (e) return e;
        if (!(l->name = astr(a, c->string))) return "oom";
        p->nlayouts++;
    }
    /* a spot opens a layout of this package (a bound name is checked when it is tapped) */
    for (int i = 0; i < p->nlayouts; i++) for (int q = 0; q < p->layouts[i].nitems; q++) {
        const pkg_val_t *v = &p->layouts[i].items[q].f[PF_OPEN];
        if (v->src == PV_STR && !pkg_layout(p, v->s)) return "item: open names no layout";
    }
    return NULL;
}

pkg_t *pkg_parse(const char *json, size_t len, const char **err) {
    const char *e = NULL;
    *err = NULL;
    if (!json || len == 0 || len > PKG_FILE_MAX) { *err = "package: size"; return NULL; }
    cJSON *j = cJSON_ParseWithLength(json, len);
    if (!j) { *err = "package: not JSON"; return NULL; }
    arena_t a = { 0 };
    pkg_t *p = aalloc(&a, sizeof(pkg_t));
    if (!p) e = "oom";
    else e = parse_root(&a, p, j);
    cJSON_Delete(j);
    if (e) { afree(a.head); *err = e; return NULL; }
    p->refs = 1; p->cap = a.total; p->used = 0;
    for (blk_t *b = a.head; b; b = b->next) p->used += b->used;
    return p;
}

void pkg_ref(pkg_t *p) { if (p) __atomic_add_fetch(&p->refs, 1, __ATOMIC_ACQ_REL); }

void pkg_unref(pkg_t *p) {
    if (p && __atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL) == 0)
        afree((blk_t *)((uint8_t *)p - sizeof(blk_t)));   /* p is the first allocation of the head block */
}

const pkg_layout_t *pkg_layout(const pkg_t *p, const char *name) {
    for (int i = 0; i < p->nlayouts; i++) if (p->layouts[i].name && !strcmp(p->layouts[i].name, name)) return &p->layouts[i];
    return NULL;
}
