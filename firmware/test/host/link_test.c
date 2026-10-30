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

static bool mood_args(const char *json, const char **mood, double *hold_s)
{
    cJSON *args = cJSON_Parse(json);
    assert(args);
    bool ok = link_mood_args(args, mood, hold_s);
    static char kept[16];                         /* the mood points into args */
    if (ok) { snprintf(kept, sizeof(kept), "%s", *mood); *mood = kept; }
    cJSON_Delete(args);
    return ok;
}

/* mood {mood, hold_s?}: old agents send no hold_s and keep working (default hold = -1) */
static void test_mood_args(void)
{
    const char *mood; double hold;
    assert(mood_args("{\"mood\":\"happy\"}", &mood, &hold) && !strcmp(mood, "happy") && hold == -1);
    assert(mood_args("{\"mood\":\"thinking\",\"hold_s\":0}", &mood, &hold) && hold == 0);
    assert(mood_args("{\"mood\":\"surprised\",\"hold_s\":2.5}", &mood, &hold) && hold == 2.5);
    assert(mood_args("{\"mood\":\"idle\",\"hold_s\":3600}", &mood, &hold) && hold == 3600);
    assert(!mood_args("{\"mood\":\"happy\",\"hold_s\":-1}", &mood, &hold));
    assert(!mood_args("{\"mood\":\"happy\",\"hold_s\":3601}", &mood, &hold));
    assert(!mood_args("{\"mood\":\"happy\",\"hold_s\":\"2\"}", &mood, &hold));
    assert(!mood_args("{\"mood\":\"grumpy\"}", &mood, &hold));
    assert(!mood_args("{\"hold_s\":2}", &mood, &hold));
}

/* device events: exactly the frames docs/AGENT-LINK.md lists */
static bool listen_args(const char *json, bool *vad, unsigned *es, unsigned *ns)
{
    cJSON *args = cJSON_Parse(json);
    assert(args);
    bool ok = link_listen_args(args, vad, es, ns);
    cJSON_Delete(args);
    return ok;
}

static void test_listen_args(void)
{
    bool vad = true; unsigned es = 700, ns = 4000;
    assert(listen_args("{\"ms\":8000}", &vad, &es, &ns) && vad && es == 700 && ns == 4000);
    assert(listen_args("{\"vad\":false}", &vad, &es, &ns) && !vad && es == 700 && ns == 4000);
    assert(listen_args("{\"vad\":true,\"end_silence_ms\":300,\"no_speech_ms\":10000}", &vad, &es, &ns)
           && vad && es == 300 && ns == 10000);
    assert(listen_args("{\"end_silence_ms\":3000,\"no_speech_ms\":1000}", &vad, &es, &ns) && es == 3000 && ns == 1000);
    es = 700; ns = 4000;
    /* a bad field leaves every output untouched */
    assert(!listen_args("{\"end_silence_ms\":800,\"no_speech_ms\":999}", &vad, &es, &ns) && es == 700 && ns == 4000);
    assert(!listen_args("{\"end_silence_ms\":299}", &vad, &es, &ns));
    assert(!listen_args("{\"end_silence_ms\":3001}", &vad, &es, &ns));
    assert(!listen_args("{\"end_silence_ms\":700.5}", &vad, &es, &ns));
    assert(!listen_args("{\"no_speech_ms\":10001}", &vad, &es, &ns));
    assert(!listen_args("{\"no_speech_ms\":\"4000\"}", &vad, &es, &ns));
    assert(!listen_args("{\"vad\":0}", &vad, &es, &ns) && vad);
    assert(!listen_args("{\"vad\":false,\"end_silence_ms\":1}", &vad, &es, &ns) && vad);
    assert(!link_listen_args(NULL, NULL, &es, &ns));
}

static void test_event_frames(void)
{
    expect_json(link_evt_shake_frame(1.83456, 4, 412),
        "{\"t\":\"evt\",\"name\":\"shake\",\"data\":{\"peak_g\":1.83,\"reversals\":4,\"ms\":412}}");
    expect_json(link_evt_tap_frame(0.25, 0.5004),
        "{\"t\":\"evt\",\"name\":\"tap\",\"data\":{\"x\":0.25,\"y\":0.5}}");
    expect_json(link_evt_tap_frame(-0.1, 1.7),
        "{\"t\":\"evt\",\"name\":\"tap\",\"data\":{\"x\":0,\"y\":1}}");
    expect_json(link_evt_button_frame(true), "{\"t\":\"evt\",\"name\":\"button\",\"data\":{\"id\":\"accept\"}}");
    expect_json(link_evt_button_frame(false), "{\"t\":\"evt\",\"name\":\"button\",\"data\":{\"id\":\"reject\"}}");
}

