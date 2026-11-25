/* SPDX-License-Identifier: MIT — see rhythm.h */
#include "rhythm.h"
#include "rhythm_wire.h"
#include "link_protocol.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define RHY_CALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#define RHY_FREE(p)   heap_caps_free(p)
#else
#define RHY_CALLOC(n) calloc(1, (n))
#define RHY_FREE(p)   free(p)
#endif

static rhy_player_t *s_p;                          /* PSRAM, once */
static rhythm_ev_t s_ev[8];
static int s_nev;
static float s_orphan;
static bool s_yielded;                             /* paused for a job on the speaker: back after it */

static bool ready(void) {
    if (!s_p && (s_p = RHY_CALLOC(sizeof(rhy_player_t)))) rhy_init(s_p);
    return s_p != NULL;
}
static void push(const rhythm_ev_t *e) {          /* (locked) */
    if (s_nev == (int)(sizeof(s_ev) / sizeof(s_ev[0]))) memmove(s_ev, s_ev + 1, sizeof(s_ev[0]) * (size_t)--s_nev);
    s_ev[s_nev++] = *e;
}
static void push_state(const char *state) {       /* (locked) */
    rhythm_ev_t e = { .kind = RHY_EV_STATE, .bpm = (int)lrintf(s_p->bpm) };
    strcpy(e.state, state);
    strcpy(e.id, s_p->lib[s_p->cur].id);
    push(&e);
}
static bool on(void) { return s_p && s_p->on && !s_p->quit; }

static const char *only(const cJSON *args, const char *const *keys, int n) {
    if (args && !cJSON_IsObject(args)) return "invalid_args";
    for (const cJSON *c = args ? args->child : NULL; c; c = c->next) {
        bool known = false;
        for (int i = 0; i < n; i++) known |= !strcmp(c->string, keys[i]);
        if (!known) return "invalid_args";
    }
    return NULL;
}
static bool bpm_of(const cJSON *j, float *bpm) {
    if (!j) return true;
    if (!cJSON_IsNumber(j) || !(j->valuedouble >= RHY_BPM_MIN && j->valuedouble <= RHY_BPM_MAX)) return false;
    *bpm = (float)j->valuedouble;
    return true;
}
static bool flag_of(const cJSON *j, int *out) {   /* -1: absent */
    if (!j) { *out = -1; return true; }
    if (!cJSON_IsBool(j)) return false;
    *out = cJSON_IsTrue(j);
    return true;
}

/* {rhythm, bpm, muted, paused, buzz} (locked) */
static cJSON *state_json(void) {
    cJSON *o = cJSON_CreateObject(), *m = cJSON_CreateArray();
    const rhy_pattern_t *pt = &s_p->lib[s_p->cur];
    cJSON_AddStringToObject(o, "rhythm", pt->id);
    cJSON_AddNumberToObject(o, "bpm", lrintf(s_p->bpm));
    for (int l = 0; l < pt->nlayers; l++) if (s_p->mute >> l & 1) cJSON_AddItemToArray(m, cJSON_CreateString(pt->layer[l].id));
    cJSON_AddItemToObject(o, "muted", m);
    cJSON_AddBoolToObject(o, "paused", s_p->paused);
    cJSON_AddBoolToObject(o, "buzz", s_p->buzz);
    return o;
}

