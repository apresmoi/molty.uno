#include "link_protocol.h"
#include "cute_act.h"   /* the actions: act, say marks */
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <string.h>
#include "cJSON.h"

bool link_code_char(uint8_t random, char *out)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ23456789";
    if (!out || random >= 238) return false; /* 7 * 34 */
    *out = alphabet[random % 34];
    return true;
}

void link_code_generate(char out[9], uint32_t (*random)(void *), void *ctx)
{
    for (int i = 0; i < 8;) {
        if (link_code_char((uint8_t)random(ctx), &out[i])) i++;
    }
    out[8] = 0;
}

void link_id_generate(char out[LINK_ID_LEN + 1], uint32_t (*random)(void *), void *ctx)
{
    memcpy(out, "molty-", 6);
    for (int i = 6; i < LINK_ID_LEN;) {
        char c;
        if (!link_code_char((uint8_t)random(ctx), &c)) continue;
        out[i++] = c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
    }
    out[LINK_ID_LEN] = 0;
}

bool link_id_valid(const char *id)
{
    return id && strlen(id) == LINK_ID_LEN && !strncmp(id, "molty-", 6)
        && strspn(id + 6, "abcdefghijklmnopqrstuvwxyz23456789") == 10;
}

bool link_url_valid(const char *url)
{
    if (!url || strlen(url) >= LINK_URL_MAX) return false;
    const char *host = !strncmp(url, "wss://", 6) ? url + 6
                     : !strncmp(url, "ws://", 5) ? url + 5 : NULL;
    return host && host[0] && !strpbrk(host, "/?#@ \t\r\n");
}

unsigned link_retry_seconds(unsigned close_code, unsigned backoff)
{
    return close_code == LINK_CLOSE_AUTH_FAILED ? 300 : backoff;
}

static cJSON *frame(const char *type)
{
    cJSON *j = cJSON_CreateObject();
    if (j) cJSON_AddStringToObject(j, "t", type);
    return j;
}

cJSON *link_code_frame(const char *code)
{
    cJSON *j = frame("link_code");
    if (j) {
        cJSON_AddStringToObject(j, "code", code);
        /* Built relay room.ts reads link_code; retain its spelling as an alias. */
        cJSON_AddStringToObject(j, "link_code", code);
    }
    return j;
}

cJSON *link_pair_frame(const char *req, bool ok, const char *reason)
{
    cJSON *j = frame("pair.result");
    if (j) {
        cJSON_AddStringToObject(j, "req", req);
        cJSON_AddBoolToObject(j, "ok", ok);
        if (!ok) cJSON_AddStringToObject(j, "reason", reason);
    }
    return j;
}

cJSON *link_say_done_frame(const char *job)
{
    cJSON *j = frame("evt");
    if (j) {
        cJSON_AddStringToObject(j, "name", "say.done");
        cJSON *data = cJSON_AddObjectToObject(j, "data");
        cJSON_AddStringToObject(data, "job", job);
    }
    return j;
}

cJSON *link_forget_frame(void) { return frame("forget"); }

bool link_mood_map(const char *mood, const char **state)
{
    /* Every mood is a mood of the .molty creature (fluid "creature" state, which
     * falls back to the blob when no creature is loaded); names match CUTE_MOOD_NAMES. */
    static const struct { const char *mood, *state; } map[] = {
        {"idle", "creature"}, {"listening", "creature"}, {"thinking", "creature"},
        {"speaking", "creature"}, {"happy", "creature"}, {"sleepy", "creature"},
        {"surprised", "creature"},
    };
    if (!mood || !state) return false;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (!strcmp(mood, map[i].mood)) { *state = map[i].state; return true; }
    }
    return false;
}

/* the mood's index in the creature's order (cute_spec.h CUTE_MOOD_NAMES / link_mood_map) */
const char *link_mood_name(int id);
static int mood_index(const char *m)
{
    for (int i = 0; link_mood_name(i)[0]; i++) if (!strcmp(m, link_mood_name(i))) return i;
    return -1;
}

const char *link_mood_name(int id)
{
    static const char *const order[] = { "idle", "listening", "thinking", "speaking", "happy", "sleepy", "surprised" };
    return id >= 0 && id < (int)(sizeof(order) / sizeof(order[0])) ? order[id] : "";
}

