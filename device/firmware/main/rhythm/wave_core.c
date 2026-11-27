/* SPDX-License-Identifier: MIT, see wave.h */
#include "wave.h"
#include "rhythm.h"
#include "link_protocol.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define WAVE_CALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#else
#define WAVE_CALLOC(n) calloc(1, (n))
#endif

/* the notes: both spellings, and the one a key prefers (fewer accidentals); a hue a note round the circle of fifths,
 * all twelve equally bright on black (OKLCH 0.8, 0.13) */
static const char *const SHARP[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
static const char *const FLAT[12] = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };
static const char *const IN_KEY[2][12] = { { "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" },
                                           { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "G#", "A", "Bb", "B" } };
static const uint32_t PALETTE[12] = { 0xff9b93u, 0x5dcbffu, 0xe4b750u, 0xc4acffu, 0x86d489u, 0xff99c2u,
                                      0x2ad5e5u, 0xfea668u, 0x95bcffu, 0xbbc75fu, 0xe8a0ecu, 0x49d9b9u };
#define INK      0xc8cce0u              /* the readout's unit, off the chosen note                              */
#define PREVIEW  60                     /* MIDI: the previews and the entry, the middle of the piano (octave 4) */
#define SLOW     1.4f                   /* turns a second: `count` goes down by octaves to under this           */
#define LAND_S   0.4f

typedef struct { char id[WAVE_ID_MAX + 1], label[WAVE_LABEL_MAX + 1]; wav_chord_t ch; uint8_t key; } wave_kind_t;
typedef struct {
    wav_player_t p;
    wave_kind_t lib[WAVE_KINDS];
    int nlib;
    bool on;
    int level, pc, kind;                    /* the level, the chosen note, the chord kind                        */
    float midi;                             /* the root's pitch while it plays (MIDI, fractions allowed)          */
    uint32_t muted;
    uint8_t tuning;
    float orphan, land;
    wave_ev_t ev[8];
    int nev;
} wave_state_t;
static wave_state_t *s_w;                   /* PSRAM, once                                                        */
static wav_tables_t *s_tab;                 /* PSRAM while a chord is loaded: room made by wave_play, freed by wave_orphan */
static bool s_building;                     /* wave_build_run is making them (locked)                             */

static inline int mod12(int m) { return ((m % 12) + 12) % 12; }
static inline const wave_kind_t *kind_of(void) { return &s_w->lib[s_w->kind]; }
static int top_midi(void) { return wav_top_midi(&kind_of()->ch, s_w->tuning); }
static float clamp_midi(float m) { const float top = (float)top_midi(); return m < WAVE_LOW_MIDI ? WAVE_LOW_MIDI : m > top ? top : m; }

void wave_readout(float hz, float midi, int key, char big[32], char unit[32]) {
    if (hz < WAVE_NOTE_HZ) {
        const int tenths = (int)lrintf(hz * 10.f);
        if (hz < 9.95f) snprintf(big, 32, "%d.%d", tenths / 10, tenths % 10);
        else snprintf(big, 32, "%d", (int)lrintf(hz));
        snprintf(unit, 32, "A SECOND");
        return;
    }
    if (hz >= 1000.f) {
        const int c = (int)lrintf(hz / 10.f);               /* hundredths of a kilohertz, one trailing 0 dropped */
        if (c % 10) snprintf(big, 32, "%d.%02dK", c / 100, c % 100);
        else snprintf(big, 32, "%d.%dK", c / 100, c % 100 / 10);
    } else snprintf(big, 32, "%d", (int)lrintf(hz));
    const int m = (int)lrintf(midi);
    snprintf(unit, 32, "HZ %s%d", IN_KEY[key & 1][mod12(m)], (int)floorf(m / 12.f) - 1);
}

/* ── the chord kinds, as wave.play sends them ───────────────────────────────────────────────────────────────────── */

