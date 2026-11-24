/*
 * SPDX-License-Identifier: MIT
 *
 * creature_clip.c: a rigged creature package played from its baked clips (creature_clip.h).
 * Port of device/creatures/js/clip-player.js; each function names its JS counterpart.
 */
#include "creature_clip.h"
#include "cJSON.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define CC_MALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CC_FREE(p)   heap_caps_free(p)
#else
#define CC_MALLOC(n) malloc(n)
#define CC_FREE(p)   free(p)
#endif

#define EPS 1e-5   /* a clip's end, against float clocks (clip-player.js EPS) */
static const char *const MOODS[7] = { "idle", "listening", "thinking", "speaking", "happy", "sleepy", "surprised" };
static const char *const GROUPS[CG_COUNT] = { "torso", "hindL", "hindR", "head", "legF.L", "legF.R", "tail", "bowl", "yarn",
                                              "eye", "earIn", "nose", "maw", "tongue", "whisker", "chest", "milk", "yarnline" };

typedef struct { uint8_t node, path, cubic, linear, w; uint16_t nk; uint16_t *keys; float *out; } chan_t;
typedef struct {
    char name[CC_NAME_MAX + 1];
    float dur, view; bool loop; int8_t from, to; int frames, nch; chan_t *ch;
    float sync;                         /* the walk's stride: left and joined only on it (0: any time)     */
} clip_t;
typedef struct { char name[CC_NAME_MAX + 1]; int16_t clip; float hold; } gest_t;
typedef struct { int16_t target, fam, clip; } enter_t;
/* a gesture played as a chain of clips (graph.chains): one of `first`, then `rounds` (a random count in [rmin, rmax]) of
 * `then`, never the same one twice in a row, then one of `last`; each pick from the creature's seed */
typedef struct { int16_t gest; uint8_t rmin, rmax, n[3]; int16_t pick[3][CC_CHAIN_PICK]; } chain_t;
typedef struct {
    int16_t clip;                       /* a pose clip, or -1: a turn of its nodes                           */
    float zero, rot[3], sq;             /* the clip's neutral place (0..1); a turn's degrees and squash a unit */
    int n; uint16_t *at;                /* the clip's channels it plays (those that move, on its nodes); a turn's nodes */
    float *ref;                         /* each channel at zero, 4 floats                                    */
    float by, w;                        /* this frame's place and weight                                     */
} layer_t;

struct cc {
    int nn; struct { int16_t parent; char id[CC_NAME_MAX + 1]; } node[CC_NODES_MAX];
    float *rest;                        /* nn * CC_POSE                                                    */
    int nshape; cc_shape_t shape[CC_SHAPES_MAX];
    int ncon; struct { int16_t node; float p[3]; } con[4];
    int npal; char palk[16][16]; uint32_t palv[16];
    float fps, blend;
    int nclip; clip_t clip[CC_CLIPS_MAX];
    int nfam; char fam[CC_FAMS_MAX][12]; int16_t rest_clip[CC_FAMS_MAX];
    int nedge; struct { int8_t a, b; int16_t clip; } edge[CC_EDGES_MAX];
    int16_t mood_clip[7];
    int ngest; gest_t gest[CC_GEST_MAX];
    int nenter; enter_t enter[8];
    int nchain; chain_t chain[CC_CHAIN_MAX];
    int16_t center[2];
    int nlay; layer_t lay[CC_LAYERS_MAX];
    /* play state (the JS player's s) */
    int cur_clip; double t; float blendT, blendD; float ox, oz, lockX, lockZ;
    int queue[16], nq;
    float oyaw;                         /* the heading carried between clips (rad, about up), as ox, oz carry the place */
    int chain_on, chain_left, chain_prev, chain_stage; uint32_t rng;   /* the chain playing (-1: none), its draws */
    int mood; int gesture; float gestureT; bool gstarted, pending;
    float camx, camz, camyaw; bool snap;
    float hd, hbase, px, pz, tx, tz, cx0, cz0, sense;   /* the body turned to its clip's side (hd, rad) and where it is
                                         * (px, pz; going to tx, tz), under a fixed camera; hbase: its way (turned round at
                                         * the frame's edges, sense: which way round) */
    int come; float comeT;              /* called (cc_come): 1 walking to the front, 2 attending, 3 facing the viewer,
                                         * 4 its first reaction playing                                               */
    float *cur, *from, *tmp, *pose, *laid, *world, *fromWorld, *out;   /* laid: the pose with the layers on */
};

/* ── parse ──────────────────────────────────────────────────────────────── */

static int b64v(int ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    return ch == '+' ? 62 : ch == '/' ? 63 : -1;
}
/* decode base64 into a fresh buffer; *n its bytes */
static uint8_t *b64(const char *s, size_t *n) {
    const size_t L = strlen(s);
    uint8_t *o = CC_MALLOC(L / 4 * 3 + 3);
    if (!o) return NULL;
    size_t k = 0; uint32_t acc = 0; int bits = 0;
    for (size_t i = 0; i < L; i++) {
        const int v = b64v((unsigned char)s[i]);
        if (v < 0) continue;
        acc = acc << 6 | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; o[k++] = (uint8_t)(acc >> bits & 255); }
    }
    *n = k;
    return o;
}

static bool num3(const cJSON *a, float *o, int n) {
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != n) return false;
    for (int i = 0; i < n; i++) { const cJSON *v = cJSON_GetArrayItem(a, i); if (!cJSON_IsNumber(v)) return false; o[i] = (float)v->valuedouble; }
    return true;
}
static int fam_of(const cc_t *c, const char *s) { for (int f = 0; f < c->nfam; f++) if (!strcmp(c->fam[f], s)) return f; return -1; }
static int group_of(const char *s) { for (int g = 0; g < CG_COUNT; g++) if (!strcmp(GROUPS[g], s)) return g; return -1; }
static int pal_of(const cc_t *c, const char *s) { for (int i = 0; i < c->npal; i++) if (!strcmp(c->palk[i], s)) return i; return -1; }
int cc_clip_find(const cc_t *c, const char *name) { for (int i = 0; i < c->nclip; i++) if (!strcmp(c->clip[i].name, name)) return i; return -1; }
int cc_node_find(const cc_t *c, const char *id) { for (int i = 0; i < c->nn; i++) if (!strcmp(c->node[i].id, id)) return i; return -1; }

static void quat_mat(const float *q, float *m) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w); m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w); m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w); m[7] = 2 * (y * z + x * w); m[8] = 1 - 2 * (x * x + y * y);
}

/* a packed clip (clip-pack.js, docs/CREATURES.md section 12 "Packed clips"): the same keys and integers as the plain
 * form, each series as its first number and its steps in the fewest bits that hold the largest */
typedef struct { const uint8_t *b; size_t n, pos; bool bad; } bits_t;
static uint32_t get_bits(bits_t *r, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++, r->pos++) {
        if (r->pos >> 3 >= r->n) { r->bad = true; return 0; }
        v |= (uint32_t)(r->b[r->pos >> 3] >> (r->pos & 7) & 1) << i;
    }
    return v;
}
static const char *unpack_clip(clip_t *k, const char *pk, const float scale[3]) {
    size_t nb;
    uint8_t *b = b64(pk, &nb);
    if (!b) return "oom";
    bits_t r = { b, nb, 0, false };
    const char *e = NULL;
    k->nch = (int)get_bits(&r, 16);
    k->ch = CC_MALLOC(sizeof(chan_t) * (size_t)(k->nch ? k->nch : 1));
    if (!k->ch) { CC_FREE(b); return "oom"; }
    memset(k->ch, 0, sizeof(chan_t) * (size_t)(k->nch ? k->nch : 1));
    for (int i = 0; i < k->nch && !e; i++) {
        chan_t *x = &k->ch[i];
        x->node = (uint8_t)get_bits(&r, 8);
        x->path = (uint8_t)get_bits(&r, 2);
        const uint32_t ip = get_bits(&r, 2), b8 = get_bits(&r, 1);
        x->nk = (uint16_t)get_bits(&r, 16);
        if (r.bad || x->path > 2 || ip > 2 || !x->nk || (b8 && x->path != 1)) { e = "rig: packed channel"; break; }
        x->cubic = ip == 2; x->linear = ip == 1; x->w = x->path == 1 ? 4 : 3;
        const int per = x->w * (x->cubic ? 3 : 1);
        x->keys = (uint16_t *)CC_MALLOC(sizeof(uint16_t) * x->nk);
        x->out = (float *)CC_MALLOC(sizeof(float) * (size_t)per * x->nk);
        if (!x->keys || !x->out) { e = "oom"; break; }
        x->keys[0] = (uint16_t)get_bits(&r, 16);
        if (x->nk > 1) { const int w = (int)get_bits(&r, 5); for (int q = 1; q < x->nk; q++) x->keys[q] = (uint16_t)(x->keys[q - 1] + get_bits(&r, w) + 1); }
        for (int cpt = 0; cpt < per; cpt++) {
            int32_t v = (int32_t)get_bits(&r, b8 ? 8 : 16);
            v = b8 ? (int32_t)(int8_t)(uint8_t)v : (int32_t)(int16_t)(uint16_t)v;
            const int w = x->nk > 1 ? (int)get_bits(&r, 5) : 0;
            for (int q = 0; q < x->nk; q++) {
                if (q) { const uint32_t z = get_bits(&r, w); v += z & 1 ? -(int32_t)((z + 1) >> 1) : (int32_t)(z >> 1); }
                x->out[(size_t)q * (size_t)per + (size_t)cpt] = b8 ? (float)v / 127.f : (float)v / scale[x->path];
            }
        }
        if (r.bad) e = "rig: packed channel";
        for (int q = 1; q < x->nk && !e; q++) if (x->keys[q] <= x->keys[q - 1]) e = "rig: keys out of order";
    }
    CC_FREE(b);
    return e;
}