bool link_marks_parse(const cJSON *args, link_mark_t marks[LINK_MARKS_MAX], int *n)
{
    if (!n || !marks) return false;
    *n = 0;
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(args, "marks");
    if (!m) return true;
    if (!cJSON_IsArray(m) || cJSON_GetArraySize(m) > LINK_MARKS_MAX) return false;
    const cJSON *e;
    cJSON_ArrayForEach(e, m) {
        const cJSON *w = cJSON_GetArrayItem(e, 0), *name = cJSON_GetArrayItem(e, 1);
        if (!cJSON_IsArray(e) || cJSON_GetArraySize(e) != 2 || !cJSON_IsNumber(w) || !cJSON_IsString(name) ||
            !(w->valuedouble >= 0 && w->valuedouble <= LINK_MARK_WORD_MAX) || w->valuedouble != floor(w->valuedouble))
            { *n = 0; return false; }
        int mood = mood_index(name->valuestring), act = cute_act_lookup(name->valuestring);
        if (mood < 0 && act < 0) { *n = 0; return false; }
        marks[*n] = (link_mark_t){ .word = (uint16_t)w->valuedouble, .act = mood < 0, .id = (int8_t)(mood < 0 ? act : mood) };
        (*n)++;
    }
    return true;
}

bool link_act_args(const cJSON *args, int *act)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(args, "action");
    if (!act || !cJSON_IsString(a)) return false;
    *act = cute_act_lookup(a->valuestring);
    return *act >= 0;
}

bool link_mood_args(const cJSON *args, const char **mood, double *hold_s)
{
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(args, "mood");
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(args, "hold_s");
    const char *state;
    if (!mood || !hold_s || !cJSON_IsString(m) || !link_mood_map(m->valuestring, &state)) return false;
    if (h && (!cJSON_IsNumber(h) || !(h->valuedouble >= 0 && h->valuedouble <= 3600))) return false;
    *mood = m->valuestring;
    *hold_s = h ? h->valuedouble : -1;
    return true;
}

static bool ms_arg(const cJSON *args, const char *key, unsigned lo, unsigned hi, unsigned *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, key);
    if (!v) return true;
    if (!cJSON_IsNumber(v) || !(v->valuedouble >= lo && v->valuedouble <= hi) ||
        v->valuedouble != floor(v->valuedouble)) return false;
    *out = (unsigned)v->valuedouble;
    return true;
}

bool link_listen_args(const cJSON *args, bool *vad, unsigned *end_silence_ms, unsigned *no_speech_ms)
{
    if (!vad || !end_silence_ms || !no_speech_ms) return false;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, "vad");
    if (v && !cJSON_IsBool(v)) return false;
    unsigned es = *end_silence_ms, ns = *no_speech_ms;
    if (!ms_arg(args, "end_silence_ms", 300, 3000, &es) || !ms_arg(args, "no_speech_ms", 1000, 10000, &ns))
        return false;
    if (v) *vad = cJSON_IsTrue(v);
    *end_silence_ms = es; *no_speech_ms = ns;
    return true;
}

bool link_listen_setting_ok(double v)
{
    return v == floor(v) && (v == LINK_LISTEN_UNLIMITED || (v >= LINK_LISTEN_SET_MIN && v <= LINK_LISTEN_MAX_MS));
}

bool link_listen_window(const cJSON *args, uint32_t dflt, uint32_t *window, uint32_t *record_ms)
{
    if (!window || !record_ms) return false;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, "ms");
    uint32_t w;
    if (!v) w = link_listen_setting_ok(dflt) ? dflt : LINK_LISTEN_DEFAULT_MS;
    else if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0 && v->valuedouble <= LINK_LISTEN_MAX_MS) ||
             v->valuedouble != floor(v->valuedouble)) return false;
    else w = (uint32_t)v->valuedouble;
    *window = w;
    *record_ms = w == LINK_LISTEN_UNLIMITED ? LINK_LISTEN_CEILING_MS : w;
    return true;
}

static const char *const ACK_NAMES[] = {"off", "thinking", "chirp"};

const char *link_ack_name(link_ack_t mode)
{
    return mode >= LINK_ACK_OFF && mode <= LINK_ACK_CHIRP ? ACK_NAMES[mode] : ACK_NAMES[LINK_ACK_DEFAULT];
}