static bool only(const cJSON *o, const char *const *keys, int n) {
    for (const cJSON *c = o ? o->child : NULL; c; c = c->next) {
        bool known = false;
        for (int i = 0; i < n; i++) known |= !strcmp(c->string, keys[i]);
        if (!known) return false;
    }
    return true;
}
static bool name_ok(const char *s, bool label) {
    const size_t n = s ? strlen(s) : 0;
    if (!n || n > (label ? WAVE_LABEL_MAX : WAVE_ID_MAX)) return false;
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        if (label ? !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '#' || c == '-') : !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    return true;
}
static bool whole(const cJSON *j, int lo, int hi) { return cJSON_IsNumber(j) && j->valuedouble == floor(j->valuedouble) && j->valuedouble >= lo && j->valuedouble <= hi; }
static const char *kind_parse(const cJSON *j, wave_kind_t *k) {
    static const char *const KEYS[] = { "id", "label", "semis", "ratio", "spell" };
    memset(k, 0, sizeof(*k));
    if (!cJSON_IsObject(j) || !only(j, KEYS, 5)) return "a chord is {id, label, semis, ratio, spell}";
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "id"), *label = cJSON_GetObjectItemCaseSensitive(j, "label");
    const cJSON *semis = cJSON_GetObjectItemCaseSensitive(j, "semis"), *ratio = cJSON_GetObjectItemCaseSensitive(j, "ratio");
    const cJSON *spell = cJSON_GetObjectItemCaseSensitive(j, "spell");
    if (!cJSON_IsString(id) || !name_ok(id->valuestring, false)) return "id: a-z 0-9 -, up to 24";
    if (!cJSON_IsString(label) || !name_ok(label->valuestring, true)) return "label: capitals, up to 24";
    const int n = cJSON_GetArraySize(semis);
    if (!cJSON_IsArray(semis) || !cJSON_IsArray(ratio) || n < 1 || n > WAV_VOICES || cJSON_GetArraySize(ratio) != n) return "semis and ratio: 1..5 each, as many";
    for (int i = 0; i < n; i++) {
        const cJSON *s = cJSON_GetArrayItem(semis, i), *r = cJSON_GetArrayItem(ratio, i);
        if (!whole(s, 0, 36) || !whole(r, 1, WAV_MAX_N) || (i && (s->valueint <= k->ch.semi[i - 1] || r->valueint <= k->ch.count[i - 1]))) return "semis 0..36 and ratio 1..48, rising";
        k->ch.semi[i] = (int8_t)s->valueint; k->ch.count[i] = (uint8_t)r->valueint;
    }
    if (k->ch.semi[0] != 0) return "semis start at 0 (the root)";
    if (spell && (!cJSON_IsString(spell) || (strcmp(spell->valuestring, "major") && strcmp(spell->valuestring, "minor")))) return "spell: major or minor";
    k->ch.n = n;
    k->key = spell && !strcmp(spell->valuestring, "minor");
    strcpy(k->id, id->valuestring); strcpy(k->label, label->valuestring);
    return NULL;
}

/* ── the levels ─────────────────────────────────────────────────────────────────────────────────────────────────── */

static void push(const wave_ev_t *e) {                   /* (locked) */
    if (s_w->nev == (int)(sizeof(s_w->ev) / sizeof(s_w->ev[0]))) memmove(s_w->ev, s_w->ev + 1, sizeof(s_w->ev[0]) * (size_t)--s_w->nev);
    s_w->ev[s_w->nev++] = *e;
}
static void push_state(const char *state) {             /* (locked) */
    wave_ev_t e = { .kind = WAVE_EV_STATE, .hz = wav_hz(s_w->midi) };
    snprintf(e.state, sizeof e.state, "%s", state);
    snprintf(e.chord, sizeof e.chord, "%s", kind_of()->id);
    snprintf(e.note, sizeof e.note, "%s", IN_KEY[kind_of()->key][s_w->pc]);
    push(&e);
}
static void preview(bool chord) {                        /* (locked) the note, or its chord, plucked once */
    static const wav_chord_t ONE = { 1, { 1 }, { 0 } };
    wav_chord(&s_w->p, chord ? &kind_of()->ch : &ONE, wav_hz(PREVIEW + s_w->pc), WAV_PLUCK, s_w->tuning, true);
}
static void enter(float midi) {                          /* (locked) playing, from its hit */
    s_w->level = WAVE_AT_PLAYING;
    s_w->midi = clamp_midi(midi);
    s_w->muted = 0;
    wav_set_mute(&s_w->p, 0);
    wav_chord(&s_w->p, &kind_of()->ch, wav_hz(s_w->midi), WAV_HOLD, s_w->tuning, true);
}
static void rest(void) {                                 /* (locked) stopped, choosing, on the note it stopped at */
    s_w->level = WAVE_AT_CHORDS;
    s_w->pc = mod12((int)lrintf(s_w->midi));
    wav_silence(&s_w->p);
}
static void pitch(float midi) {                          /* (locked) */
    if (s_w->level != WAVE_AT_PLAYING) { enter(midi); return; }
    s_w->midi = clamp_midi(midi);
    wav_set_root(&s_w->p, wav_hz(s_w->midi));
}
static bool on(void) { return s_w && s_w->on; }