const char *rhythm_play(const cJSON *args, cJSON **out) {
    static const char *const KEYS[] = { "rhythms", "pick", "bpm", "mute", "buzz", "paused" };
    const char *e = only(args, KEYS, 6), *why = NULL;
    if (e || !args) return "invalid_args";
    static rhy_pattern_t *lib;
    if (!lib && !(lib = RHY_CALLOC(sizeof(rhy_pattern_t) * RHY_LIB))) return "oom";
    const int n = rhy_parse_lib(cJSON_GetObjectItemCaseSensitive(args, "rhythms"), lib, &why);
    const cJSON *pick = cJSON_GetObjectItemCaseSensitive(args, "pick");
    float bpm = 0.f;
    int buzz, paused;
    uint32_t mute = 0;
    if (n < 1 || !cJSON_IsNumber(pick) || pick->valueint < 0 || pick->valueint >= n || pick->valuedouble != pick->valueint ||
        !cJSON_GetObjectItemCaseSensitive(args, "bpm") || !bpm_of(cJSON_GetObjectItemCaseSensitive(args, "bpm"), &bpm) ||
        !rhy_parse_mute(&lib[pick->valueint], cJSON_GetObjectItemCaseSensitive(args, "mute"), &mute) ||
        !flag_of(cJSON_GetObjectItemCaseSensitive(args, "buzz"), &buzz) || !flag_of(cJSON_GetObjectItemCaseSensitive(args, "paused"), &paused))
        return "invalid_args";
    rhythm_lock();
    if (!ready()) { rhythm_unlock(); return "oom"; }
    rhy_load(s_p, lib, n, pick->valueint, bpm, mute, paused == 1);
    s_p->buzz = buzz == 1;
    s_orphan = 0.f;
    if (out) *out = state_json();
    rhythm_unlock();
    return NULL;
}

const char *rhythm_set(const cJSON *args, cJSON **out) {
    static const char *const KEYS[] = { "bpm", "pick", "mute", "next", "buzz", "paused" };
    if (only(args, KEYS, 6) || !args || !args->child) return "invalid_args";
    rhythm_lock();
    const char *err = NULL;
    if (!on()) err = "not_playing";
    const cJSON *pick = cJSON_GetObjectItemCaseSensitive(args, "pick"), *next = cJSON_GetObjectItemCaseSensitive(args, "next");
    float bpm = 0.f;
    int buzz = -1, paused = -1;
    uint32_t mute = 0;
    if (!err && pick && (!cJSON_IsNumber(pick) || pick->valueint < 0 || pick->valueint >= s_p->nlib || pick->valuedouble != pick->valueint)) err = "invalid_args";
    const rhy_pattern_t *pt = err ? NULL : &s_p->lib[pick ? pick->valueint : s_p->cur];
    const cJSON *mj = cJSON_GetObjectItemCaseSensitive(args, "mute");
    if (!err && (!bpm_of(cJSON_GetObjectItemCaseSensitive(args, "bpm"), &bpm) || (mj && !rhy_parse_mute(pt, mj, &mute)) ||
                 (next && !cJSON_IsTrue(next)) || !flag_of(cJSON_GetObjectItemCaseSensitive(args, "buzz"), &buzz) ||
                 !flag_of(cJSON_GetObjectItemCaseSensitive(args, "paused"), &paused)))
        err = "invalid_args";
    if (!err) {
        if (pick && pick->valueint != s_p->cur) rhy_pick(s_p, pick->valueint);
        if (bpm > 0.f) rhy_set_bpm(s_p, bpm);
        if (mj) rhy_set_mute(s_p, mute);
        if (next) rhy_next(s_p);
        if (buzz >= 0) s_p->buzz = buzz;
        if (paused >= 0) rhy_pause(s_p, paused);
        if (out) *out = state_json();
    }
    rhythm_unlock();
    return err;
}

const char *rhythm_stop(void) {
    rhythm_lock();
    if (s_p) rhy_stop(s_p);
    rhythm_unlock();
    return NULL;
}

bool rhythm_now(rhy_view_t *v, rhy_pattern_t *pt) {
    rhythm_lock();
    const bool have = s_p && s_p->nlib && s_p->on;
    if (have) {
        rhy_view(s_p, rhythm_heard(), v);
        if (pt) *pt = s_p->lib[s_p->cur];
    } else memset(v, 0, sizeof(*v));
    rhythm_unlock();
    return have;
}

int rhythm_labels(char (*out)[RHY_LABEL_MAX + 1], int max) {
    rhythm_lock();
    const int n = s_p ? (s_p->nlib < max ? s_p->nlib : max) : 0;
    for (int i = 0; i < n; i++) strcpy(out[i], s_p->lib[i].label);
    rhythm_unlock();
    return n;
}

const char *rhythm_id(int i, char out[RHY_ID_MAX + 1]) {
    rhythm_lock();
    strcpy(out, s_p && i >= 0 && i < s_p->nlib ? s_p->lib[i].id : "");
    rhythm_unlock();
    return out;
}

