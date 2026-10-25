#include "link_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static uint32_t counter_random(void *ctx) {
    unsigned *counter = ctx;
    return (*counter)++;
}

static void test_codes(void)
{
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ23456789";
    unsigned counts[256] = {0}, accepted = 0;
    for (int i = 0; i < 256; i++) {
        char c = 0;
        bool ok = link_code_char((uint8_t)i, &c);
        assert(ok == (i < 238));
        if (ok) { assert(strchr(alphabet, c)); counts[(unsigned char)c]++; accepted++; }
    }
    assert(accepted == 238);
    for (const char *p = alphabet; *p; p++) assert(counts[(unsigned char)*p] == 7);
    char code[9] = {0};
    for (int i = 0; i < 8; i++) assert(link_code_char((uint8_t)(33 + i * 34), &code[i]));
    assert(strlen(code) == 8);
    unsigned counter = 235;
    memset(code, '?', sizeof(code));
    link_code_generate(code, counter_random, &counter);
    assert(strlen(code) == 8 && !strcmp(code, "789ABCDE"));
    assert(counter == 261); /* rejected 238..255, then A..E */
}

static void test_identity_and_urls(void)
{
    unsigned random = 24;
    char id[LINK_ID_LEN + 1];
    link_id_generate(id, counter_random, &random);
    assert(!strcmp(id, "molty-yz23456789") && link_id_valid(id));
    assert(!link_id_valid("molty-a1b2c3"));
    assert(!link_id_valid("molty-abcdefgh01"));
    assert(!link_id_valid("molty-ABCDEFGHIJ"));
    assert(!link_id_valid("molty-abcdefghijk"));
    assert(!link_id_valid(NULL));
    assert(link_url_valid("ws://192.168.1.2:8787"));
    assert(link_url_valid("wss://relay.example:443"));
    assert(!link_url_valid("http://relay.example"));
    assert(!link_url_valid("ws://"));
    assert(!link_url_valid("ws://user@host"));
    assert(!link_url_valid("wss://relay.example/v1/device"));
    assert(!link_url_valid("wss://relay.example?x=1"));
    assert(!link_url_valid("ws://relay.example\n"));
    assert(link_retry_seconds(4401, 1) == 300);
    assert(link_retry_seconds(4400, 1) == 1);
    assert(link_retry_seconds(4409, 60) == 60);
    assert(link_retry_seconds(4410, 2) == 2);
}

static void expect_json(cJSON *frame, const char *expected)
{
    assert(frame);
    char *encoded = cJSON_PrintUnformatted(frame);
    assert(encoded && !strcmp(encoded, expected));
    cJSON_free(encoded); cJSON_Delete(frame);
}

static void test_control_frames(void)
{
    expect_json(link_code_frame("ABCDEFGH"),
        "{\"t\":\"link_code\",\"code\":\"ABCDEFGH\",\"link_code\":\"ABCDEFGH\"}");
    expect_json(link_pair_frame("r1", false, "timeout"),
        "{\"t\":\"pair.result\",\"req\":\"r1\",\"ok\":false,\"reason\":\"timeout\"}");
    expect_json(link_pair_frame("r2", false, "rejected"),
        "{\"t\":\"pair.result\",\"req\":\"r2\",\"ok\":false,\"reason\":\"rejected\"}");
    expect_json(link_pair_frame("r3", true, NULL), "{\"t\":\"pair.result\",\"req\":\"r3\",\"ok\":true}");
    expect_json(link_say_done_frame("molty-abcdefghij.j_123"),
        "{\"t\":\"evt\",\"name\":\"say.done\",\"data\":{\"job\":\"molty-abcdefghij.j_123\"}}");
    expect_json(link_forget_frame(), "{\"t\":\"forget\"}");
}

static void test_moods(void)
{
    const char *moods[] = {"idle", "listening", "thinking", "speaking", "happy", "sleepy", "surprised"};
    const char *want[] = {"creature", "creature", "creature", "creature", "creature", "creature", "creature"};
    for (int i = 0; i < 7; i++) {
        const char *state = "sentinel";
        assert(link_mood_map(moods[i], &state));
        assert(want[i] ? state && !strcmp(state, want[i]) : state == NULL);
    }
    const char *state;
    assert(!link_mood_map("unknown", &state));
    assert(!link_mood_map(NULL, &state));
}