/* {chord, note, midi, hz, tuning, muted, paused} (locked) */
static cJSON *state_json(void) {
    cJSON *o = cJSON_CreateObject(), *m = cJSON_CreateArray();
    cJSON_AddStringToObject(o, "chord", kind_of()->id);
    cJSON_AddStringToObject(o, "note", IN_KEY[kind_of()->key][s_w->pc]);
    cJSON_AddNumberToObject(o, "midi", roundf(s_w->midi * 100.f) / 100.f);
    cJSON_AddNumberToObject(o, "hz", roundf(wav_hz(s_w->midi) * 100.f) / 100.f);
    cJSON_AddStringToObject(o, "tuning", s_w->tuning == WAV_EQUAL ? "equal" : "just");
    for (int i = 0; i < kind_of()->ch.n; i++) if (s_w->muted >> i & 1) cJSON_AddItemToArray(m, cJSON_CreateNumber(i + 1));
    cJSON_AddItemToObject(o, "muted", m);
    cJSON_AddBoolToObject(o, "paused", s_w->level != WAVE_AT_PLAYING);
    return o;
}

/* the pitch an args object names (midi or hz; with `more`, octave or count too, for note pc whose root's whole number is
 * n0), if any: false for a bad one or two of them */
static bool pitch_of(const cJSON *args, bool more, int pc, int n0, bool *have, float *midi) {
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(args, "midi"), *hz = cJSON_GetObjectItemCaseSensitive(args, "hz");
    const cJSON *oct = more ? cJSON_GetObjectItemCaseSensitive(args, "octave") : NULL, *cnt = more ? cJSON_GetObjectItemCaseSensitive(args, "count") : NULL;
    const int given = (m != NULL) + (hz != NULL) + (oct != NULL) + (cnt != NULL);
    *have = given == 1;
    if (!given) return true;
    if (given > 1) return false;
    if (m) { if (!cJSON_IsNumber(m) || !(m->valuedouble >= WAVE_LOW_MIDI && m->valuedouble <= 127.0)) return false; *midi = (float)m->valuedouble; }
    if (hz) { if (!cJSON_IsNumber(hz) || !(hz->valuedouble >= WAV_MIN_HZ && hz->valuedouble <= WAV_MAX_HZ)) return false; *midi = 69.f + 12.f * log2f((float)hz->valuedouble / 440.f); }
    if (oct) { if (!whole(oct, -3, 8)) return false; *midi = (float)(12 * (oct->valueint + 1) + pc); }
    if (cnt) {                                           /* the octave where the clock turns about once a second */
        if (!cJSON_IsTrue(cnt)) return false;
        *midi = (float)(PREVIEW + pc);
        while (wav_hz(*midi) / n0 > SLOW) *midi -= 12.f;
    }
    return true;
}
static bool mute_of(const cJSON *j, int nv, uint32_t *mask) {
    *mask = 0;
    if (!cJSON_IsArray(j)) return false;
    for (const cJSON *e = j->child; e; e = e->next) { if (!whole(e, 1, nv)) return false; *mask |= 1u << (e->valueint - 1); }
    return true;
}
static bool tuning_of(const cJSON *j, uint8_t *t) {
    if (!j) return true;
    if (!cJSON_IsString(j) || (strcmp(j->valuestring, "just") && strcmp(j->valuestring, "equal"))) return false;
    *t = strcmp(j->valuestring, "equal") ? WAV_JUST : WAV_EQUAL;
    return true;
}
static bool ready(void) {
    if (!s_w && (s_w = WAVE_CALLOC(sizeof(wave_state_t)))) wav_init(&s_w->p, NULL);
    return s_w != NULL;
}
/* the tables: room for them now (a few ms), made off this task (wave_build_spawn); false: no memory. (Tables told to quit
 * but not freed yet are taken back, and made on.) */