void rhythm_local_mute(int layer, float x, float y) {
    rhythm_lock();
    if (on() && layer >= 0 && layer < s_p->lib[s_p->cur].nlayers) {
        rhy_set_mute(s_p, s_p->mute ^ 1u << layer);
        rhythm_ev_t e = { .kind = RHY_EV_MUTE, .on = s_p->mute >> layer & 1, .x = x, .y = y, .bpm = (int)lrintf(s_p->bpm) };
        strcpy(e.id, s_p->lib[s_p->cur].layer[layer].id);
        push(&e);
    }
    rhythm_unlock();
}
void rhythm_local_bpm(float bpm) { rhythm_lock(); if (on()) rhy_set_bpm(s_p, bpm); rhythm_unlock(); }
void rhythm_local_pick(int i) { rhythm_lock(); if (on() && i != s_p->cur) rhy_pick(s_p, i); rhythm_unlock(); }
void rhythm_local_pause(void) {
    rhythm_lock();
    s_yielded = false;
    if (on()) { rhy_pause(s_p, !s_p->paused); push_state(s_p->paused ? "paused" : "playing"); }
    rhythm_unlock();
}
void rhythm_local_stop(void) {
    rhythm_lock();
    if (on()) { rhy_stop(s_p); push_state("stopped"); }
    rhythm_unlock();
}
bool rhythm_active(void) { rhythm_lock(); const bool a = on(); rhythm_unlock(); return a; }

void rhythm_orphan(bool shown, float dt) {
    rhythm_lock();
    s_orphan = shown || !on() ? 0.f : s_orphan + dt;
    if (s_orphan >= RHYTHM_ORPHAN_S) { rhy_stop(s_p); push_state("stopped"); s_orphan = 0.f; }
    rhythm_unlock();
}

bool rhythm_take_event(rhythm_ev_t *e) {
    rhythm_lock();
    const bool got = s_nev > 0;
    if (got) { *e = s_ev[0]; memmove(s_ev, s_ev + 1, sizeof(s_ev[0]) * (size_t)--s_nev); }
    rhythm_unlock();
    return got;
}

cJSON *rhythm_event_frame(const rhythm_ev_t *e) {
    if (e->kind == RHY_EV_MUTE) {
        cJSON *j = link_evt_tap_frame(e->x, e->y), *d = cJSON_GetObjectItemCaseSensitive(j, "data");
        if (d) { cJSON_AddStringToObject(d, "spot", e->id); cJSON_AddBoolToObject(d, "muted", e->on); }
        return j;
    }
    return link_evt_rhythm_frame(e->state, e->id, e->bpm);
}

size_t rhythm_fill(int16_t *out, size_t n) {
    rhythm_lock();
    const size_t got = s_p ? rhy_fill(s_p, out, n) : 0;
    if (!s_p) memset(out, 0, n * sizeof(int16_t));
    rhythm_unlock();
    return got;
}
void rhythm_yield(bool yield) {
    rhythm_lock();
    if (yield && s_p && rhy_sounding(s_p) && !s_p->paused) { rhy_pause(s_p, true); s_yielded = true; }
    else if (!yield && s_yielded) { s_yielded = false; if (on() && s_p->paused) rhy_pause(s_p, false); }
    rhythm_unlock();
}
bool rhythm_wants_speaker(void) {
    rhythm_lock();
    const bool w = s_p && (rhy_sounding(s_p) || (s_yielded && on()));
    rhythm_unlock();
    return w;
}
bool rhythm_sounding(void) { rhythm_lock(); const bool s = s_p && rhy_sounding(s_p); rhythm_unlock(); return s; }
bool rhythm_take_buzz(uint64_t upto, bool *first) {
    rhythm_lock();
    bool got = false;
    while (s_p && rhy_take_beat(s_p, upto, first)) if (s_p->buzz) { got = true; break; }
    rhythm_unlock();
    return got;
}
uint64_t rhythm_made(void) { rhythm_lock(); const uint64_t n = s_p ? s_p->n : 0; rhythm_unlock(); return n; }