static const char *parse_clip(cc_t *c, clip_t *k, const cJSON *j, const float scale[3]) {
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "dur"), *lp = cJSON_GetObjectItemCaseSensitive(j, "loop"),
                *fr = cJSON_GetObjectItemCaseSensitive(j, "from"), *to = cJSON_GetObjectItemCaseSensitive(j, "to"),
                *vw = cJSON_GetObjectItemCaseSensitive(j, "view"), *nf = cJSON_GetObjectItemCaseSensitive(j, "frames"),
                *ch = cJSON_GetObjectItemCaseSensitive(j, "ch"), *pk = cJSON_GetObjectItemCaseSensitive(j, "pk");
    if (!cJSON_IsNumber(d) || !cJSON_IsBool(lp) || !cJSON_IsString(fr) || !cJSON_IsString(to) || !cJSON_IsNumber(vw) ||
        !cJSON_IsNumber(nf) || !(cJSON_IsArray(ch) ^ cJSON_IsString(pk)) || (ch && pk)) return "rig: clip fields";
    k->dur = (float)d->valuedouble; k->loop = cJSON_IsTrue(lp); k->view = (float)vw->valuedouble; k->frames = nf->valueint;
    k->from = (int8_t)fam_of(c, fr->valuestring); k->to = (int8_t)fam_of(c, to->valuestring);
    if (k->from < 0 || k->to < 0 || k->dur <= 0.f || k->frames < 1) return "rig: clip family or length";
    if (pk) {
        const char *e = unpack_clip(k, pk->valuestring, scale);
        if (e) return e;
        for (int i = 0; i < k->nch; i++) if (k->ch[i].node >= c->nn) return "rig: channel node";
        return NULL;
    }
    k->nch = cJSON_GetArraySize(ch);
    k->ch = CC_MALLOC(sizeof(chan_t) * (size_t)(k->nch ? k->nch : 1));
    if (!k->ch) return "oom";
    memset(k->ch, 0, sizeof(chan_t) * (size_t)(k->nch ? k->nch : 1));
    int i = 0;
    for (const cJSON *h = ch->child; h; h = h->next, i++) {
        chan_t *x = &k->ch[i];
        /* [node, path, interpolation S|L|C, bytes per component 1|2, input, output] */
        if (!cJSON_IsArray(h) || cJSON_GetArraySize(h) != 6) return "rig: channel";
        const cJSON *nd = cJSON_GetArrayItem(h, 0), *pa = cJSON_GetArrayItem(h, 1), *ip = cJSON_GetArrayItem(h, 2),
                    *ctn = cJSON_GetArrayItem(h, 3), *in = cJSON_GetArrayItem(h, 4), *out = cJSON_GetArrayItem(h, 5);
        if (!cJSON_IsNumber(nd) || !cJSON_IsString(pa) || !cJSON_IsString(ip) || !cJSON_IsNumber(ctn) || !cJSON_IsString(in) || !cJSON_IsString(out)) return "rig: channel fields";
        if (nd->valueint < 0 || nd->valueint >= c->nn) return "rig: channel node";
        x->node = (uint8_t)nd->valueint;
        const char p = pa->valuestring[0];
        x->path = p == 't' ? 0 : p == 'r' ? 1 : p == 's' ? 2 : 9;
        if (x->path > 2 || pa->valuestring[1]) return "rig: channel path";
        x->w = x->path == 1 ? 4 : 3;
        if (!strcmp(ip->valuestring, "C")) x->cubic = 1;
        else if (!strcmp(ip->valuestring, "L")) x->linear = 1;
        else if (strcmp(ip->valuestring, "S")) return "rig: interpolation";
        if (ctn->valueint != 1 && ctn->valueint != 2) return "rig: component size";
        const bool b8 = ctn->valueint == 1;
        if (b8 && x->path != 1) return "rig: int8 outputs are rotations only";
        size_t nb, no;
        uint8_t *kb = b64(in->valuestring, &nb), *ob = b64(out->valuestring, &no);
        if (!kb || !ob) { CC_FREE(kb); CC_FREE(ob); return "oom"; }
        const bool k8 = k->frames < 256;                    /* key frames: one byte each under 256 frames */
        x->nk = (uint16_t)(k8 ? nb : nb / 2);
        const size_t per = (size_t)x->w * (x->cubic ? 3 : 1), vb = b8 ? 1 : 2;
        if (!x->nk || no / vb != per * x->nk || no % vb) { CC_FREE(kb); CC_FREE(ob); return "rig: channel size"; }
        x->keys = (uint16_t *)CC_MALLOC(sizeof(uint16_t) * x->nk);
        x->out = (float *)CC_MALLOC(sizeof(float) * per * x->nk);
        if (!x->keys || !x->out) { CC_FREE(kb); CC_FREE(ob); return "oom"; }
        for (int q = 0; q < x->nk; q++) x->keys[q] = k8 ? kb[q] : (uint16_t)(kb[2 * q] | kb[2 * q + 1] << 8);
        for (size_t q = 0; q < per * x->nk; q++)
            x->out[q] = b8 ? (float)(int8_t)ob[q] / 127.f : (float)(int16_t)(ob[2 * q] | ob[2 * q + 1] << 8) / scale[x->path];
        CC_FREE(kb); CC_FREE(ob);
        for (int q = 1; q < x->nk; q++) if (x->keys[q] <= x->keys[q - 1]) return "rig: keys out of order";
    }
    return NULL;
}