static bool tables(void) {
    rhythm_lock();
    bool spawn = false;
    if (s_tab) { atomic_store(&s_tab->quit, false); if (!s_building) s_building = spawn = true; }
    const bool have = s_tab != NULL;
    rhythm_unlock();
    if (have) { if (spawn) wave_build_spawn(); return true; }
    wav_tables_t *t = wav_tables_new();
    if (!t) return false;
    rhythm_lock();
    if (!s_tab) { s_tab = t; t = NULL; s_building = spawn = true; }
    rhythm_unlock();
    if (spawn) wave_build_spawn();
    wav_tables_free(t);                                  /* (another got there first) */
    return true;
}
void wave_build_run(void) {
    for (;;) {
        rhythm_lock();
        wav_tables_t *t = s_tab;
        rhythm_unlock();
        while (t && wav_tables_step(t)) {}               /* (only the fluid task frees them, and not while this runs) */
        rhythm_lock();
        const bool more = t && t == s_tab && !atomic_load(&t->quit) && !wav_tables_made(t);   /* (a quit taken back) */
        if (!more) s_building = false;
        rhythm_unlock();
        if (!more) return;
    }
}

const char *wave_play(const cJSON *args, cJSON **out) {
    static const char *const KEYS[] = { "chords", "pick", "note", "midi", "hz", "tuning", "paused", "mute" };
    if (!cJSON_IsObject(args) || !only(args, KEYS, 8)) return "invalid_args";
    static wave_kind_t *lib;                              /* (PSRAM, once: the link task's stack stays small) */
    if (!lib && !(lib = WAVE_CALLOC(sizeof(wave_kind_t) * WAVE_KINDS))) return "oom";
    const cJSON *chords = cJSON_GetObjectItemCaseSensitive(args, "chords"), *pick = cJSON_GetObjectItemCaseSensitive(args, "pick");
    const cJSON *note = cJSON_GetObjectItemCaseSensitive(args, "note"), *paused = cJSON_GetObjectItemCaseSensitive(args, "paused");
    const int n = cJSON_GetArraySize(chords);
    if (!cJSON_IsArray(chords) || n < 1 || n > WAVE_KINDS || !whole(pick, 0, n - 1) || (note && !whole(note, 0, 11)) || (paused && !cJSON_IsBool(paused))) return "invalid_args";
    int i = 0;
    for (const cJSON *c = chords->child; c; c = c->next, i++) {
        if (kind_parse(c, &lib[i])) return "invalid_args";
        for (int q = 0; q < i; q++) if (!strcmp(lib[q].id, lib[i].id)) return "invalid_args";
    }
    uint8_t tuning = WAV_JUST;
    bool have;
    const int pc = note ? note->valueint : 0;
    float midi = (float)(PREVIEW + pc);
    uint32_t mute = 0;
    const cJSON *mj = cJSON_GetObjectItemCaseSensitive(args, "mute");
    if (!tuning_of(cJSON_GetObjectItemCaseSensitive(args, "tuning"), &tuning) || !pitch_of(args, false, pc, 1, &have, &midi) ||
        (mj && !mute_of(mj, lib[pick->valueint].ch.n, &mute)))
        return "invalid_args";
    rhythm_stop();                                       /* (one player on the speaker) */
    if (!tables()) return "oom";
    rhythm_lock();
    if (!ready()) { rhythm_unlock(); return "oom"; }
    memcpy(s_w->lib, lib, sizeof(lib[0]) * (size_t)n);
    s_w->nlib = n; s_w->kind = pick->valueint; s_w->pc = pc;
    s_w->p.t = s_tab;
    s_w->tuning = tuning; s_w->on = true; s_w->orphan = 0.f; s_w->land = 0.f; s_w->midi = clamp_midi(midi);
    if (cJSON_IsTrue(paused)) { s_w->level = WAVE_AT_CHORDS; wav_silence(&s_w->p); preview(true); }
    else {
        enter(midi);
        s_w->muted = mute;
        wav_set_mute(&s_w->p, mute);
    }
    if (out) *out = state_json();
    rhythm_unlock();
    return NULL;
}