bool link_ack_parse(const char *name, link_ack_t *mode)
{
    if (!name || !mode) return false;
    for (int i = LINK_ACK_OFF; i <= LINK_ACK_CHIRP; i++)
        if (!strcmp(name, ACK_NAMES[i])) { *mode = (link_ack_t)i; return true; }
    return false;
}

bool link_ack_arg(const cJSON *args, link_ack_t setting, link_ack_t *mode)
{
    if (!mode) return false;
    if (setting < LINK_ACK_OFF || setting > LINK_ACK_CHIRP) setting = LINK_ACK_DEFAULT;
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(args, "ack");
    if (!a || cJSON_IsTrue(a)) *mode = setting;
    else if (cJSON_IsFalse(a)) *mode = LINK_ACK_OFF;
    else if (cJSON_IsString(a) && !strcmp(a->valuestring, "silent"))
        *mode = setting == LINK_ACK_OFF ? LINK_ACK_OFF : LINK_ACK_THINKING;
    else return false;
    return true;
}

link_ack_t link_ack_decide(link_ack_t mode, const char *reason, bool speech_known, uint32_t speech_ms,
                           bool screen_taken, bool has_speaker)
{
    if (mode <= LINK_ACK_OFF || mode > LINK_ACK_CHIRP || screen_taken || !reason) return LINK_ACK_OFF;
    bool heard = !strcmp(reason, "turn_end") ||
                 (!strcmp(reason, "silence") && (!speech_known || speech_ms > 0)) ||
                 (!strcmp(reason, "max") && speech_known && speech_ms >= LINK_ACK_MIN_SPEECH_MS);
    if (!heard) return LINK_ACK_OFF;
    return mode == LINK_ACK_CHIRP && !has_speaker ? LINK_ACK_THINKING : mode;
}

static cJSON *evt(const char *name, cJSON **data)
{
    cJSON *j = frame("evt");
    if (!j) return NULL;
    cJSON_AddStringToObject(j, "name", name);
    if (!(*data = cJSON_AddObjectToObject(j, "data"))) { cJSON_Delete(j); return NULL; }
    return j;
}

static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

cJSON *link_evt_shake_frame(double peak_g, int reversals, unsigned ms, const char *listen)
{
    cJSON *data, *j = evt("shake", &data);
    if (j) {
        cJSON_AddNumberToObject(data, "peak_g", round(peak_g * 100) / 100);
        cJSON_AddNumberToObject(data, "reversals", reversals);
        cJSON_AddNumberToObject(data, "ms", ms);
        if (listen) cJSON_AddStringToObject(data, "listen", listen);
    }
    return j;
}

bool link_welcome_has(const cJSON *welcome, const char *name)
{
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(welcome, "features"), *it;
    if (!name || !cJSON_IsArray(f)) return false;
    cJSON_ArrayForEach(it, f) if (cJSON_IsString(it) && !strcmp(it->valuestring, name)) return true;
    return false;
}

cJSON *link_evt_tap_frame(double x, double y)
{
    cJSON *data, *j = evt("tap", &data);
    if (j) {
        cJSON_AddNumberToObject(data, "x", round(clamp01(x) * 1000) / 1000);
        cJSON_AddNumberToObject(data, "y", round(clamp01(y) * 1000) / 1000);
    }
    return j;
}

cJSON *link_evt_button_frame(bool accept)
{
    cJSON *data, *j = evt("button", &data);
    if (j) cJSON_AddStringToObject(data, "id", accept ? "accept" : "reject");
    return j;
}

char *link_restore_body(const char *saved_state_json)
{
    cJSON *saved = saved_state_json ? cJSON_Parse(saved_state_json) : NULL;
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(saved, "state");
    const char *name = cJSON_IsString(st) ? st->valuestring : "creature";
    if (!strcmp(name, "text") || !strcmp(name, "autoplay") ||
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(saved, "autoplay"))) name = "creature";
    cJSON *j = cJSON_CreateObject();
    char *body = NULL;
    if (j && cJSON_AddStringToObject(j, "state", name)) body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j); cJSON_Delete(saved);
    return body;
}

static bool valid_stream(const char *s)
{
    return s && s[0] && strlen(s) < LINK_STREAM_MAX;
}