static const char *parse(cc_t *c, const cJSON *rig) {
    if (!cJSON_IsObject(rig)) return "rig: not an object";
    const cJSON *fps = cJSON_GetObjectItemCaseSensitive(rig, "fps"), *nodes = cJSON_GetObjectItemCaseSensitive(rig, "nodes"),
                *shapes = cJSON_GetObjectItemCaseSensitive(rig, "shapes"), *clips = cJSON_GetObjectItemCaseSensitive(rig, "clips"),
                *graph = cJSON_GetObjectItemCaseSensitive(rig, "graph"), *pal = cJSON_GetObjectItemCaseSensitive(rig, "palette"),
                *sc = cJSON_GetObjectItemCaseSensitive(rig, "scale"), *con = cJSON_GetObjectItemCaseSensitive(rig, "contacts");
    if (!cJSON_IsNumber(fps) || !cJSON_IsArray(nodes) || !cJSON_IsArray(shapes) || !cJSON_IsObject(clips) || !cJSON_IsObject(graph) ||
        !cJSON_IsObject(pal) || !cJSON_IsObject(sc)) return "rig: fields";
    c->fps = (float)fps->valuedouble;
    /* the palette */
    for (const cJSON *p = pal->child; p; p = p->next) {
        if (c->npal >= 16 || !cJSON_IsString(p) || strlen(p->string) > 15 || strlen(p->valuestring) != 7) return "rig: palette";
        strcpy(c->palk[c->npal], p->string);
        c->palv[c->npal++] = (uint32_t)strtoul(p->valuestring + 1, NULL, 16);
    }
    /* the nodes */
    c->nn = cJSON_GetArraySize(nodes);
    if (c->nn < 1 || c->nn > CC_NODES_MAX) return "rig: nodes";
    c->rest = CC_MALLOC(sizeof(float) * CC_POSE * (size_t)c->nn * 6 + sizeof(float) * 12 * (size_t)c->nn * 3);
    if (!c->rest) return "oom";
    c->cur = c->rest + CC_POSE * c->nn; c->from = c->cur + CC_POSE * c->nn; c->tmp = c->from + CC_POSE * c->nn;
    c->pose = c->tmp + CC_POSE * c->nn; c->laid = c->pose + CC_POSE * c->nn;
    c->world = c->laid + CC_POSE * c->nn; c->fromWorld = c->world + 12 * c->nn; c->out = c->fromWorld + 12 * c->nn;
    int i = 0;
    for (const cJSON *n = nodes->child; n; n = n->next, i++) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(n, "id"), *pa = cJSON_GetObjectItemCaseSensitive(n, "parent");
        if (!cJSON_IsString(id) || strlen(id->valuestring) > CC_NAME_MAX || !cJSON_IsNumber(pa)) return "rig: node";
        strcpy(c->node[i].id, id->valuestring);
        c->node[i].parent = (int16_t)pa->valueint;
        if (c->node[i].parent >= i) return "rig: a node before its parent";
        float *r = c->rest + i * CC_POSE;
        if (!num3(cJSON_GetObjectItemCaseSensitive(n, "t"), r, 3) || !num3(cJSON_GetObjectItemCaseSensitive(n, "r"), r + 3, 4) ||
            !num3(cJSON_GetObjectItemCaseSensitive(n, "s"), r + 7, 3)) return "rig: node transform";
    }
    /* the shapes */
    for (const cJSON *s = shapes->child; s; s = s->next) {
        if (c->nshape >= CC_SHAPES_MAX) return "rig: too many shapes";
        cc_shape_t *x = &c->shape[c->nshape++];
        memset(x, 0, sizeof(*x)); x->q[3] = 1.f; x->eye = CC_COL_OTHER;
        const cJSON *k = cJSON_GetObjectItemCaseSensitive(s, "k"), *b = cJSON_GetObjectItemCaseSensitive(s, "b"),
                    *g = cJSON_GetObjectItemCaseSensitive(s, "g"), *cl = cJSON_GetObjectItemCaseSensitive(s, "c");
        if (!cJSON_IsString(k) || !cJSON_IsNumber(b) || !cJSON_IsString(g) || !cJSON_IsString(cl)) return "rig: shape";
        static const char *const K[] = { "blob", "ell", "cap", "cone", "cyl", "patch" };
        int kk = -1; for (int q = 0; q < 6; q++) if (!strcmp(K[q], k->valuestring)) kk = q;
        const int gg = group_of(g->valuestring), pc = pal_of(c, cl->valuestring);
        if (kk < 0 || gg < 0 || pc < 0 || b->valueint < 0 || b->valueint >= c->nn) return "rig: shape kind, group, colour or node";
        x->kind = (uint8_t)kk; x->node = (uint8_t)b->valueint; x->group = (uint8_t)gg; x->col = (uint8_t)pc;
        if (gg == CG_EYE) x->eye = !strcmp(cl->valuestring, "iris") ? CC_IRIS : !strcmp(cl->valuestring, "shine") ? CC_SHINE : CC_PUPIL;
        if (!num3(cJSON_GetObjectItemCaseSensitive(s, "p"), x->p, 3)) return "rig: shape p";
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(s, "r");
        const int nr = cJSON_GetArraySize(r);
        if (nr < 2 || nr > 3 || !num3(r, x->r, nr)) return "rig: shape r";
        if (kk == CC_CAP || kk == CC_CONE || kk == CC_CYL) { if (!num3(cJSON_GetObjectItemCaseSensitive(s, "p1"), x->p1, 3)) return "rig: shape p1"; }
        if (kk == CC_BLOB || kk == CC_ELL || kk == CC_PATCH) { if (!num3(cJSON_GetObjectItemCaseSensitive(s, "q"), x->q, 4) || nr != 3) return "rig: shape q"; }
        if (kk == CC_CONE && !num3(cJSON_GetObjectItemCaseSensitive(s, "fa"), x->fa, 3)) return "rig: shape fa";
        if (kk == CC_BLOB) { const cJSON *st = cJSON_GetObjectItemCaseSensitive(s, "st"); if (!cJSON_IsNumber(st) || st->valuedouble <= 0.61) return "rig: blob st"; x->st = (float)st->valuedouble; }
        quat_mat(x->q, x->rq);
    }
    /* the paws */
    if (cJSON_IsArray(con)) for (const cJSON *p = con->child; p && c->ncon < 4; p = p->next) {
        const cJSON *nd = cJSON_GetArrayItem(p, 0);
        if (!cJSON_IsNumber(nd) || nd->valueint < 0 || nd->valueint >= c->nn || !num3(cJSON_GetArrayItem(p, 1), c->con[c->ncon].p, 3)) return "rig: contacts";
        c->con[c->ncon++].node = (int16_t)nd->valueint;
    }
    /* the graph, before the clips (they name its families) */
    const cJSON *fams = cJSON_GetObjectItemCaseSensitive(graph, "families");
    if (!cJSON_IsArray(fams) || cJSON_GetArraySize(fams) > CC_FAMS_MAX) return "rig: families";
    for (const cJSON *f = fams->child; f; f = f->next) {
        if (!cJSON_IsString(f) || strlen(f->valuestring) > 11) return "rig: family";
        strcpy(c->fam[c->nfam++], f->valuestring);
    }
    const float scale[3] = { 0, 0, 0 };
    float sv[3];
    {
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(sc, "t"), *r = cJSON_GetObjectItemCaseSensitive(sc, "r"), *s = cJSON_GetObjectItemCaseSensitive(sc, "s");
        if (!cJSON_IsNumber(t) || !cJSON_IsNumber(r) || !cJSON_IsNumber(s)) return "rig: scale";
        sv[0] = (float)t->valuedouble; sv[1] = (float)r->valuedouble; sv[2] = (float)s->valuedouble;
        (void)scale;
    }
    for (const cJSON *k = clips->child; k; k = k->next) {
        if (c->nclip >= CC_CLIPS_MAX || strlen(k->string) > CC_NAME_MAX) return "rig: clips";
        clip_t *x = &c->clip[c->nclip++];
        strcpy(x->name, k->string);
        const char *e = parse_clip(c, x, k, sv);
        if (e) return e;
    }
    const cJSON *edges = cJSON_GetObjectItemCaseSensitive(graph, "edges"), *rest = cJSON_GetObjectItemCaseSensitive(graph, "rest"),
                *bl = cJSON_GetObjectItemCaseSensitive(graph, "blend"), *ctr = cJSON_GetObjectItemCaseSensitive(graph, "center");
    if (!cJSON_IsObject(edges) || !cJSON_IsObject(rest) || !cJSON_IsNumber(bl) || !cJSON_IsArray(ctr) || cJSON_GetArraySize(ctr) != 2) return "rig: graph";
    c->blend = (float)bl->valuedouble;
    c->center[0] = (int16_t)cJSON_GetArrayItem(ctr, 0)->valueint; c->center[1] = (int16_t)cJSON_GetArrayItem(ctr, 1)->valueint;
    if (c->center[0] < 0 || c->center[0] >= c->nn || c->center[1] < 0 || c->center[1] >= c->nn) return "rig: center";
    for (const cJSON *e = edges->child; e; e = e->next) {
        char a[16], b[16]; const char *gt = strchr(e->string, '>');
        if (!gt || gt - e->string > 15 || strlen(gt + 1) > 15 || !cJSON_IsString(e) || c->nedge >= CC_EDGES_MAX) return "rig: edge";
        memcpy(a, e->string, (size_t)(gt - e->string)); a[gt - e->string] = 0; strcpy(b, gt + 1);
        const int fa = fam_of(c, a), fb = fam_of(c, b), cl = cc_clip_find(c, e->valuestring);
        if (fa < 0 || fb < 0 || cl < 0) return "rig: edge names";
        c->edge[c->nedge].a = (int8_t)fa; c->edge[c->nedge].b = (int8_t)fb; c->edge[c->nedge++].clip = (int16_t)cl;
    }
    for (int f = 0; f < c->nfam; f++) {
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(rest, c->fam[f]);
        c->rest_clip[f] = (int16_t)(cJSON_IsString(r) ? cc_clip_find(c, r->valuestring) : -1);
        if (c->rest_clip[f] < 0) return "rig: a family without its rest";
    }
    const cJSON *moods = cJSON_GetObjectItemCaseSensitive(graph, "moods"), *gest = cJSON_GetObjectItemCaseSensitive(graph, "gestures"),
                *hold = cJSON_GetObjectItemCaseSensitive(graph, "hold"), *enter = cJSON_GetObjectItemCaseSensitive(graph, "enter"),
                *sync = cJSON_GetObjectItemCaseSensitive(graph, "sync");
    for (int m = 0; m < 7; m++) {
        const cJSON *v = moods ? cJSON_GetObjectItemCaseSensitive(moods, MOODS[m]) : NULL;
        int k = cc_clip_find(c, cJSON_IsString(v) ? v->valuestring : MOODS[m]);
        c->mood_clip[m] = (int16_t)(k >= 0 ? k : c->rest_clip[fam_of(c, "sit") >= 0 ? fam_of(c, "sit") : 0]);
    }
    if (gest) for (const cJSON *g = gest->child; g; g = g->next) {
        if (c->ngest >= CC_GEST_MAX || !cJSON_IsString(g) || strlen(g->string) > CC_NAME_MAX) return "rig: gestures";
        const int k = cc_clip_find(c, g->valuestring);
        if (k < 0) return "rig: a gesture's clip";
        strcpy(c->gest[c->ngest].name, g->string); c->gest[c->ngest].clip = (int16_t)k;
        const cJSON *h = hold ? cJSON_GetObjectItemCaseSensitive(hold, g->string) : NULL;
        c->gest[c->ngest++].hold = cJSON_IsNumber(h) ? (float)h->valuedouble : 0.f;
    }
    if (enter) for (const cJSON *t = enter->child; t; t = t->next)
        for (const cJSON *f = t->child; f; f = f->next) {
            if (c->nenter >= 8 || !cJSON_IsString(f)) return "rig: enter";
            const int tg = cc_clip_find(c, t->string), fm = fam_of(c, f->string), cl = cc_clip_find(c, f->valuestring);
            if (tg < 0 || fm < 0 || cl < 0) return "rig: enter names";
            c->enter[c->nenter++] = (enter_t){ (int16_t)tg, (int16_t)fm, (int16_t)cl };
        }
    if (sync) for (const cJSON *s = sync->child; s; s = s->next) {
        const int k = cc_clip_find(c, s->string);
        if (k < 0 || !cJSON_IsNumber(s) || s->valuedouble <= 0) return "rig: sync";
        c->clip[k].sync = (float)s->valuedouble;
    }
    const cJSON *chains = cJSON_GetObjectItemCaseSensitive(graph, "chains");
    if (chains) for (const cJSON *h = chains->child; h; h = h->next) {
        static const char *const PART[3] = { "first", "then", "last" };
        chain_t *x = &c->chain[c->nchain];
        int g = -1;
        for (int i = 0; i < c->ngest; i++) if (!strcmp(c->gest[i].name, h->string)) g = i;
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(h, "rounds");
        if (c->nchain >= CC_CHAIN_MAX || g < 0 || !cJSON_IsArray(r) || cJSON_GetArraySize(r) != 2) return "rig: chain";
        const cJSON *r0 = cJSON_GetArrayItem(r, 0), *r1 = cJSON_GetArrayItem(r, 1);
        if (!cJSON_IsNumber(r0) || !cJSON_IsNumber(r1) || r0->valueint < 0 || r1->valueint < r0->valueint || r1->valueint > 16) return "rig: chain rounds";
        x->gest = (int16_t)g; x->rmin = (uint8_t)r0->valueint; x->rmax = (uint8_t)r1->valueint;
        for (int q = 0; q < 3; q++) {
            const cJSON *a = cJSON_GetObjectItemCaseSensitive(h, PART[q]);
            x->n[q] = 0;
            if (!a) continue;
            if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) > CC_CHAIN_PICK) return "rig: chain";
            for (const cJSON *v = a->child; v; v = v->next) {
                const int k = cJSON_IsString(v) ? cc_clip_find(c, v->valuestring) : -1;
                if (k < 0 || c->clip[k].loop) return "rig: a chain's clip";
                x->pick[q][x->n[q]++] = (int16_t)k;
            }
        }
        if (!x->n[1] || (!x->n[0] && !x->rmin)) return "rig: chain then";
        c->nchain++;
    }
    return NULL;
}

