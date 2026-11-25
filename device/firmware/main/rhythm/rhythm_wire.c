/* SPDX-License-Identifier: MIT, see rhythm_wire.h */
#include "rhythm_wire.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool name_ok(const char *s, size_t max, bool upper) {
    const size_t n = s ? strlen(s) : 0;
    if (!n || n > max) return false;
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        const bool ok = upper ? ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '/' || c == '-')
                              : ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-');
        if (!ok) return false;
    }
    return true;
}

/* a list of step numbers 1..steps → bits; false for anything else (a repeat too) */
static bool steps_of(const cJSON *a, int steps, bool need, uint32_t *out) {
    *out = 0;
    if (!a) return !need;
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) > steps) return false;
    for (const cJSON *e = a->child; e; e = e->next) {
        if (!cJSON_IsNumber(e) || e->valuedouble != floor(e->valuedouble) || e->valueint < 1 || e->valueint > steps) return false;
        const uint32_t b = 1u << (e->valueint - 1);
        if (*out & b) return false;
        *out |= b;
    }
    return !need || *out;
}

static bool color_of(const cJSON *c, uint32_t *rgb) {
    unsigned v;
    char tail;
    if (!cJSON_IsString(c) || strlen(c->valuestring) != 7 || c->valuestring[0] != '#' || sscanf(c->valuestring + 1, "%6x%c", &v, &tail) != 1) return false;
    *rgb = v;
    return true;
}

