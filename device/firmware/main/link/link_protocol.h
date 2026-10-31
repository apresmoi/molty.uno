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
/* Device→relay listen end frame (cap "trace"): the listen's own account, header "tr"
 * (docs/AGENT-LINK.md "Turn trace"). has_* false: that key is left out. */
typedef struct {
    uint32_t wall;          /* ms from the mic opening to its end                       */
    uint32_t cap;           /* ms of audio captured (after a shake's settle trim)        */
    uint32_t sent;          /* frames sent, this one included                            */
    uint32_t fail;          /* failed send attempts in all                               */
    uint32_t drop, dropms;  /* frames (and their ms) dropped: the device's ring overflowed */
    bool has_rssi0, has_rssi1;
    int r0, r1;             /* Wi-Fi rssi (dBm) at the listen's start / end               */
    uint32_t gain;          /* the mic gain                                              */
    uint32_t pk;            /* peak |sample|, permille of full scale                      */
    bool shake;             /* a shake's listen: "set" / "setc" are written              */
    uint32_t set;           /* ms of leading handling noise trimmed                      */
    bool setc;              /* the settle hit its cap                                    */
} link_trace_end_t;
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
    /* Device→relay listen frames (cap "trace", written only when `trace`): "ct" capture ms
     * of the frame's first sample from the mic start, "lv" one hex byte per 100 ms chunk
     * (link_level_byte), "cl" saturated samples (left out when 0), "rt" failed sends of
     * this frame before (left out when 0), "wq" ms it waited on the device; the end frame
     * adds "tr" (when tr is set). Should the header outgrow LINK_HEADER_MAX, "tr" then "lv"
     * are left out: the frame always goes. Parsing does not read them. */
    bool trace;
    uint32_t ct, cl, rt, wq;
    uint8_t nlv, lv[4];
    const link_trace_end_t *tr;
} link_frame_t;
/* A chunk's level byte for "lv": its RMS in dBFS → clamp(round(2 * (dBFS + 100)), 0, 200)
 * (0 = digital silence or <= -100 dBFS, 200 = full scale). sum_sq: Σ s² over n samples. */
uint8_t link_level_byte(double sum_sq, uint32_t n);
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
/* The auto filler (docs/AGENT-LINK.md "Quick phrases"): when a human turn ends with speech and
 * no agent answers within this delay, the RELAY has Molty say a short filler ("Hmm…") itself.
 * The device only keeps the owner's setting (POST /api/creature/state {filler_ms}, NVS like
 * ring_color) and reports it in every listen's answer (`filler_ms`): 0 = off, else
 * LINK_FILLER_MIN_MS..LINK_FILLER_MAX_MS; LINK_FILLER_DEFAULT_MS out of the box. */
#define LINK_FILLER_DEFAULT_MS 2500u
#define LINK_FILLER_MIN_MS     400u
#define LINK_FILLER_MAX_MS     5000u
#define LINK_FILLER_OFF        0u
/* A valid setting: 0 (off) or LINK_FILLER_MIN_MS..LINK_FILLER_MAX_MS, an integer. 400 ms: Molty's
 * filler lands almost as soon as the words are in (the relay counts from the listen's end). */
bool link_filler_setting_ok(double v);
/* A react mood (`mood {react:true}`) shows over the listening about this long before the relay
 * sends `listening` again (services/relay reactShowMs): the device's own listener reactions wait
 * for it (link_audio_external_react). */
#define LINK_REACT_SHOW_MS     1500u
/* The turn acknowledgement (cap "ack"): when a listen that heard speech ends, the device
 * itself plays a short chirp and shows `thinking` (held LINK_ACK_THINK_S, or until the
 * agent's next say / mood / ask / listen), so the human never waits in dead air while the
 * agent works out its answer. The device's setting (POST /api/creature/state {ack}, NVS like
 * ring_color): "chirp" (chirp + thinking, out of the box) | "thinking" (no sound) | "off". */