const char *wave_set(const cJSON *args, cJSON **out) {
    static const char *const KEYS[] = { "midi", "hz", "octave", "count", "note", "pick", "tuning", "mute", "paused" };
    if (!cJSON_IsObject(args) || !args->child || !only(args, KEYS, 9)) return "invalid_args";
    rhythm_lock();
    if (!on()) { rhythm_unlock(); return "not_playing"; }
    const cJSON *note = cJSON_GetObjectItemCaseSensitive(args, "note"), *pick = cJSON_GetObjectItemCaseSensitive(args, "pick");
    const cJSON *paused = cJSON_GetObjectItemCaseSensitive(args, "paused"), *mj = cJSON_GetObjectItemCaseSensitive(args, "mute");
    uint8_t tuning = s_w->tuning;
    uint32_t mute = 0;
    bool have = false;
    float midi = s_w->midi;
    const int kind = pick && whole(pick, 0, s_w->nlib - 1) ? pick->valueint : s_w->kind, pc = note && whole(note, 0, 11) ? note->valueint : s_w->pc;
    if ((note && !whole(note, 0, 11)) || (pick && !whole(pick, 0, s_w->nlib - 1)) || (paused && !cJSON_IsBool(paused)) ||
        !tuning_of(cJSON_GetObjectItemCaseSensitive(args, "tuning"), &tuning) || (mj && !mute_of(mj, s_w->lib[kind].ch.n, &mute)) ||
        !pitch_of(args, true, pc, s_w->lib[kind].ch.count[0], &have, &midi)) {
        rhythm_unlock();
        return "invalid_args";
    }
    const bool playing = s_w->level == WAVE_AT_PLAYING;
    if (kind != s_w->kind || pc != s_w->pc) {            /* another chord or note: playing, it starts again in its octave */
        const int m = (int)lrintf(s_w->midi);
        s_w->kind = kind; s_w->pc = pc;
        if (s_w->level == WAVE_AT_NOTES) s_w->level = WAVE_AT_CHORDS;
        if (playing) enter((float)(m - mod12(m) + pc));
        else preview(true);
    }
    if (tuning != s_w->tuning) { s_w->tuning = tuning; wav_set_tuning(&s_w->p, tuning); if (s_w->level == WAVE_AT_PLAYING) pitch(s_w->midi); }
    if (have) pitch(midi);
    if (paused && cJSON_IsTrue(paused) && s_w->level == WAVE_AT_PLAYING) rest();
    if (paused && cJSON_IsFalse(paused) && s_w->level != WAVE_AT_PLAYING) enter(s_w->midi);
    if (mj && s_w->level == WAVE_AT_PLAYING) { s_w->muted = mute; wav_set_mute(&s_w->p, mute); }
    if (out) *out = state_json();
    rhythm_unlock();
    return NULL;
}

const char *wave_stop(void) {
    rhythm_lock();
    if (on()) { wav_silence(&s_w->p); s_w->on = false; }
    rhythm_unlock();
    return NULL;
}

/* ── the screen ─────────────────────────────────────────────────────────────────────────────────────────────────── */

