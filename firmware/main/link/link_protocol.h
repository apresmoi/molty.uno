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