static bool parses(const char *str)
{
    link_frame_t h; const uint8_t *p; size_t n;
    return link_frame_parse(str, strlen(str), &h, &p, &n);
}

static void test_frames(void)
{
    uint8_t frame[LINK_MESSAGE_MAX + 2];
    link_frame_t h = {.stream = "job-\"quoted", .seq = 4294967295U, .end = true}, got;
    int n = link_frame_header((char *)frame, sizeof(frame), &h);
    assert(n > 0 && frame[n - 1] == '\n');
    memset(frame + n, 0xa5, LINK_PCM_MAX);
    const uint8_t *pcm; size_t bytes;
    assert(link_frame_parse(frame, n + LINK_PCM_MAX, &got, &pcm, &bytes));
    assert(!strcmp(h.stream, got.stream) && got.seq == h.seq && got.end);
    assert(bytes == LINK_PCM_MAX && pcm[0] == 0xa5 && pcm[bytes - 1] == 0xa5);
    assert(!link_frame_parse(frame, n + LINK_PCM_MAX + 2, &got, &pcm, &bytes));
    assert(!link_frame_parse(frame, n + 1, &got, &pcm, &bytes));
    assert(link_frame_parse(frame, n, &got, &pcm, &bytes) && bytes == 0 && got.end);
    assert(link_frame_header((char *)frame, 4, &h) == -1);
    assert(!parses("{\"s\":\"j\",\"seq\":0,\"kind\":\"pcm16\",\"rate\":8000,\"end\":true}\n"));
    assert(!parses("{\"s\":\"j\",\"seq\":-1,\"kind\":\"pcm16\",\"rate\":16000,\"end\":true}\n"));
    assert(!parses("{\"s\":\"j\",\"seq\":0.5,\"kind\":\"pcm16\",\"rate\":16000,\"end\":true}\n"));
    assert(!parses("{\"s\":\"j\",\"seq\":4294967296,\"kind\":\"pcm16\",\"rate\":16000,\"end\":true}\n"));
    assert(!parses("{\"s\":\"\",\"seq\":0,\"kind\":\"pcm16\",\"rate\":16000,\"end\":true}\n"));
    assert(!parses("{\"s\":\"j\",\"seq\":0,\"kind\":\"wav\",\"rate\":16000,\"end\":true}\n"));
    assert(!parses("{\"s\":\"j\",\"seq\":0,\"kind\":\"pcm16\",\"rate\":16000,\"end\":1}\n"));
    assert(parses("{\"s\":\"j\",\"seq\":0,\"kind\":\"pcm16\",\"rate\":16000}\n"));
    assert(parses("{\"s\":\"j\",\"seq\":0,\"kind\":\"pcm16\",\"rate\":16000,\"end\":true}"));
    char stream[129]; memset(stream, 's', 128); stream[128] = 0;
    strcpy(h.stream, stream); h.seq = 0;
    n = link_frame_header((char *)frame, sizeof(frame), &h);
    assert(n > 0 && link_frame_parse(frame, n, &got, &pcm, &bytes));
    assert(strlen(got.stream) == 128);
    assert(!parses("{}")); assert(!parses("{} trailing\n"));
    memset(frame, ' ', LINK_HEADER_MAX + 1); frame[LINK_HEADER_MAX + 1] = '\n';
    assert(!link_frame_parse(frame, LINK_HEADER_MAX + 2, &got, &pcm, &bytes));
    assert(!link_frame_parse(NULL, 0, &got, &pcm, &bytes));
}

int main(void)
{
    test_codes(); test_moods(); test_frames(); test_identity_and_urls(); test_control_frames();
    puts("LINK HOST PASS: audio/control frames, random IDs, code alphabet, URLs, close retry policy, moods");
    return 0;
}