bool wave_now(wave_screen_t *o) {
    memset(o, 0, sizeof(*o));
    rhythm_lock();
    if (!on()) { rhythm_unlock(); return false; }
    const wave_kind_t *k = kind_of();
    o->on = true; o->level = s_w->level; o->t = s_w->p.t; o->land = s_w->land;
    o->index = s_w->level == WAVE_AT_NOTES ? s_w->pc : s_w->kind;
    wav_view(&s_w->p, rhythm_heard_seen(), &o->v);
    const int root = s_w->level == WAVE_AT_PLAYING ? mod12((int)lrintf(s_w->midi)) : s_w->pc;
    o->nv = k->ch.n;
    for (int i = 0; i < k->ch.n; i++) { o->count[i] = k->ch.count[i]; o->rgb[i] = PALETTE[mod12(root + k->ch.semi[i])]; }
    if (s_w->level == WAVE_AT_NOTES) {
        if (strcmp(SHARP[s_w->pc], FLAT[s_w->pc])) snprintf(o->name, sizeof o->name, "%s/%s", SHARP[s_w->pc], FLAT[s_w->pc]);
        else snprintf(o->name, sizeof o->name, "%s", SHARP[s_w->pc]);
        o->name_rgb = PALETTE[s_w->pc];
    } else {
        snprintf(o->name, sizeof o->name, "%s", k->label);
        o->name_rgb = s_w->level == WAVE_AT_PLAYING ? INK : 0xffffffu;
        snprintf(o->top, sizeof o->top, "%s", IN_KEY[k->key][s_w->pc]);
        o->top_rgb = PALETTE[s_w->pc];
    }
    if (s_w->level == WAVE_AT_PLAYING) {
        const float f = wav_hz(s_w->midi);                  /* (where it glides to: a glide redraws no letter) */
        wave_readout(f, s_w->midi, k->key, o->big, o->unit);
        o->tone = f <= WAVE_NOTE_HZ ? 0.f : f >= 30.f ? 1.f : (f - WAVE_NOTE_HZ) / (30.f - WAVE_NOTE_HZ);
        o->tone = o->tone * o->tone * (3.f - 2.f * o->tone);
        o->unit_rgb = f >= WAVE_NOTE_HZ && root == s_w->pc ? PALETTE[root] : INK;   /* the chosen note, in any octave: its colour */
    }
    rhythm_unlock();
    return true;
}

bool wave_where(int *level, int *index, float *midi, float *land) {
    rhythm_lock();
    const bool have = on();
    if (have) { *level = s_w->level; *index = s_w->level == WAVE_AT_NOTES ? s_w->pc : s_w->kind; *midi = s_w->midi; *land = s_w->land; }
    rhythm_unlock();
    return have;
}
const char *wave_kind_id(int i, char out[WAVE_ID_MAX + 1]) {
    rhythm_lock();
    snprintf(out, WAVE_ID_MAX + 1, "%s", s_w && i >= 0 && i < s_w->nlib ? s_w->lib[i].id : "");
    rhythm_unlock();
    return out;
}

int wave_labels(char (*out)[WAVE_LABEL_MAX + 1], uint32_t *tints, int max) {
    rhythm_lock();
    int n = 0;
    if (on() && s_w->level == WAVE_AT_NOTES)
        for (; n < 12 && n < max; n++) {
            snprintf(out[n], WAVE_LABEL_MAX + 1, "%s", FLAT[n]);
            if (tints) tints[n] = PALETTE[n];
        }
    else if (on())
        for (; n < s_w->nlib && n < max; n++) { snprintf(out[n], WAVE_LABEL_MAX + 1, "%s", s_w->lib[n].label); if (tints) tints[n] = 0; }
    rhythm_unlock();
    return n;
}

