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
typedef struct {
    char stream[LINK_STREAM_MAX];
    uint32_t seq;
    bool end;
    /* Device→relay listen end frame only (header fields "reason", "speech_ms"): why the
     * listen ended ("silence" | "no_speech" | "max" | "error") and the voiced ms heard.
     * NULL reason: neither field is written. Parsing does not read them (NULL / 0). */
    const char *reason;
    uint32_t speech_ms;
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
cJSON *link_evt_shake_frame(double peak_g, int reversals, unsigned ms);
cJSON *link_evt_tap_frame(double x, double y);
cJSON *link_evt_button_frame(bool accept);
/* After the pairing code / the Improv "hi": the state body to go back to (malloc'd),
 * the saved one unless it was text or the demo reel, which become the creature. */
char *link_restore_body(const char *saved_state_json);