/* ── sampling (sampleChannel, samplePose, blendPose, forward) ─────────── */

static void sample_channel(const chan_t *h, float f, float period, bool loop, float *dst) {
    const int n = h->nk, w = h->w, stride = h->cubic ? 3 * w : w, vo = h->cubic ? w : 0;
    if (n == 1 || (!h->cubic && !h->linear)) { for (int k = 0; k < w; k++) dst[k] = h->out[vo + k]; return; }
    int a = n - 1, b = 0; float fa = h->keys[n - 1], fb = period;
    if (!loop || f < h->keys[n - 1]) {
        if (f <= h->keys[0]) { for (int k = 0; k < w; k++) dst[k] = h->out[vo + k]; return; }
        if (f >= h->keys[n - 1]) { for (int k = 0; k < w; k++) dst[k] = h->out[(n - 1) * stride + vo + k]; return; }
        int lo = 0, hi = n - 1;
        while (hi - lo > 1) { const int m = (lo + hi) >> 1; if (h->keys[m] <= f) lo = m; else hi = m; }
        a = lo; b = hi; fa = h->keys[lo]; fb = h->keys[hi];
    }
    const float td = fb - fa, u = (f - fa) / td;
    if (h->linear) {                                     /* LINEAR (rotations: the shorter way, then normalised) */
        float sg = 1.f;
        if (h->path == 1) { float d = 0; for (int k = 0; k < 4; k++) d += h->out[a * stride + k] * h->out[b * stride + k]; sg = d < 0 ? -1.f : 1.f; }
        for (int k = 0; k < w; k++) dst[k] = h->out[a * stride + k] + (sg * h->out[b * stride + k] - h->out[a * stride + k]) * u;
    } else {
        const float u2 = u * u, u3 = u2 * u;
        const float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
        for (int k = 0; k < w; k++) {
            const float v0 = h->out[a * stride + w + k], m0 = h->out[a * stride + 2 * w + k], v1 = h->out[b * stride + w + k], m1 = h->out[b * stride + k];
            dst[k] = h00 * v0 + h10 * td * m0 + h01 * v1 + h11 * td * m1;
        }
    }
    if (h->path == 1) {
        float l = sqrtf(dst[0] * dst[0] + dst[1] * dst[1] + dst[2] * dst[2] + dst[3] * dst[3]);
        if (l <= 0.f) l = 1.f;
        for (int k = 0; k < 4; k++) dst[k] /= l;
    }
}

static void sample_pose(const cc_t *c, int ci, double t, float *pose) {
    const clip_t *k = &c->clip[ci];
    memcpy(pose, c->rest, sizeof(float) * CC_POSE * (size_t)c->nn);
    const float period = (float)k->frames;
    float f;
    if (k->loop) { double x = fmod(t * c->fps, period); if (x < 0) x += period; f = (float)x; }
    else { f = (float)(t * c->fps); if (f > period - 1) f = period - 1; }
    float tmp[4];
    for (int i = 0; i < k->nch; i++) {
        const chan_t *h = &k->ch[i];
        sample_channel(h, f, period, k->loop, tmp);
        if (k->loop && h->node == 0 && h->path == 0 && h->nk >= 2 && f >= h->keys[h->nk - 1]) {
            /* a loop's root travels: past its last key it carries on at the last step's pace (it does not swing back to
             * the first key's place), so a loop's end is its start plus one cycle's travel (the wrap in cc_update) */
            const int st = h->cubic ? 3 * h->w : h->w, vo = h->cubic ? h->w : 0, a = (h->nk - 2) * st + vo, b = (h->nk - 1) * st + vo;
            const float u = (f - h->keys[h->nk - 1]) / (h->keys[h->nk - 1] - h->keys[h->nk - 2]);
            for (int q = 0; q < 3; q++) tmp[q] = h->out[b + q] + (h->out[b + q] - h->out[a + q]) * u;
        }
        const int o = h->node * CC_POSE + (h->path == 0 ? 0 : h->path == 1 ? 3 : 7);
        for (int q = 0; q < h->w; q++) pose[o + q] = tmp[q];
    }
}

static void blend_pose(const float *a, const float *b, float w, float *out, int nn) {
    for (int i = 0; i < nn; i++) {
        const int o = i * CC_POSE;
        for (int k = 0; k < 3; k++) { out[o + k] = a[o + k] + (b[o + k] - a[o + k]) * w; out[o + 7 + k] = a[o + 7 + k] + (b[o + 7 + k] - a[o + 7 + k]) * w; }
        const float d = a[o + 3] * b[o + 3] + a[o + 4] * b[o + 4] + a[o + 5] * b[o + 5] + a[o + 6] * b[o + 6], sg = d < 0 ? -1.f : 1.f;
        float l = 0.f;
        for (int k = 3; k < 7; k++) { out[o + k] = a[o + k] + (sg * b[o + k] - a[o + k]) * w; l += out[o + k] * out[o + k]; }
        l = sqrtf(l); if (l <= 0.f) l = 1.f;
        for (int k = 3; k < 7; k++) out[o + k] /= l;
    }
}

