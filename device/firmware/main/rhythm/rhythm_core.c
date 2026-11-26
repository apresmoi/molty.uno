/* SPDX-License-Identifier: MIT, see rhythm.h */
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
static rhy_region_t s_regions[RHY_REGIONS];       /* the library's, as it came (rhythm.play)          */
static int s_nregions;
static int32_t s_ahead_us;                         /* the picture's delay (rhythm_view_ahead)        */
static int s_sync_ms;                              /* the human's trim                                */
static bool s_sync_new;                            /* ... changed: kept once the lock is let go       */
void rhythm_view_ahead(int32_t us) { rhythm_lock(); s_ahead_us = us; rhythm_unlock(); }
void rhythm_sync_init(int ms) { s_sync_ms = ms < -RHYTHM_SYNC_MAX ? -RHYTHM_SYNC_MAX : ms > RHYTHM_SYNC_MAX ? RHYTHM_SYNC_MAX : ms; }
int rhythm_sync(void) { return s_sync_ms; }
/* after the lock: a new trim kept (a flash write: never under the lock, the speaker's fill takes it) */
static void sync_keep(void) {
    rhythm_lock();
    const bool keep = s_sync_new;
    s_sync_new = false;
    const int ms = s_sync_ms;
    rhythm_unlock();
    if (keep) rhythm_sync_saved(ms);
}
/* {sync: ms}: a new trim (locked); false: not a whole number of ms in range */
static bool sync_of(const cJSON *j) {
    if (!j) return true;
    if (!cJSON_IsNumber(j) || j->valuedouble != (int)j->valuedouble || j->valueint < -RHYTHM_SYNC_MAX || j->valueint > RHYTHM_SYNC_MAX) return false;
    if (j->valueint != s_sync_ms) { s_sync_ms = j->valueint; s_sync_new = true; }
    return true;
}

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
/* playing, or at rest (locked): a yield for the speaker is neither (s_yielded) */
static void rest(bool r) {
    s_yielded = false;
    if (r) rhy_rest(s_p);
    else rhy_pause(s_p, false);
}

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
    cJSON_AddNumberToObject(o, "sync", s_sync_ms);
    return o;
}

const char *rhythm_play(const cJSON *args, cJSON **out) {
    static const char *const KEYS[] = { "rhythms", "regions", "pick", "bpm", "mute", "buzz", "paused", "sync" };
    const char *e = only(args, KEYS, 8), *why = NULL;
    if (e || !args) return "invalid_args";
    static rhy_pattern_t *lib;
    if (!lib && !(lib = RHY_CALLOC(sizeof(rhy_pattern_t) * RHY_LIB))) return "oom";
    static rhy_region_t regions[RHY_REGIONS];          /* (a static: the link task's stack stays small) */
    const int nr = rhy_parse_regions(cJSON_GetObjectItemCaseSensitive(args, "regions"), regions, &why);
    const int n = nr < 0 ? -1 : rhy_parse_lib(cJSON_GetObjectItemCaseSensitive(args, "rhythms"), nr, lib, &why);
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
    if (!sync_of(cJSON_GetObjectItemCaseSensitive(args, "sync"))) { rhythm_unlock(); return "invalid_args"; }
    rhy_load(s_p, lib, n, pick->valueint, bpm, mute, paused == 1);
    memcpy(s_regions, regions, sizeof(regions[0]) * (size_t)nr); s_nregions = nr;
    s_p->buzz = buzz == 1;
    s_orphan = 0.f;
    if (out) *out = state_json();
    rhythm_unlock();
    sync_keep();
    return NULL;
}

const char *rhythm_set(const cJSON *args, cJSON **out) {
    static const char *const KEYS[] = { "bpm", "pick", "mute", "next", "buzz", "paused", "sync" };
    if (only(args, KEYS, 7) || !args || !args->child) return "invalid_args";
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
                 !flag_of(cJSON_GetObjectItemCaseSensitive(args, "paused"), &paused) || !sync_of(cJSON_GetObjectItemCaseSensitive(args, "sync"))))
        err = "invalid_args";
    if (!err) {
        if (pick && pick->valueint != s_p->cur) rhy_pick(s_p, pick->valueint);
        if (bpm > 0.f) rhy_set_bpm(s_p, bpm);
        if (mj) rhy_set_mute(s_p, mute);
        if (next) rhy_next(s_p);
        if (buzz >= 0) s_p->buzz = buzz;
        if (paused >= 0 && paused != (s_p->paused && !s_yielded)) rest(paused);
        if (out) *out = state_json();
    }
    rhythm_unlock();
    sync_keep();
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
        const int64_t ahead = (int64_t)s_ahead_us + (int64_t)s_sync_ms * 1000;
        rhy_view(s_p, rhythm_heard((int32_t)ahead), v);
        v->rest = s_p->paused && !s_yielded;
        if (pt) *pt = s_p->lib[s_p->cur];
    } else memset(v, 0, sizeof(*v));
    rhythm_unlock();
    return have;
}

int rhythm_labels(int region, char (*out)[RHYTHM_LABEL_ROOM], int *index, int max) {
    rhythm_lock();
    int n = 0;
    for (int i = 0; s_p && i < s_p->nlib && n < max; i++) {
        if (region >= 0 && s_p->lib[i].region != region) continue;
        strcpy(out[n], s_p->lib[i].label);
        if (index) index[n] = i;
        n++;
    }
    rhythm_unlock();
    return n;
}
int rhythm_regions(char (*label)[RHYTHM_LABEL_ROOM], uint32_t *rgb, int *count, int max) {
    rhythm_lock();
    const int n = s_nregions < max ? s_nregions : max;
    for (int r = 0; r < n; r++) {
        strcpy(label[r], s_regions[r].label);
        if (rgb) rgb[r] = s_regions[r].rgb;
        if (count) { count[r] = 0; for (int i = 0; s_p && i < s_p->nlib; i++) count[r] += s_p->lib[i].region == r; }
    }
    rhythm_unlock();
    return n;
}
int rhythm_region_of(int i) {
    rhythm_lock();
    const int r = s_p && i >= 0 && i < s_p->nlib ? s_p->lib[i].region : 0;
    rhythm_unlock();
    return r;
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
void rhythm_local_play(void) {
    rhythm_lock();
    if (on()) { const bool play = s_p->paused && !s_yielded; rest(!play); push_state(play ? "playing" : "paused"); }
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