int wave_local_turn(int clicks, bool fast) {
    rhythm_lock();
    int moved = 0;
    if (on() && clicks) {
        if (s_w->level == WAVE_AT_NOTES) { s_w->pc = mod12(s_w->pc + clicks); preview(false); moved = 1; }
        else if (s_w->level == WAVE_AT_CHORDS) { s_w->kind = ((s_w->kind + clicks) % s_w->nlib + s_w->nlib) % s_w->nlib; preview(true); moved = 1; }
        else {
            /* a semitone a click; turning fast, straight to the chosen note in the next octave that way */
            const int c = clicks > 0 ? 1 : -1;
            float m = s_w->midi;
            for (int q = 0; q < abs(clicks); q++) {
                m += (float)c;
                if (fast) while (mod12((int)lrintf(m)) != s_w->pc) m += (float)c;
            }
            m = clamp_midi(m);
            if (m != s_w->midi) {
                moved = mod12((int)lrintf(m)) == s_w->pc && wav_hz(m) >= WAVE_NOTE_HZ ? 2 : 1;
                if (moved == 2) s_w->land = 1.f;
                s_w->midi = m;
                wav_set_root(&s_w->p, wav_hz(m));
            }
        }
    }
    rhythm_unlock();
    return moved;
}
void wave_local_centre(void) {
    rhythm_lock();
    if (on()) {
        if (s_w->level == WAVE_AT_NOTES) { s_w->level = WAVE_AT_CHORDS; preview(true); }
        else if (s_w->level == WAVE_AT_CHORDS) { enter((float)(PREVIEW + s_w->pc)); push_state("playing"); }   /* the pitch just heard */
        else { rest(); push_state("paused"); }
    }
    rhythm_unlock();
}
void wave_local_up(void) {
    rhythm_lock();
    if (on() && s_w->level == WAVE_AT_CHORDS) { s_w->level = WAVE_AT_NOTES; preview(false); }
    rhythm_unlock();
}
void wave_local_mute(int voice, float x, float y) {
    rhythm_lock();
    if (on() && s_w->level == WAVE_AT_PLAYING && voice >= 0 && voice < kind_of()->ch.n) {
        s_w->muted ^= 1u << voice;
        wav_set_mute(&s_w->p, s_w->muted);
        push(&(wave_ev_t){ .kind = WAVE_EV_MUTE, .voice = voice + 1, .on = s_w->muted >> voice & 1, .x = x, .y = y });
    }
    rhythm_unlock();
}
void wave_local_stop(void) {
    rhythm_lock();
    if (on()) { wav_silence(&s_w->p); s_w->on = false; push_state("stopped"); }
    rhythm_unlock();
}
bool wave_active(void) { rhythm_lock(); const bool a = on(); rhythm_unlock(); return a; }

void wave_orphan(bool shown, float dt) {
    wav_tables_t *gone = NULL;
    rhythm_lock();
    if (s_w) {
        s_w->land = fmaxf(0.f, s_w->land - dt / LAND_S);
        s_w->orphan = shown || !s_w->on ? 0.f : s_w->orphan + dt;
        if (s_w->orphan >= WAVE_ORPHAN_S) { wav_silence(&s_w->p); s_w->on = false; push_state("stopped"); s_w->orphan = 0.f; }
        if (!s_w->on && !shown && s_tab && !wav_active(&s_w->p)) {   /* nothing plays them: the maker stops, then they go */
            atomic_store(&s_tab->quit, true);
            if (!s_building) { gone = s_tab; s_tab = NULL; s_w->p.t = NULL; }
        }
    }
    rhythm_unlock();
    wave_build_reap();
    wav_tables_free(gone);
}

bool wave_take_event(wave_ev_t *e) {
    rhythm_lock();
    const bool got = s_w && s_w->nev > 0;
    if (got) { *e = s_w->ev[0]; memmove(s_w->ev, s_w->ev + 1, sizeof(s_w->ev[0]) * (size_t)--s_w->nev); }
    rhythm_unlock();
    return got;
}
cJSON *wave_event_frame(const wave_ev_t *e) {
    if (e->kind == WAVE_EV_MUTE) {
        cJSON *j = link_evt_tap_frame(e->x, e->y), *d = cJSON_GetObjectItemCaseSensitive(j, "data");
        char spot[24];
        snprintf(spot, sizeof spot, "voice-%d", e->voice);
        if (d) { cJSON_AddStringToObject(d, "spot", spot); cJSON_AddBoolToObject(d, "muted", e->on); }
        return j;
    }
    return link_evt_wave_frame(e->state, e->chord, e->note, e->hz);
}

/* ── the speaker (inside the rhythm's lock) ─────────────────────────────────────────────────────────────────────── */

size_t wave_fill_locked(int16_t *out, size_t n) { return s_w ? wav_fill(&s_w->p, out, n) : 0; }
bool wave_sounding_locked(void) { return s_w && wav_active(&s_w->p) && !(s_w->p.yield && s_w->p.duck < 0.003f && !s_w->p.later.on); }
bool wave_wants_locked(void) { return s_w && wav_active(&s_w->p); }
void wave_yield_locked(bool yield) { if (s_w && s_w->p.yield != yield) wav_yield(&s_w->p, yield); }
uint64_t wave_made_locked(void) { return s_w ? s_w->p.n : 0; }