static void forward(const cc_t *c, const float *pose, float ox, float oz, float *world) {
    for (int i = 0; i < c->nn; i++) {
        const float *p = pose + i * CC_POSE, x = p[3], y = p[4], z = p[5], w = p[6], sx = p[7], sy = p[8], sz = p[9];
        const float L[12] = {
            (1 - 2 * (y * y + z * z)) * sx, (2 * (x * y - z * w)) * sy, (2 * (x * z + y * w)) * sz, p[0],
            (2 * (x * y + z * w)) * sx, (1 - 2 * (x * x + z * z)) * sy, (2 * (y * z - x * w)) * sz, p[1],
            (2 * (x * z - y * w)) * sx, (2 * (y * z + x * w)) * sy, (1 - 2 * (x * x + y * y)) * sz, p[2] };
        float *W = world + i * 12;
        const int pa = c->node[i].parent;
        if (pa < 0) {
            memcpy(W, L, sizeof(L));
            if (c->oyaw != 0.f) {                       /* the carried heading: turned about up, through the origin */
                const float cs = cosf(c->oyaw), sn = sinf(c->oyaw);
                for (int col = 0; col < 4; col++) { W[col] = cs * L[col] + sn * L[8 + col]; W[8 + col] = -sn * L[col] + cs * L[8 + col]; }
            }
            W[3] += ox; W[11] += oz; continue;
        }
        const float *P = world + pa * 12;
        for (int r = 0; r < 3; r++) for (int col = 0; col < 4; col++)
            W[r * 4 + col] = P[r * 4] * L[col] + P[r * 4 + 1] * L[4 + col] + P[r * 4 + 2] * L[8 + col] + (col == 3 ? P[r * 4 + 3] : 0.f);
    }
}

static void point_on(const float *w, int n, const float *p, float *o) {
    const float *m = w + n * 12;
    o[0] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
    o[1] = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
    o[2] = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
}

/* ── life layers ───────────────────────────────────────────────────────── */

/* creature.life.layers: [{ by, w?, clip, zero?, nodes? } or { by, w?, nodes, rot?, sq? }]; by and w name the creature
 * player's signals (creature_pkg.c binds them) */
static const char *parse_layers(cc_t *c, const cJSON *a) {
    if (!a) return NULL;
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) > CC_LAYERS_MAX) return "life: layers";
    static const char *const K[] = { "by", "w", "clip", "zero", "nodes", "rot", "sq", NULL };
    for (const cJSON *j = a->child; j; j = j->next) {
        layer_t *L = &c->lay[c->nlay++];
        memset(L, 0, sizeof(*L)); L->clip = -1;
        if (!cJSON_IsObject(j) || !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(j, "by"))) return "life: a layer";
        for (const cJSON *k = j->child; k; k = k->next) {
            bool ok = false;
            for (int i = 0; K[i]; i++) ok |= !strcmp(k->string, K[i]);
            if (!ok) return "life: a layer";
        }
        const cJSON *cl = cJSON_GetObjectItemCaseSensitive(j, "clip"), *z = cJSON_GetObjectItemCaseSensitive(j, "zero"),
                    *nodes = cJSON_GetObjectItemCaseSensitive(j, "nodes"), *rot = cJSON_GetObjectItemCaseSensitive(j, "rot"),
                    *sq = cJSON_GetObjectItemCaseSensitive(j, "sq");
        bool on[CC_NODES_MAX];
        for (int i = 0; i < c->nn; i++) on[i] = !nodes;
        if (nodes && (!cJSON_IsArray(nodes) || !cJSON_GetArraySize(nodes))) return "life: a layer's nodes";
        if (nodes) for (const cJSON *n = nodes->child; n; n = n->next) {
            if (!cJSON_IsNumber(n) || n->valueint < 0 || n->valueint >= c->nn) return "life: a layer's nodes";
            on[n->valueint] = true;
        }
        if (cl) {
            if (!cJSON_IsString(cl) || (L->clip = (int16_t)cc_clip_find(c, cl->valuestring)) < 0 || rot || sq) return "life: a layer's clip";
            if (z && (!cJSON_IsNumber(z) || z->valuedouble < 0 || z->valuedouble > 1)) return "life: a layer's zero";
            L->zero = z ? (float)z->valuedouble : 0.f;
            const clip_t *k = &c->clip[L->clip];
            L->at = CC_MALLOC(sizeof(uint16_t) * (size_t)(k->nch ? k->nch : 1));
            L->ref = CC_MALLOC(sizeof(float) * 4 * (size_t)(k->nch ? k->nch : 1));
            if (!L->at || !L->ref) return "oom";
            for (int i = 0; i < k->nch; i++) {          /* a held channel adds nothing */
                if (!on[k->ch[i].node] || k->ch[i].nk < 2) continue;
                sample_channel(&k->ch[i], L->zero * (float)(k->frames - 1), (float)k->frames, false, L->ref + 4 * L->n);
                L->at[L->n++] = (uint16_t)i;
            }
        } else {
            if (!nodes || z || (!rot && !sq) || (rot && !num3(rot, L->rot, 3)) || (sq && !cJSON_IsNumber(sq))) return "life: a layer's turn";
            L->sq = sq ? (float)sq->valuedouble : 0.f;
            if (!(L->at = CC_MALLOC(sizeof(uint16_t) * (size_t)c->nn))) return "oom";
            for (int i = 0; i < c->nn; i++) if (on[i]) L->at[L->n++] = (uint16_t)i;
        }
    }
    return NULL;
}

