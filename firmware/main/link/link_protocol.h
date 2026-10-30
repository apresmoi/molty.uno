#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#define LINK_ID_LEN 16
#define LINK_URL_MAX 384
#define LINK_CLOSE_MALFORMED 4400
#define LINK_CLOSE_AUTH_FAILED 4401
#define LINK_CLOSE_REPLACED 4409
#define LINK_CLOSE_FORGOTTEN 4410
#define LINK_HEADER_MAX 512
#define LINK_PCM_MAX 16384
#define LINK_TEXT_MAX 65536
#define LINK_STREAM_MAX 129
#define LINK_MESSAGE_MAX (LINK_HEADER_MAX + 1 + LINK_PCM_MAX)
#define LINK_CUES_MAX 32        /* word cues one frame header may carry (≤ 512 B header) */
#define LINK_CUE_WORDS 512      /* words of one say that can be cued (the reader's tokens) */
typedef struct {
    char stream[LINK_STREAM_MAX];
    uint32_t seq;
    bool end;
    /* Device→relay listen end frame only (header fields "reason", "speech_ms"): why the
     * listen ended ("silence" | "no_speech" | "max" | "error") and the voiced ms heard.
     * NULL reason: neither field is written. speech_known false (VAD off, or the listen
     * ended before VAD calibration): "speech_ms" is omitted, since 0 would claim
     * silence the device never measured. Parsing does not read them (NULL / 0 / false). */
    const char *reason;
    uint32_t speech_ms;
    bool speech_known;
    /* Relay→device say frames (header field "w", cap "say_cues"): word cues, each
     * [word, sample] — the say text's word `word` (0-based, whitespace-split) is spoken
     * from `sample` (16 kHz, counted from the stream's first sample). Up to
     * LINK_CUES_MAX per frame; parsed only (link_frame_header never writes them). */
    uint16_t ncues;
    uint16_t cue_word[LINK_CUES_MAX];
    uint32_t cue_sample[LINK_CUES_MAX];
} link_frame_t;
/* The newline is not counted in header_len. Payload is a borrowed byte view. */
bool link_frame_parse(const void *data, size_t len, link_frame_t *h,
                      const uint8_t **pcm, size_t *pcm_len);
int link_frame_header(char *out, size_t cap, const link_frame_t *h);
/* Rejection sampling: returns false for bytes outside a uniform alphabet range. */
bool link_code_char(uint8_t random, char *out);
void link_code_generate(char out[9], uint32_t (*random)(void *), void *ctx);
void link_id_generate(char out[LINK_ID_LEN + 1], uint32_t (*random)(void *), void *ctx);
bool link_id_valid(const char *id);
bool link_url_valid(const char *url);
unsigned link_retry_seconds(unsigned close_code, unsigned backoff);
cJSON *link_code_frame(const char *code);
cJSON *link_pair_frame(const char *req, bool ok, const char *reason);
cJSON *link_say_done_frame(const char *job);
cJSON *link_forget_frame(void);
/* NULL state means preserve the current visual state. */
bool link_mood_map(const char *mood, const char **state);
/* mood cmd args {mood, hold_s?}: hold_s 0..3600 s (0 = until the next mood); absent
 * → *hold_s = -1 (the mood's default: happy / surprised revert, the rest persist). */
bool link_mood_args(const cJSON *args, const char **mood, double *hold_s);
/* Device events (sense/sense.c), {"t":"evt","name":...,"data":{...}}:
 *   shake  {peak_g, reversals, ms}   tap {x, y} (0..1 of the panel)   button {id} */
/* listen end-of-speech args {vad?, end_silence_ms? 300..3000, no_speech_ms? 1000..10000}
 * (integers; vad a bool). Absent fields leave the outputs untouched (the caller's
 * defaults); a wrong type or an out-of-range value → false. */
bool link_listen_args(const cJSON *args, bool *vad, unsigned *end_silence_ms, unsigned *no_speech_ms);
/* The listen window (cap "listen_window"). Every listen ends early on end of speech / turn:
 * the window is a ceiling. An agent's "ms": 1..LINK_LISTEN_MAX_MS, or 0 = unlimited (it ends
 * only on end of speech / turn, no_speech, listen_stop, or LINK_LISTEN_CEILING_MS → "max").
 * Absent: the device's own setting (POST /api/creature/state {listen_ms}: 0 or
 * LINK_LISTEN_SET_MIN..LINK_LISTEN_MAX_MS, LINK_LISTEN_DEFAULT_MS out of the box). The mic
 * streams in fixed chunks: nothing on the device is sized by the window. */
#define LINK_LISTEN_MAX_MS      30000u
#define LINK_LISTEN_DEFAULT_MS  30000u
#define LINK_LISTEN_SET_MIN     5000u
#define LINK_LISTEN_UNLIMITED   0u
#define LINK_LISTEN_CEILING_MS  300000u   /* unlimited: the safety backstop (stuck mic, noisy room) */
/* args' "ms" (absent → dflt) → *window (0 = unlimited, else ms) and *record_ms (what the mic
 * records at most: the window, or the ceiling for unlimited). Not an integer 0..MAX → false. */
bool link_listen_window(const cJSON *args, uint32_t dflt, uint32_t *window, uint32_t *record_ms);
/* A valid device setting: 0 (unlimited) or LINK_LISTEN_SET_MIN..LINK_LISTEN_MAX_MS. */
bool link_listen_setting_ok(double v);
cJSON *link_evt_shake_frame(double peak_g, int reversals, unsigned ms);
cJSON *link_evt_tap_frame(double x, double y);
cJSON *link_evt_button_frame(bool accept);
/* A colour on the wire: "#rrggbb" (hex digits either case) → 0xRRGGBB. Anything else → false. */
bool link_color_parse(const char *s, uint32_t *rgb);
/* An optional colour field (listen's "ring_color"): absent → *rgb = -1 and true; a
 * "#rrggbb" string → its value; any other value (wrong type, bad hex, null) → false. */
bool link_color_arg(const cJSON *args, const char *key, int32_t *rgb);
/* ask cmd args {job, options, colors?, timeout_ms}: job a stream-sized id (1..128 chars);
 * options 1..LINK_ASK_MAX strings, each 1..LINK_ASK_LABEL_MAX chars of printable ASCII
 * (0x20..0x7E), not all spaces, no two equal ignoring case; timeout_ms an integer
 * 1000..120000. All required. Labels are copied verbatim (the answer echoes them).
 * colors (optional; the relay resolves names): one "#rrggbb" per option, in order →
 * colors[i] = 0xRRGGBB; absent → every colors[i] = -1 (the screen's default by index);
 * present but not an array of exactly n valid hex strings → false. */
#define LINK_ASK_MAX        5
#define LINK_ASK_LABEL_MAX  8
#define LINK_ASK_TIMEOUT_MIN 1000
#define LINK_ASK_TIMEOUT_MAX 120000
bool link_ask_args(const cJSON *args, char job[LINK_STREAM_MAX], char labels[LINK_ASK_MAX][LINK_ASK_LABEL_MAX + 1],
                   int32_t colors[LINK_ASK_MAX], int *n, unsigned *timeout_ms);
/* {"t":"evt","name":"answer","data":{...}}: index ≥ 0 → {job, index, label} (a tap);
 * index < 0 → {job, index:null, reason} ("timeout" | "cancelled"). */
cJSON *link_evt_answer_frame(const char *job, int index, const char *label, const char *reason);
/* After the pairing code / the Improv "hi": the state body to go back to (malloc'd),
 * the saved one unless it was text or the demo reel, which become the creature. */
char *link_restore_body(const char *saved_state_json);