typedef enum { LINK_ACK_OFF = 0, LINK_ACK_THINKING = 1, LINK_ACK_CHIRP = 2 } link_ack_t;
#define LINK_ACK_DEFAULT  LINK_ACK_CHIRP
#define LINK_ACK_THINK_S  30          /* the acknowledgement's thinking reverts to idle after this */
/* "chirp" | "thinking" | "off" (an unknown mode reads as the default's name) */
const char *link_ack_name(link_ack_t mode);
/* A setting's name → mode; false when it is none of the three. */
bool link_ack_parse(const char *name, link_ack_t *mode);
/* listen/talk's per-listen "ack": absent or true → the device's setting; "silent" → no chirp
 * (thinking, unless the setting is off); false → off. An agent can only turn it down. A wrong
 * type / another string → false, *mode untouched. */
bool link_ack_arg(const cJSON *args, link_ack_t setting, link_ack_t *mode);
/* What a listen that ended acknowledges: the listen's mode when it heard speech — reason
 * "turn_end" (the relay's end of turn), "silence" (the device VAD's end of speech, with
 * speech measured > 0 if measured), or "max" (window full) with ≥ LINK_ACK_MIN_SPEECH_MS of
 * measured speech — and nothing else owns the screen (an explicit mood, an ask, an update
 * screen or a pairing arrived during the listen: screen_taken). no_speech / error / a
 * cancelled listen ("error" too) never ack. A chirp without a speaker is thinking only. */
#define LINK_ACK_MIN_SPEECH_MS 160u
link_ack_t link_ack_decide(link_ack_t mode, const char *reason, bool speech_known, uint32_t speech_ms,
                           bool screen_taken, bool has_speaker);
/* shake {peak_g, reversals, ms, listen?}: listen = the id of the listen the device began by
 * itself on this shake (shake to talk), for the relay to adopt; NULL = a plain shake. */
cJSON *link_evt_shake_frame(double peak_g, int reversals, unsigned ms, const char *listen);
/* Shake to talk (docs/AGENT-LINK.md "Shake to talk"): on a shake the device starts listening at
 * once, by itself, under a local id ("shake-<n>"), and names it in the shake event. The relay
 * adopts it with `cmd listen {job, adopt: <local id>, ...}` (its own job id and end-of-turn
 * settings from then on: the audio recorded so far replays through them and goes up first) or
 * declines it with `cmd listen_stop {job: <local id>}`. Not adopted within LINK_SHAKE_ADOPT_MS
 * of audio → it ends quietly: no frames, no acknowledgement, idle. Only when the relay's
 * welcome lists LINK_FEATURE_ADOPT. */
#define LINK_SHAKE_ADOPT_MS 3000u
#define LINK_FEATURE_ADOPT  "listen_adopt"
/* The relay's welcome {"t":"welcome","features":[...]} lists `name` (absent list: none). */
bool link_welcome_has(const cJSON *welcome, const char *name);
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
/* say marks (cap say_marks): `cmd say {..., cues:true, marks:[[word, "name"], ...]}` — the
 * relay's inline cues ([happy], [nod]) stripped out of the text. Each mark fires when the
 * speaker reaches word `word` (its word cue), or when playback ends for a word with no cue
 * (past the last word / LINK_CUE_WORDS). name: a mood (link_mood_map) or an action
 * (cute_act.h). At most LINK_MARKS_MAX; word 0..LINK_MARK_WORD_MAX. Absent = none. */
#define LINK_MARKS_MAX      16
#define LINK_MARK_WORD_MAX  4096
typedef struct { uint16_t word; bool act; int8_t id; } link_mark_t;   /* id: CM_* mood or CA_* action */
bool link_marks_parse(const cJSON *args, link_mark_t marks[LINK_MARKS_MAX], int *n);
/* A mood mark's name (its id, the creature's order); "" when out of range. */
const char *link_mood_name(int id);
/* act {action}: one of cute_act.h's names → *act (CA_*). */
bool link_act_args(const cJSON *args, int *act);
/* After the pairing code / the Improv "hi": the state body to go back to (malloc'd),
 * the saved one unless it was text or the demo reel, which become the creature. */
char *link_restore_body(const char *saved_state_json);