static void qmul(const float *a, const float *b, float *o) {   /* x y z w */
    const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
/* q turned on by d in its own frame, d's share w of the way from no turn (nlerp) */
static void turn_by(float *q, const float *d, float w) {
    const float sg = d[3] < 0 ? -1.f : 1.f, e[4] = { sg * w * d[0], sg * w * d[1], sg * w * d[2], 1.f - w + sg * w * d[3] };
    float r[4]; qmul(q, e, r);
    const float l = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
    for (int k = 0; k < 4; k++) q[k] = r[k] / (l > 0.f ? l : 1.f);
}

/* the layers over c->cur into c->laid; false (laid untouched) when none plays */
static bool layers(cc_t *c) {
    int on = 0;
    for (int i = 0; i < c->nlay && on < CC_LAYERS_ON; i++) {
        const layer_t *L = &c->lay[i];
        const float w = L->w > 1.f ? 1.f : L->w;
        if (!(w > 0.01f)) continue;
        if (!on++) memcpy(c->laid, c->cur, sizeof(float) * CC_POSE * (size_t)c->nn);
        if (L->clip < 0) {                                /* a turn: degrees and a squash a unit of its signal */
            const float D2R = 3.14159265358979f / 180.f, a[3] = { L->by * w * L->rot[0] * D2R, L->by * w * L->rot[1] * D2R, L->by * w * L->rot[2] * D2R };
            const float cx = cosf(a[0] / 2), sx = sinf(a[0] / 2), cy = cosf(a[1] / 2), sy = sinf(a[1] / 2), cz = cosf(a[2] / 2), sz = sinf(a[2] / 2);
            const float d[4] = { sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz, cx * cy * sz - sx * sy * cz, cx * cy * cz + sx * sy * sz };   /* as a bone's turn (body.mjs) */
            const float y = fmaxf(0.3f, 1.f + L->by * w * L->sq), xz = 1.f / sqrtf(y);
            for (int j = 0; j < L->n; j++) {
                float *o = c->laid + L->at[j] * CC_POSE;
                turn_by(o + 3, d, 1.f);
                o[7] *= xz; o[8] *= y; o[9] *= xz;
            }
            continue;
        }
        const clip_t *k = &c->clip[L->clip];
        const float u = fminf(1.f, fmaxf(0.f, L->by >= 0.f ? L->zero + L->by * (1.f - L->zero) : L->zero + L->by * L->zero));
        for (int j = 0; j < L->n; j++) {                  /* its difference from the neutral place, added */
            const chan_t *h = &k->ch[L->at[j]];
            const float *r = L->ref + 4 * j;
            float v[4];
            sample_channel(h, u * (float)(k->frames - 1), (float)k->frames, false, v);
            float *o = c->laid + h->node * CC_POSE;
            if (h->path == 0) for (int q = 0; q < 3; q++) o[q] += w * (v[q] - r[q]);
            else if (h->path == 2) for (int q = 0; q < 3; q++) o[7 + q] *= 1.f + w * (r[q] != 0.f ? v[q] / r[q] - 1.f : 0.f);
            else { const float ri[4] = { -r[0], -r[1], -r[2], r[3] }; float d[4]; qmul(ri, v, d); turn_by(o + 3, d, w); }
        }
    }
    return on > 0;
}

int  cc_layers(const cc_t *c) { return c->nlay; }
void cc_layer(cc_t *c, int i, float by, float w) { if (i >= 0 && i < c->nlay) { c->lay[i].by = by; c->lay[i].w = w; } }

/* ── routing (route, bestPhase, start, plan, advance) ─────────────────── */

int cc_route(const cc_t *c, int a, int b, int *out, int max) {
    if (a == b) return 0;
    int prev_f[CC_FAMS_MAX], prev_c[CC_FAMS_MAX], q[CC_FAMS_MAX], qh = 0, qt = 0;
    bool seen[CC_FAMS_MAX] = { false };
    seen[a] = true; q[qt++] = a;
    while (qh < qt) {
        const int f = q[qh++];
        for (int e = 0; e < c->nedge; e++) {
            const int x = c->edge[e].a, y = c->edge[e].b;
            if (x != f || seen[y]) continue;
            seen[y] = true; prev_f[y] = f; prev_c[y] = c->edge[e].clip;
            if (y == b) {
                int r[CC_FAMS_MAX], n = 0;
                for (int s = b; s != a; s = prev_f[s]) r[n++] = prev_c[s];
                if (n > max) return -1;
                for (int i = 0; i < n; i++) out[i] = r[n - 1 - i];
                return n;
            }
            q[qt++] = y;
        }
    }
    return -1;
}

/* The camera stays where it started, looking from the first clip's side: the floor never moves, so a walking frame
 * changes only the body's cells. The body turns instead (about its centre: the same picture of it as the camera going
 * round), to each clip's side, and walks on the floor in the way it faces; near the frame's side edges, walking
 * outward, it turns round (an arc, as it walks). c->world is the clip's pose (root motion included); c->out the same
 * turned and placed (clip-player.js turn()). */
#define CC_ATTEND "listening"           /* called, it sits up attending in this clip (ears up, eyes on the viewer)    */
#define CC_ATTEND_S 3.f                 /* for this long, then back to its mood                                       */
#define CC_COME_R 0.08f                 /* it is at the front this near the middle across (world units), facing front */
#define CC_COME_K 3.f                   /* how hard it steers to the middle while it comes                           */
#define CC_COME_S 8.f                   /* ...or after walking this long                                              */
#define CC_FRONT 0.15f                  /* facing front this near the middle (across the frame) it only perks up       */
static void attend(cc_t *c);
#define CC_AWAY 1.75f                   /* a chain that leaves it facing further from the viewer than this (rad) calls it */
#define CC_PACE 0.6f                    /* the most it moves a second (world units): above a walk's 0.36           */
#define CC_EDGE 0.25f                   /* walking past this far from the frame's centre (world units, across) turns it */
static void turn(cc_t *c, float dt) {
    const clip_t *cc = &c->clip[c->cur_clip];
    const float *W = c->world;
    const float cx = (W[c->center[0] * 12 + 3] + W[c->center[1] * 12 + 3]) / 2, cz = (W[c->center[0] * 12 + 11] + W[c->center[1] * 12 + 11]) / 2;
    const float D2R = 3.14159265358979f / 180.f;
    if (c->snap) {
        c->camx = c->px = c->tx = cx; c->camz = c->pz = c->tz = cz; c->camyaw = cc->view; c->hd = c->hbase = c->sense = 0.f; c->snap = false;
    } else {
        const float dx = cx - c->cx0, dz = cz - c->cz0, s = sinf(c->hd), k = cosf(c->hd);
        const float wx = k * dx + s * dz, wz = -s * dx + k * dz;        /* this frame's travel, turned as the body is */
        /* where it should be (tx, tz); it goes there at most CC_PACE a second: a walk's own pace, and a crossfade's jump
         * of the centre eased out (as the following camera did) */
        c->tx += wx; c->tz += wz;
        const float ex = c->tx - c->px, ez = c->tz - c->pz, el = sqrtf(ex * ex + ez * ez), mv = CC_PACE * dt;
        const float q = el > mv ? mv / el : 1.f;
        c->px += ex * q; c->pz += ez * q;
        const float rx = cosf(c->camyaw * D2R), rz = -sinf(c->camyaw * D2R);   /* across the frame */
        const float lat = (c->px - c->camx) * rx + (c->pz - c->camz) * rz, vlat = wx * rx + wz * rz;
        float want = c->hbase + (c->camyaw - cc->view) * D2R, gap = want - c->hd;
        gap -= 6.28318530718f * roundf(gap / 6.28318530718f);
        const int wf = fam_of(c, "walk");
        const bool walking = wf >= 0 && cc->to == wf;
        if (c->come == 1 && walking) {
            /* called: it walks toward the viewer, steering to the middle across the frame, and stops there facing front
             * (its stop carries it on toward the viewer, which does not show) */
            const float fx = sinf(c->camyaw * D2R), fz = cosf(c->camyaw * D2R);         /* toward the viewer, on the floor */
            const float vx = fx - CC_COME_K * lat * rx, vz = fz - CC_COME_K * lat * rz;
            c->comeT += dt;
            if ((fabsf(lat) < CC_COME_R && sinf(c->hd) * fx + cosf(c->hd) * fz > 0.9f) || c->comeT > CC_COME_S) {
                attend(c);                             /* its stop still walks on toward the viewer */
            } else {                                   /* its way: the walk's travel (+z of the clip) along v */
                c->hbase = atan2f(vx, vz) - c->oyaw - (c->camyaw - cc->view) * D2R; c->sense = 0.f;
            }
        }
        if (c->come == 2 && cc->to != wf && cc->from != wf) {   /* out of the walk: it turns to face the viewer */
            c->hbase = 6.28318530718f * floorf((c->hd + c->oyaw - (c->camyaw - cc->view) * D2R) / 6.28318530718f + 0.5f) - c->oyaw; c->sense = 0.f;
            c->come = 3;
        }
        if (walking && !c->come && fabsf(lat) > CC_EDGE && lat * vlat > 0.05f * dt * fabsf(lat) && fabsf(gap) < 0.3f) {
            /* turn round, the way whose arc bends it back toward the frame's middle in depth (turning the same way
             * every time would carry it off into the distance) */
            const float dep = -(c->px - c->camx) * rz + (c->pz - c->camz) * rx, bend = wz * -rz - wx * rx;   /* depth; its change per +turn */
            c->hbase += 3.14159265358979f; c->sense = dep * bend > 0.f ? -1.f : 1.f;
        }
        want = c->hbase + (c->camyaw - cc->view) * D2R; gap = want - c->hd;
        gap -= 6.28318530718f * roundf(gap / 6.28318530718f);
        if (c->sense != 0.f && fabsf(gap) < 1.f) c->sense = 0.f;
        else if (c->sense * gap < 0.f) gap += c->sense * 6.28318530718f;
        c->hd += gap * (1.f - expf(-2.f * dt));
    }
    c->cx0 = cx; c->cz0 = cz;
    const float s = sinf(c->hd), k = cosf(c->hd);
    for (int i = 0; i < c->nn; i++) {                                  /* out = T(p) R(hd) T(-centre) world */
        const float *m = W + i * 12; float *o = c->out + i * 12;
        for (int r = 0; r < 4; r++) {
            const float a = m[r], b = m[4 + r], e = m[8 + r];
            o[r] = k * a + s * e; o[4 + r] = b; o[8 + r] = -s * a + k * e;
        }
        const float tx = m[3] - cx, tz = m[11] - cz;
        o[3] = k * tx + s * tz + c->px; o[11] = -s * tx + k * tz + c->pz;
    }
}

static float ease(float x) { return x <= 0.f ? 0.f : x >= 1.f ? 1.f : x * x * (3.f - 2.f * x); }

static void root_at(cc_t *c, int ci, double t, float *x, float *z) { sample_pose(c, ci, t, c->tmp); *x = c->tmp[0]; *z = c->tmp[2]; }

static double best_phase(cc_t *c, int ci, const float *target) {
    const clip_t *k = &c->clip[ci];
    int best = 0; double bd = 1e30;
    for (int f = 0; f < k->frames; f++) {
        sample_pose(c, ci, (double)f / c->fps, c->tmp);
        double d = 0;
        for (int i = 1; i < c->nn; i++) {
            const int o = i * CC_POSE;
            d += 1.0 - fabs((double)c->tmp[o + 3] * target[o + 3] + (double)c->tmp[o + 4] * target[o + 4] + (double)c->tmp[o + 5] * target[o + 5] + (double)c->tmp[o + 6] * target[o + 6]);
        }
        if (d < bd - 1e-9) { bd = d; best = f; }
    }
    return (double)best / c->fps;
}

/* the root's turn about up from its rest (rad; 0 for a clip that never turns it) */
static float root_yaw(const cc_t *c, const float *pose) {
    const float rc[4] = { -c->rest[3], -c->rest[4], -c->rest[5], c->rest[6] };
    float d[4]; qmul(pose + 3, rc, d);
    const float a = 2.f * atan2f(d[3] < 0.f ? -d[1] : d[1], fabsf(d[3]));
    return fabsf(a) < 1e-4f ? 0.f : a;
}

static void start(cc_t *c, int ci, float blend) {
    const clip_t *k = &c->clip[ci];
    const float rx = c->cur[0], rz = c->cur[2], ry = root_yaw(c, c->cur);
    memcpy(c->from, c->cur, sizeof(float) * CC_POSE * (size_t)c->nn);
    memcpy(c->fromWorld, c->world, sizeof(float) * 12 * (size_t)c->nn);
    double t0 = 0;
    if (k->loop && k->sync > 0.f) t0 = best_phase(c, ci, c->cur);
    float nx, nz;
    root_at(c, ci, t0, &nx, &nz);
    const float ny = root_yaw(c, c->tmp);
    if (ry == 0.f && ny == 0.f && c->oyaw == 0.f) { c->ox += rx - nx; c->oz += rz - nz; }
    else {                                    /* the heading carries too: the new clip goes on facing the way this one ended */
        const float y0 = c->oyaw, y1 = remainderf(y0 + ry - ny, 6.28318530718f), c0 = cosf(y0), s0 = sinf(y0), c1 = cosf(y1), s1 = sinf(y1);
        const float ox = c->ox + c0 * rx + s0 * rz - (c1 * nx + s1 * nz), oz = c->oz - s0 * rx + c0 * rz - (-s1 * nx + c1 * nz);
        const float hq[4] = { 0.f, sinf((y0 - y1) / 2), 0.f, cosf((y0 - y1) / 2) };
        for (int i = 0; i < c->nn; i++) {      /* the crossfade's start, seen in the new frame: where it was */
            if (c->node[i].parent >= 0) continue;
            float *p = c->from + i * CC_POSE, q[4];
            const float wx = c0 * p[0] + s0 * p[2] + c->ox - ox, wz = -s0 * p[0] + c0 * p[2] + c->oz - oz;
            p[0] = c1 * wx - s1 * wz; p[2] = s1 * wx + c1 * wz;
            qmul(hq, p + 3, q); memcpy(p + 3, q, sizeof(q));
        }
        c->ox = ox; c->oz = oz; c->oyaw = y1;
    }
    c->cur_clip = ci; c->t = t0; c->blendT = 0.f; c->blendD = blend;
}

/* a draw from the creature's seed, 0..n-1, never `avoid` (when it is one of the n and there is another) */
static int draw(cc_t *c, const int16_t *k, int n, int avoid) {
    int m = n, skip = -1;
    for (int i = 0; i < n && n > 1; i++) if (k[i] == avoid) { skip = i; m = n - 1; }
    c->rng = c->rng * 1664525u + 1013904223u;
    int r = (int)((c->rng >> 8) % (uint32_t)m);
    if (skip >= 0 && r >= skip) r++;
    return k[r];
}
/* a chain's next clip (-1: it is done) */
static int chain_next(cc_t *c) {
    if (c->chain_on < 0) return -1;
    const chain_t *h = &c->chain[c->chain_on];
    if (c->chain_left > 0) { c->chain_left--; return c->chain_prev = draw(c, h->pick[1], h->n[1], c->chain_prev); }
    if (!c->chain_stage && h->n[2]) { c->chain_stage = 1; return draw(c, h->pick[2], h->n[2], -1); }
    c->chain_on = -1;
    return -1;
}

static int target(const cc_t *c) { return c->gesture >= 0 ? c->gest[c->gesture].clip : c->mood_clip[c->mood]; }

static void plan(cc_t *c) {
    const int want = target(c), fam = c->clip[c->cur_clip].to;
    c->nq = 0;
    if (want == c->cur_clip && c->clip[c->cur_clip].loop) return;
    for (int e = 0; e < c->nenter; e++)
        if (c->enter[e].target == want && c->enter[e].fam == fam) {   /* an authored way in (the startle) */
            if (c->enter[e].clip != want) c->queue[c->nq++] = c->enter[e].clip;
            c->queue[c->nq++] = want;
            return;
        }
    int r[CC_FAMS_MAX];
    const int n = cc_route(c, fam, c->clip[want].from, r, CC_FAMS_MAX);
    for (int i = 0; i < n; i++) c->queue[c->nq++] = r[i];
    c->queue[c->nq++] = want;
}

static void advance(cc_t *c) {
    if (!c->nq) plan(c);
    if (!c->nq) return;
    const int next = c->queue[0];
    memmove(c->queue, c->queue + 1, sizeof(int) * (size_t)(--c->nq));
    if (c->gesture >= 0 && next == c->gest[c->gesture].clip) c->gstarted = true;
    start(c, next, c->blend);
}

/* ── the API ───────────────────────────────────────────────────────────── */

cc_t *cc_create(const void *crv, const char **err) {
    const cJSON *cr = crv, *rig = cJSON_GetObjectItemCaseSensitive(cr, "rig");
    cc_t *c = CC_MALLOC(sizeof(cc_t));
    if (!c) { *err = "oom"; return NULL; }
    memset(c, 0, sizeof(*c));
    const char *e = parse(c, rig);
    if (!e) e = parse_layers(c, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cr, "life"), "layers"));
    if (e) { cc_destroy(c); *err = e; return NULL; }
    const int sit = fam_of(c, "sit");
    c->cur_clip = c->rest_clip[sit >= 0 ? sit : 0];
    c->gesture = -1; c->chain_on = -1; cc_seed(c, 1);
    sample_pose(c, c->cur_clip, 0, c->cur);
    c->blendT = c->blendD = 1.f;
    forward(c, c->cur, 0, 0, c->world);
    memcpy(c->out, c->world, sizeof(float) * 12 * (size_t)c->nn);
    c->camyaw = c->clip[c->cur_clip].view; c->snap = true;
    *err = NULL;
    return c;
}