bool link_frame_parse(const void *data, size_t len, link_frame_t *h,
                      const uint8_t **pcm, size_t *pcm_len)
{
    if (!data || !h || !pcm || !pcm_len || len > LINK_MESSAGE_MAX) return false;
    const uint8_t *p = data;
    const uint8_t *nl = memchr(p, '\n', len);
    size_t n = nl ? (size_t)(nl - p) : len, payload = nl ? len - n - 1 : 0;
    if (!n || n > LINK_HEADER_MAX) return false;
    if (payload > LINK_PCM_MAX || (payload & 1) || memchr(p, 0, n)) return false;
    char json[LINK_HEADER_MAX + 1];
    memcpy(json, p, n); json[n] = 0;
    cJSON *j = cJSON_ParseWithOpts(json, NULL, true);
    if (!j) return false;
    cJSON *s = cJSON_GetObjectItemCaseSensitive(j, "s");
    cJSON *seq = cJSON_GetObjectItemCaseSensitive(j, "seq");
    cJSON *kind = cJSON_GetObjectItemCaseSensitive(j, "kind");
    cJSON *rate = cJSON_GetObjectItemCaseSensitive(j, "rate");
    cJSON *end = cJSON_GetObjectItemCaseSensitive(j, "end");
    cJSON *w = cJSON_GetObjectItemCaseSensitive(j, "w");
    bool ok = cJSON_IsObject(j) && cJSON_IsString(s) && valid_stream(s->valuestring)
        && cJSON_IsNumber(seq) && isfinite(seq->valuedouble) && seq->valuedouble >= 0
        && seq->valuedouble <= UINT32_MAX && floor(seq->valuedouble) == seq->valuedouble
        && cJSON_IsString(kind) && !strcmp(kind->valuestring, "pcm16")
        && cJSON_IsNumber(rate) && rate->valuedouble == 16000 && (!end || cJSON_IsBool(end))
        && (!w || (cJSON_IsArray(w) && cJSON_GetArraySize(w) <= LINK_CUES_MAX));
    h->ncues = 0;
    if (ok && w) {                       /* word cues: [[word, sample], ...] */
        const cJSON *c;
        cJSON_ArrayForEach(c, w) {
            const cJSON *a = cJSON_GetArrayItem(c, 0), *b = cJSON_GetArrayItem(c, 1);
            if (!cJSON_IsArray(c) || cJSON_GetArraySize(c) != 2 || !cJSON_IsNumber(a) || !cJSON_IsNumber(b)
                || !(a->valuedouble >= 0 && a->valuedouble < LINK_CUE_WORDS) || floor(a->valuedouble) != a->valuedouble
                || !(b->valuedouble >= 0 && b->valuedouble <= UINT32_MAX) || floor(b->valuedouble) != b->valuedouble) { ok = false; break; }
            h->cue_word[h->ncues] = (uint16_t)a->valuedouble; h->cue_sample[h->ncues] = (uint32_t)b->valuedouble; h->ncues++;
        }
        if (!ok) h->ncues = 0;
    }
    if (ok) {
        strcpy(h->stream, s->valuestring); h->seq = (uint32_t)seq->valuedouble;
        h->end = cJSON_IsTrue(end); h->reason = NULL; h->speech_ms = 0; h->speech_known = false; *pcm = nl ? nl + 1 : p + len; *pcm_len = payload;
    }
    cJSON_Delete(j);
    return ok;
}

int link_frame_header(char *out, size_t cap, const link_frame_t *h)
{
    if (!out || !h || !valid_stream(h->stream)) return -1;
    cJSON *j = cJSON_CreateObject();
    if (!j) return -1;
    cJSON_AddStringToObject(j, "s", h->stream);
    cJSON_AddNumberToObject(j, "seq", h->seq);
    cJSON_AddStringToObject(j, "kind", "pcm16");
    cJSON_AddNumberToObject(j, "rate", 16000);
    cJSON_AddBoolToObject(j, "end", h->end);
    if (h->reason) {
        cJSON_AddStringToObject(j, "reason", h->reason);
        if (h->speech_known) cJSON_AddNumberToObject(j, "speech_ms", h->speech_ms);
    }
    char *json = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!json) return -1;
    size_t n = strlen(json);
    if (n > LINK_HEADER_MAX || cap < n + 2) { cJSON_free(json); return -1; }
    memcpy(out, json, n); out[n] = '\n'; out[n + 1] = 0;
    cJSON_free(json);
    return (int)n + 1;
}