/* after the pairing code / the Improv "hi": back to the shown state, never the reel */
static void test_restore(void)
{
    const struct { const char *saved, *want; } cases[] = {
        { "{\"state\":\"creature\",\"autoplay\":false,\"mood\":\"idle\"}", "{\"state\":\"creature\"}" },
        { "{\"state\":\"effect:vortex\",\"autoplay\":false}", "{\"state\":\"effect:vortex\"}" },
        { "{\"state\":\"blob\",\"autoplay\":true}", "{\"state\":\"creature\"}" },
        { "{\"state\":\"text\",\"autoplay\":false}", "{\"state\":\"creature\"}" },
        { "{\"state\":\"autoplay\"}", "{\"state\":\"creature\"}" },
        { "not json", "{\"state\":\"creature\"}" },
        { NULL, "{\"state\":\"creature\"}" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *body = link_restore_body(cases[i].saved);
        assert(body && !strcmp(body, cases[i].want));
        free(body);
    }
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
    /* listen end frame: {reason, speech_ms} after end; parse ignores them (relay too) */
    link_frame_t e = {.stream = "j", .seq = 7, .end = true, .reason = "silence", .speech_ms = 1420, .speech_known = true};
    n = link_frame_header((char *)frame, sizeof(frame), &e);
    frame[n] = 0;
    assert(!strcmp((char *)frame, "{\"s\":\"j\",\"seq\":7,\"kind\":\"pcm16\",\"rate\":16000,\"end\":true,\"reason\":\"silence\",\"speech_ms\":1420}\n"));
    got.reason = "x"; got.speech_ms = 9;
    assert(link_frame_parse(frame, n, &got, &pcm, &bytes) && got.end && got.seq == 7 && !got.reason && !got.speech_ms);
    /* VAD off / ended inside calibration: the reason, but no unmeasured speech_ms:0 */
    e.reason = "max"; e.speech_ms = 0; e.speech_known = false;
    n = link_frame_header((char *)frame, sizeof(frame), &e);
    frame[n] = 0;
    assert(!strcmp((char *)frame, "{\"s\":\"j\",\"seq\":7,\"kind\":\"pcm16\",\"rate\":16000,\"end\":true,\"reason\":\"max\"}\n"));
    e.speech_known = true;
    n = link_frame_header((char *)frame, sizeof(frame), &e);
    frame[n] = 0;
    assert(strstr((char *)frame, "\"speech_ms\":0}"));   /* a measured 0 is still sent */
    e.reason = NULL;                    /* every other frame: unchanged shape */
    n = link_frame_header((char *)frame, sizeof(frame), &e);
    frame[n] = 0;
    assert(!strcmp((char *)frame, "{\"s\":\"j\",\"seq\":7,\"kind\":\"pcm16\",\"rate\":16000,\"end\":true}\n"));
}

static bool ask_ok(const char *json, int *n_out) {
    cJSON *a = cJSON_Parse(json); assert(a);
    char job[LINK_STREAM_MAX], labels[LINK_ASK_MAX][LINK_ASK_LABEL_MAX + 1]; int n = -1; unsigned to = 0;
    bool ok = link_ask_args(a, job, labels, &n, &to);
    if (ok && n_out) *n_out = n;
    cJSON_Delete(a);
    return ok;
}
#define ASK(opts, to) "{\"job\":\"j1\",\"options\":" opts ",\"timeout_ms\":" to "}"

static void test_ask_args(void)
{
    cJSON *a = cJSON_Parse("{\"job\":\"molty-x.j_3\",\"options\":[\"Yes\",\" no \",\"(a)~\"],\"timeout_ms\":15000,\"extra\":1}");
    char job[LINK_STREAM_MAX], labels[LINK_ASK_MAX][LINK_ASK_LABEL_MAX + 1]; int n; unsigned to;
    assert(link_ask_args(a, job, labels, &n, &to) && n == 3 && to == 15000);
    /* verbatim: the answer's label must equal options[index] exactly */
    assert(!strcmp(job, "molty-x.j_3") && !strcmp(labels[0], "Yes") && !strcmp(labels[1], " no ") && !strcmp(labels[2], "(a)~"));
    cJSON_Delete(a);
    int got = 0;
    assert(ask_ok(ASK("[\"a\"]", "1000"), &got) && got == 1);
    assert(ask_ok(ASK("[\"a\",\"b\",\"c\",\"d\",\"e\"]", "120000"), &got) && got == 5);
    /* every printable ASCII char is allowed (the font draws them all) */
    assert(ask_ok(ASK("[\"12345678\",\" ~!@#$%^\",\"&*()_+{}\",\"|:<>?`[]\",\"\\\\;',./\\\"-\"]", "5000"), &got) && got == 5);
    /* count: 1..5 */
    assert(!ask_ok(ASK("[]", "5000"), NULL) && !ask_ok(ASK("[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\"]", "5000"), NULL));
    /* length 1..8 */
    assert(!ask_ok(ASK("[\"\"]", "5000"), NULL) && !ask_ok(ASK("[\"123456789\"]", "5000"), NULL));
    /* printable ASCII only, not all spaces */
    assert(!ask_ok(ASK("[\"   \"]", "5000"), NULL) && !ask_ok(ASK("[\"a\\tb\"]", "5000"), NULL));
    assert(!ask_ok(ASK("[\"a\\u007f\"]", "5000"), NULL) && !ask_ok(ASK("[\"caf\\u00e9\"]", "5000"), NULL));
    assert(!ask_ok(ASK("[\"a\\nb\"]", "5000"), NULL) && !ask_ok(ASK("[\"\\u0000a\"]", "5000"), NULL));
    /* unique ignoring case (they render alike: the font has capitals only) */
    assert(!ask_ok(ASK("[\"Yes\",\"no\",\"YES\"]", "5000"), NULL) && !ask_ok(ASK("[\"a\",\"a\"]", "5000"), NULL));
    assert(ask_ok(ASK("[\"yes\",\"yes \"]", "5000"), NULL) && ask_ok(ASK("[\"ab\",\"abc\"]", "5000"), NULL));
    /* option types */
    assert(!ask_ok(ASK("[\"a\",1]", "5000"), NULL) && !ask_ok(ASK("\"a\"", "5000"), NULL) && !ask_ok(ASK("[null]", "5000"), NULL));
    /* timeout_ms: integer 1000..120000, required */
    assert(!ask_ok(ASK("[\"a\"]", "999"), NULL) && !ask_ok(ASK("[\"a\"]", "120001"), NULL));
    assert(!ask_ok(ASK("[\"a\"]", "1500.5"), NULL) && !ask_ok(ASK("[\"a\"]", "\"5000\""), NULL));
    assert(!ask_ok("{\"job\":\"j1\",\"options\":[\"a\"]}", NULL));
    /* job: 1..128 chars, required */
    assert(!ask_ok("{\"options\":[\"a\"],\"timeout_ms\":5000}", NULL) && !ask_ok("{\"job\":\"\",\"options\":[\"a\"],\"timeout_ms\":5000}", NULL));
    char big[300]; char id[130]; memset(id, 'j', 129); id[129] = 0;
    snprintf(big, sizeof(big), "{\"job\":\"%s\",\"options\":[\"a\"],\"timeout_ms\":5000}", id);
    assert(!ask_ok(big, NULL));
    id[128] = 0; snprintf(big, sizeof(big), "{\"job\":\"%s\",\"options\":[\"a\"],\"timeout_ms\":5000}", id);
    assert(ask_ok(big, NULL));

    /* the answer events, exactly */
    cJSON *f = link_evt_answer_frame("j1", 2, "(a)~", NULL);
    char *t = cJSON_PrintUnformatted(f);
    assert(!strcmp(t, "{\"t\":\"evt\",\"name\":\"answer\",\"data\":{\"job\":\"j1\",\"index\":2,\"label\":\"(a)~\"}}"));
    cJSON_free(t); cJSON_Delete(f);
    f = link_evt_answer_frame("j1", -1, NULL, "timeout"); t = cJSON_PrintUnformatted(f);
    assert(!strcmp(t, "{\"t\":\"evt\",\"name\":\"answer\",\"data\":{\"job\":\"j1\",\"index\":null,\"reason\":\"timeout\"}}"));
    cJSON_free(t); cJSON_Delete(f);
    f = link_evt_answer_frame("j1", -1, NULL, "cancelled"); t = cJSON_PrintUnformatted(f);
    assert(!strcmp(t, "{\"t\":\"evt\",\"name\":\"answer\",\"data\":{\"job\":\"j1\",\"index\":null,\"reason\":\"cancelled\"}}"));
    cJSON_free(t); cJSON_Delete(f);
}

int main(void)
{
    test_codes(); test_moods(); test_frames(); test_identity_and_urls(); test_control_frames();
    test_mood_args(); test_listen_args(); test_event_frames(); test_restore(); test_ask_args();
    puts("LINK HOST PASS: audio/control/event frames, random IDs, code alphabet, URLs, close retry policy, moods + hold_s, listen args, restore, ask args + answer frames");
    return 0;
}