void cc_destroy(cc_t *c) {
    if (!c) return;
    for (int i = 0; i < c->nclip; i++) {
        for (int k = 0; k < c->clip[i].nch; k++) { CC_FREE(c->clip[i].ch[k].keys); CC_FREE(c->clip[i].ch[k].out); }
        CC_FREE(c->clip[i].ch);
    }
    for (int i = 0; i < c->nlay; i++) { CC_FREE(c->lay[i].at); CC_FREE(c->lay[i].ref); }
    CC_FREE(c->rest);
    CC_FREE(c);
}

int cc_nodes(const cc_t *c) { return c->nn; }
const char *cc_node_id(const cc_t *c, int i) { return c->node[i].id; }
int cc_shapes(const cc_t *c, const cc_shape_t **out) { *out = c->shape; return c->nshape; }
uint32_t cc_color(const cc_t *c, const char *name) { const int i = pal_of(c, name); return i < 0 ? 0 : c->palv[i]; }
uint32_t cc_color_at(const cc_t *c, int i) { return i >= 0 && i < c->npal ? c->palv[i] : 0; }
int cc_clips(const cc_t *c) { return c->nclip; }
const char *cc_clip_name(const cc_t *c, int i) { return c->clip[i].name; }
const float *cc_world(const cc_t *c) { return c->out; }
void cc_cam(const cc_t *c, float *x, float *z, float *yaw) { *x = c->camx; *z = c->camz; *yaw = c->camyaw; }
int cc_clip(const cc_t *c, float *t) { if (t) *t = (float)c->t; return c->cur_clip; }
int cc_queue(const cc_t *c, int *out, int max) { int n = c->nq < max ? c->nq : max; memcpy(out, c->queue, sizeof(int) * (size_t)n); return c->nq; }
int cc_families(const cc_t *c) { return c->nfam; }
const char *cc_family_name(const cc_t *c, int f) { return c->fam[f]; }
int cc_clip_from(const cc_t *c, int k) { return c->clip[k].from; }
int cc_clip_to(const cc_t *c, int k) { return c->clip[k].to; }
void cc_seed(cc_t *c, uint32_t seed) {   /* mixed, so near seeds draw far apart */
    seed ^= seed >> 16; seed *= 0x7feb352du; seed ^= seed >> 15; seed *= 0x846ca68bu; seed ^= seed >> 16;
    c->rng = seed;
}
bool cc_clip_loop(const cc_t *c, int k) { return c->clip[k].loop; }