bool link_color_parse(const char *s, uint32_t *rgb)
{
    if (!s || s[0] != '#' || strlen(s) != 7) return false;
    uint32_t v = 0;
    for (int i = 1; i < 7; i++) {
        int c = (unsigned char)s[i], d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                       : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = v << 4 | (uint32_t)d;
    }
    *rgb = v;
    return true;
}

bool link_color_arg(const cJSON *args, const char *key, int32_t *rgb)
{
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(args, key);
    uint32_t v;
    if (!c) { *rgb = -1; return true; }
    if (!cJSON_IsString(c) || !link_color_parse(c->valuestring, &v)) return false;
    *rgb = (int32_t)v;
    return true;
}

bool link_ask_args(const cJSON *args, char job[LINK_STREAM_MAX], char labels[LINK_ASK_MAX][LINK_ASK_LABEL_MAX + 1],
                   int32_t colors[LINK_ASK_MAX], int *n, unsigned *timeout_ms)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(args, "job");
    const cJSON *opts = cJSON_GetObjectItemCaseSensitive(args, "options");
    const cJSON *to = cJSON_GetObjectItemCaseSensitive(args, "timeout_ms");
    if (!cJSON_IsString(j) || !valid_stream(j->valuestring)) return false;
    if (!cJSON_IsNumber(to) || !isfinite(to->valuedouble) || floor(to->valuedouble) != to->valuedouble ||
        to->valuedouble < LINK_ASK_TIMEOUT_MIN || to->valuedouble > LINK_ASK_TIMEOUT_MAX) return false;
    int count = cJSON_IsArray(opts) ? cJSON_GetArraySize(opts) : 0;
    if (count < 1 || count > LINK_ASK_MAX) return false;
    for (int i = 0; i < count; i++) {
        const cJSON *o = cJSON_GetArrayItem(opts, i);
        if (!cJSON_IsString(o)) return false;
        const char *v = o->valuestring;
        size_t len = strlen(v);
        if (len < 1 || len > LINK_ASK_LABEL_MAX) return false;
        bool ink = false;
        for (size_t k = 0; k < len; k++) {
            unsigned char c = (unsigned char)v[k];
            if (c < 0x20 || c > 0x7E) return false;
            if (c != ' ') ink = true;
        }
        if (!ink) return false;
        for (int p = 0; p < i; p++) {                 /* unique, ignoring case */
            const char *a = labels[p];
            size_t q = 0;
            while (a[q] && v[q] && tolower((unsigned char)a[q]) == tolower((unsigned char)v[q])) q++;
            if (!a[q] && !v[q]) return false;
        }
        memcpy(labels[i], v, len + 1);
    }
    const cJSON *cols = cJSON_GetObjectItemCaseSensitive(args, "colors");
    for (int i = 0; i < LINK_ASK_MAX; i++) colors[i] = -1;
    if (cols) {
        if (!cJSON_IsArray(cols) || cJSON_GetArraySize(cols) != count) return false;
        for (int i = 0; i < count; i++) {
            const cJSON *c = cJSON_GetArrayItem(cols, i);
            uint32_t v;
            if (!cJSON_IsString(c) || !link_color_parse(c->valuestring, &v)) return false;
            colors[i] = (int32_t)v;
        }
    }
    strcpy(job, j->valuestring);
    *n = count; *timeout_ms = (unsigned)to->valuedouble;
    return true;
}

cJSON *link_evt_answer_frame(const char *job, int index, const char *label, const char *reason)
{
    cJSON *data, *j = evt("answer", &data);
    if (!j) return NULL;
    cJSON_AddStringToObject(data, "job", job ? job : "");
    if (index >= 0) {
        cJSON_AddNumberToObject(data, "index", index);
        cJSON_AddStringToObject(data, "label", label ? label : "");
    } else {
        cJSON_AddNullToObject(data, "index");
        cJSON_AddStringToObject(data, "reason", reason ? reason : "cancelled");
    }
    return j;
}