static const char *pattern(const cJSON *j, rhy_pattern_t *pt) {
    memset(pt, 0, sizeof(*pt));
    static const char *const KEYS[] = { "id", "label", "steps", "per", "beats", "layers", "lengths", "swing", "bars", "order", "count_in" };
    if (!cJSON_IsObject(j)) return "a rhythm is an object";
    for (const cJSON *c = j->child; c; c = c->next) {
        bool known = false;
        for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) known |= !strcmp(c->string, KEYS[i]);
        if (!known) return "unknown key in a rhythm";
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "id"), *label = cJSON_GetObjectItemCaseSensitive(j, "label");
    if (!cJSON_IsString(id) || !name_ok(id->valuestring, RHY_ID_MAX, false)) return "id: a-z 0-9 -, up to 24";
    if (!cJSON_IsString(label) || !name_ok(label->valuestring, RHY_LABEL_MAX, true)) return "label: capitals, up to 12";
    strcpy(pt->id, id->valuestring); strcpy(pt->label, label->valuestring);
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(j, "steps"), *per = cJSON_GetObjectItemCaseSensitive(j, "per");
    if (!cJSON_IsNumber(st) || st->valuedouble != floor(st->valuedouble) || st->valueint < 2 || st->valueint > RHY_STEPS) return "steps: 2..32";
    pt->steps = (uint8_t)st->valueint;
    if (!cJSON_IsNumber(per) || !(per->valuedouble > 0.0 && per->valuedouble <= 8.0)) return "per: (0..8]";
    pt->per = (float)per->valuedouble;
    const cJSON *len = cJSON_GetObjectItemCaseSensitive(j, "lengths");
    if (len && (!cJSON_IsArray(len) || cJSON_GetArraySize(len) != pt->steps)) return "lengths: one a step";
    for (int s = 0; s < pt->steps; s++) {
        const cJSON *l = len ? cJSON_GetArrayItem(len, s) : NULL;
        if (l && (!cJSON_IsNumber(l) || !(l->valuedouble > 0.0 && l->valuedouble <= 8.0))) return "lengths: (0..8] each";
        pt->start[s + 1] = pt->start[s] + (l ? (float)l->valuedouble : 1.f);
    }
    const cJSON *sw = cJSON_GetObjectItemCaseSensitive(j, "swing");
    if (sw && (!cJSON_IsNumber(sw) || sw->valuedouble < 0.0 || sw->valuedouble > 0.33)) return "swing: 0..0.33";
    pt->swing = sw ? (float)sw->valuedouble : 0.f;
    if (!steps_of(cJSON_GetObjectItemCaseSensitive(j, "beats"), pt->steps, true, &pt->beats)) return "beats: step numbers";
    const cJSON *ls = cJSON_GetObjectItemCaseSensitive(j, "layers");
    if (!cJSON_IsArray(ls) || cJSON_GetArraySize(ls) < 1 || cJSON_GetArraySize(ls) > RHY_LAYERS) return "layers: 1..4";
    for (const cJSON *L = ls->child; L; L = L->next) {
        rhy_layer_t *o = &pt->layer[pt->nlayers++];
        const cJSON *lid = cJSON_GetObjectItemCaseSensitive(L, "id"), *snd = cJSON_GetObjectItemCaseSensitive(L, "sound");
        if (!cJSON_IsObject(L) || cJSON_GetArraySize(L) > 5) return "a layer is {id, sound, color, hits, accent?}";
        if (!cJSON_IsString(lid) || !name_ok(lid->valuestring, RHY_LAYER_ID_MAX, false)) return "layer id: a-z 0-9 -, up to 16";
        for (int q = 0; q + 1 < pt->nlayers; q++) if (!strcmp(pt->layer[q].id, lid->valuestring)) return "layer ids differ";
        strcpy(o->id, lid->valuestring);
        o->sound = RHY_SOUNDS;
        for (int q = 0; cJSON_IsString(snd) && q < RHY_SOUNDS; q++) if (!strcmp(snd->valuestring, RHY_SOUND_NAMES[q])) o->sound = (uint8_t)q;
        if (o->sound == RHY_SOUNDS) return "layer sound: low head rim clap bell hat snare click";
        if (!color_of(cJSON_GetObjectItemCaseSensitive(L, "color"), &o->rgb)) return "layer color: #rrggbb";
        if (!steps_of(cJSON_GetObjectItemCaseSensitive(L, "hits"), pt->steps, true, &o->hits)) return "layer hits: step numbers";
        if (!steps_of(cJSON_GetObjectItemCaseSensitive(L, "accent"), pt->steps, false, &o->accent) || (o->accent & ~o->hits)) return "layer accent: some of its hits";
    }
    const cJSON *bars = cJSON_GetObjectItemCaseSensitive(j, "bars"), *order = cJSON_GetObjectItemCaseSensitive(j, "order");
    if (!bars != !order) return "bars and order go together";
    if (bars) {
        if (!cJSON_IsArray(bars) || cJSON_GetArraySize(bars) < 1 || cJSON_GetArraySize(bars) > RHY_BARS) return "bars: 1..8";
        for (const cJSON *b = bars->child; b; b = b->next, pt->nbars++) {
            const cJSON *h = cJSON_GetObjectItemCaseSensitive(b, "hits"), *nm = cJSON_GetObjectItemCaseSensitive(b, "name");
            if (!cJSON_IsString(nm) || !cJSON_IsArray(h) || cJSON_GetArraySize(h) != pt->nlayers) return "a bar is {name, hits: a list a layer}";
            for (int l = 0; l < pt->nlayers; l++) if (!steps_of(cJSON_GetArrayItem(h, l), pt->steps, false, &pt->bar[pt->nbars][l])) return "bar hits: step numbers";
        }
        if (!cJSON_IsArray(order) || cJSON_GetArraySize(order) < 1 || cJSON_GetArraySize(order) > RHY_ORDER) return "order: 1..16";
        for (const cJSON *o = order->child; o; o = o->next) {
            if (!cJSON_IsNumber(o) || o->valueint < 0 || o->valueint >= pt->nbars || o->valuedouble != o->valueint) return "order: bar indices";
            pt->order[pt->norder++] = (uint8_t)o->valueint;
        }
    }
    const cJSON *cin = cJSON_GetObjectItemCaseSensitive(j, "count_in");
    if (cin && (!cJSON_IsNumber(cin) || cin->valueint < 0 || cin->valueint > 2 || cin->valuedouble != cin->valueint)) return "count_in: 0, 1 or 2";
    pt->count_in = cin ? (uint8_t)cin->valueint : 1;
    return NULL;
}

int rhy_parse_lib(const cJSON *arr, rhy_pattern_t *out, const char **err) {
    *err = NULL;
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) < 1 || cJSON_GetArraySize(arr) > RHY_LIB) { *err = "rhythms: 1..16"; return -1; }
    int n = 0;
    for (const cJSON *j = arr->child; j; j = j->next, n++) {
        if ((*err = pattern(j, &out[n]))) return -1;
        for (int q = 0; q < n; q++) if (!strcmp(out[q].id, out[n].id)) { *err = "rhythm ids differ"; return -1; }
    }
    return n;
}

bool rhy_parse_mute(const rhy_pattern_t *pt, const cJSON *ids, uint32_t *mute) {
    *mute = 0;
    if (!ids) return true;
    if (!cJSON_IsArray(ids)) return false;
    for (const cJSON *e = ids->child; e; e = e->next) {
        const int l = cJSON_IsString(e) ? rhy_layer_of(pt, e->valuestring) : -1;
        if (l < 0) return false;
        *mute |= 1u << l;
    }
    return true;
}

int rhy_layer_of(const rhy_pattern_t *pt, const char *id) {
    for (int l = 0; id && l < pt->nlayers; l++) if (!strcmp(pt->layer[l].id, id)) return l;
    return -1;
}