void cc_mood(cc_t *c, const char *mood) {
    for (int m = 0; m < 7; m++)
        if (!strcmp(MOODS[m], mood)) { if (m != c->mood) { c->mood = m; c->pending = true; } return; }
}

static bool act(cc_t *c, const char *name);
/* a call does not wait: a gesture or a rest loop playing ends now (crossfading into the way to the call), and so does a
 * clip with more than 1.2 s to go; a shorter transition finishes and then routes to it; a walk leaves at its stride's end */
static void interrupt(cc_t *c) {
    const clip_t *k = &c->clip[c->cur_clip];
    if (k->loop && k->to == fam_of(c, "walk")) return;              /* a walk leaves where a stride ends */
    c->nq = 0;
    if (k->loop || k->from == k->to || k->dur - c->t > 1.2) { c->pending = false; advance(c); }
}
static void attend(cc_t *c) {
    if (!act(c, CC_ATTEND)) { c->come = 0; return; }
    c->gest[c->gesture].hold = CC_ATTEND_S; c->come = 2;
    interrupt(c);
    if (c->gest[c->gesture].clip == c->cur_clip) { c->gstarted = true; c->pending = false; }   /* attending already: it holds */
}

bool cc_come(cc_t *c, const char *first) {
    if (first && act(c, first)) { interrupt(c); c->come = 4; return true; }   /* its own reaction first (a shake's) */
    const clip_t *cc = &c->clip[c->cur_clip];
    const float D2R = 3.14159265358979f / 180.f, Y = c->camyaw * D2R, TAU = 6.28318530718f;
    float f = c->hd + c->oyaw - (c->camyaw - cc->view) * D2R; f -= TAU * floorf(f / TAU + 0.5f);
    const float ox = c->px - c->camx, oz = c->pz - c->camz;
    const int wf = fam_of(c, "walk");
    if (fabsf(f) < 0.4f && fabsf(ox * cosf(Y) - oz * sinf(Y)) < CC_FRONT && cc->to != wf && cc->from != wf) { attend(c); return c->come != 0; }   /* it perks up */
    if (!act(c, "walk")) return false;
    interrupt(c);
    c->come = 1; c->comeT = 0.f;
    return true;
}
bool cc_coming(const cc_t *c) { return c->come != 0; }
bool cc_free(const cc_t *c) {
    const clip_t *k = &c->clip[c->cur_clip];
    return c->gesture < 0 && !c->come && !c->nq && k->loop && k->to != fam_of(c, "walk");
}

static bool act(cc_t *c, const char *name) {
    int g = -1;
    for (int i = 0; i < c->ngest; i++) if (!strcmp(c->gest[i].name, name)) g = i;
    if (g < 0) {                                   /* a clip by its own name, as the JS player allows */
        const int k = cc_clip_find(c, name);
        if (k < 0 || c->ngest >= CC_GEST_MAX) return false;
        g = c->ngest;                              /* a scratch slot past the declared ones */
        snprintf(c->gest[g].name, sizeof(c->gest[g].name), "%s", name); c->gest[g].clip = (int16_t)k; c->gest[g].hold = 0.f;
    }
    c->gesture = g; c->gestureT = 0.f; c->gstarted = false; c->pending = true;
    c->chain_on = -1;
    for (int i = 0; i < c->nchain; i++) if (c->chain[i].gest == g) {   /* a chain: its first clip now, the rest as each ends */
        const chain_t *h = &c->chain[i];
        c->chain_on = i; c->chain_stage = 0; c->chain_prev = -1;
        c->rng = c->rng * 1664525u + 1013904223u;
        c->chain_left = h->rmin + (int)((c->rng >> 8) % (uint32_t)(h->rmax - h->rmin + 1));
        if (h->n[0]) c->gest[g].clip = (int16_t)draw(c, h->pick[0], h->n[0], -1);
        else { c->gest[g].clip = (int16_t)(c->chain_prev = draw(c, h->pick[1], h->n[1], -1)); c->chain_left--; }
    }
    return true;
}

bool cc_act(cc_t *c, const char *name) { c->come = 0; return act(c, name); }   /* the agent's: it ends a call */

void cc_play(cc_t *c, const char *clip) {
    const int k = cc_clip_find(c, clip);
    if (k < 0) return;
    c->come = 0;
    c->gesture = -1; c->nq = 0; c->chain_on = -1;
    start(c, k, 0.f);
}

void cc_update(cc_t *c, float dt) {
    const int ci = c->cur_clip;
    const clip_t *k = &c->clip[ci];
    c->t += dt; c->blendT += dt;
    if (c->gesture >= 0 && c->gstarted) c->gestureT += dt;
    if (c->come == 4 && c->gesture < 0) { c->come = 0; cc_come(c, NULL); }   /* its reaction done: now it comes */
    else if (c->come >= 2 && c->come <= 3 && c->gesture < 0) c->come = 0;   /* attended: back to its mood */
    if (c->come != 1 && c->gesture >= 0 && c->gstarted && c->gest[c->gesture].hold > 0.f && c->gestureT >= c->gest[c->gesture].hold && c->clip[c->cur_clip].loop) {
        c->gesture = -1; c->pending = true;
    }
    if (k->loop) {
        if (c->pending) {
            if (k->sync <= 0.f) { c->pending = false; c->nq = 0; advance(c); }
            else if (fmod(c->t, k->sync) < dt) { c->pending = false; c->nq = 0; advance(c); }   /* only where a stride ends */
        }
        if (c->cur_clip == ci && c->t >= k->dur - EPS) {                       /* wrap: the root's travel carries on */
            float ax, az, bx, bz;
            root_at(c, ci, k->dur - 1e-6, &ax, &az); root_at(c, ci, 0, &bx, &bz);
            if (c->oyaw != 0.f) {                                               /* along the way it faces */
                const float cs = cosf(c->oyaw), sn = sinf(c->oyaw), dx = ax - bx, dz = az - bz;
                ax = bx + cs * dx + sn * dz; az = bz - sn * dx + cs * dz;
            }
            c->ox += ax - bx; c->oz += az - bz; c->t -= k->dur;
        }
    } else if (c->t >= k->dur - EPS) {
        int nx = -1; bool chained = false;
        if (c->gesture >= 0 && c->gstarted && c->gest[c->gesture].clip == c->cur_clip && !c->clip[c->cur_clip].loop) {
            chained = c->chain_on >= 0;
            if ((nx = chain_next(c)) < 0) c->gesture = -1;
        }
        c->t = k->dur;
        const float D2R = 3.14159265358979f / 180.f;
        if (nx >= 0) { c->gest[c->gesture].clip = (int16_t)nx; start(c, nx, 0.f); }   /* the chain's next: it starts where this ended */
        else if (chained && fabsf(remainderf(c->hd + c->oyaw - (c->camyaw - k->view) * D2R, 6.28318530718f)) > CC_AWAY) cc_come(c, NULL);
        else { c->pending = false; advance(c); }   /* a chain that left it facing away: it comes round to the front on its feet */
    }
    sample_pose(c, c->cur_clip, c->t, c->pose);
    const float w = ease(c->blendD > 0.f ? c->blendT / c->blendD : 1.f);
    if (w < 1.f) {
        blend_pose(c->from, c->pose, w, c->cur, c->nn);
        for (int i = 1; i < c->nn; i++)          /* a prop (a root-level node but the first) is never cross-faded: it is where
                                                  * the clip puts it, so a bowl leaving never sweeps across to where it rests */
            if (c->node[i].parent < 0) memcpy(c->cur + i * CC_POSE, c->pose + i * CC_POSE, sizeof(float) * CC_POSE);
    }
    else memcpy(c->cur, c->pose, sizeof(float) * CC_POSE * (size_t)c->nn);
    const float *P = layers(c) ? c->laid : c->cur;   /* cur stays the base: the next crossfade starts from it */
    forward(c, P, c->ox, c->oz, c->world);
    if (w < 1.f && c->ncon) {                                            /* the light paw lock */
        float dx = 0, dz = 0; int n = 0;
        for (int i = 0; i < c->ncon; i++) {
            float a[3], b[3];
            point_on(c->fromWorld, c->con[i].node, c->con[i].p, a); point_on(c->world, c->con[i].node, c->con[i].p, b);
            if (a[1] < 0.03f && b[1] < 0.03f) { dx += a[0] - b[0]; dz += a[2] - b[2]; n++; }
        }
        if (n) { const float q = (1.f - w) / n; c->lockX = dx * q; c->lockZ = dz * q; forward(c, P, c->ox + c->lockX, c->oz + c->lockZ, c->world); }
    } else { c->lockX = c->lockZ = 0.f; }
    turn(c, dt);
}
