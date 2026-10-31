#include "link_protocol.h"
#include "cute_act.h"
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

static bool window(const char *json, uint32_t dflt, uint32_t *w, uint32_t *rec)
{
    cJSON *args = cJSON_Parse(json);
    assert(args);
    bool ok = link_listen_window(args, dflt, w, rec);
    cJSON_Delete(args);
    return ok;
}

/* the listen window: 1..30000, 0 = unlimited (records to the 300 s ceiling), absent = the setting */
static void test_listen_window(void)
{
    uint32_t w = 7, r = 7;
    assert(window("{\"ms\":30000}", 30000, &w, &r) && w == 30000 && r == 30000);
    assert(window("{\"ms\":1}", 30000, &w, &r) && w == 1 && r == 1);
    assert(window("{\"ms\":0}", 30000, &w, &r) && w == 0 && r == LINK_LISTEN_CEILING_MS && r == 300000);
    assert(window("{}", 12000, &w, &r) && w == 12000 && r == 12000);          /* the device's setting */
    assert(window("{}", 0, &w, &r) && w == 0 && r == 300000);                  /* ...unlimited */
    assert(window("{}", 999, &w, &r) && w == LINK_LISTEN_DEFAULT_MS && w == 30000);   /* a bad setting: the default */
    w = r = 7;
    assert(!window("{\"ms\":30001}", 30000, &w, &r) && w == 7 && r == 7);
    assert(!window("{\"ms\":-1}", 30000, &w, &r) && !window("{\"ms\":1.5}", 30000, &w, &r));
    assert(!window("{\"ms\":\"30000\"}", 30000, &w, &r) && !window("{\"ms\":null}", 30000, &w, &r));
    assert(!link_listen_window(NULL, 0, NULL, &r));
    /* the console's setting: 0 or 5..30 s */
    assert(link_listen_setting_ok(0) && link_listen_setting_ok(5000) && link_listen_setting_ok(30000));
    assert(!link_listen_setting_ok(4999) && !link_listen_setting_ok(30001) && !link_listen_setting_ok(10000.5) && !link_listen_setting_ok(-1));
    /* the auto filler's delay: 0 (off) or 0.4..5 s (a filler almost at once, for a slow agent) */
    assert(link_filler_setting_ok(0) && link_filler_setting_ok(400) && link_filler_setting_ok(500) && link_filler_setting_ok(800) && link_filler_setting_ok(1500) && link_filler_setting_ok(2500) && link_filler_setting_ok(5000));
    assert(LINK_FILLER_MIN_MS == 400 && LINK_FILLER_MAX_MS == 5000);
    assert(!link_filler_setting_ok(1) && !link_filler_setting_ok(399) && !link_filler_setting_ok(5001) && !link_filler_setting_ok(2500.5) && !link_filler_setting_ok(-1));
    assert(LINK_FILLER_DEFAULT_MS == 2500 && LINK_FILLER_OFF == 0);
}

/* The turn acknowledgement's rules (link_ack_decide) and its args / setting names. */
static link_ack_t ack_of(const char *json, link_ack_t setting, bool *ok)
{
    cJSON *args = cJSON_Parse(json);
    assert(args);
    link_ack_t m = (link_ack_t)99;
    *ok = link_ack_arg(args, setting, &m);
    cJSON_Delete(args);
    return m;
}
static void test_ack(void)
{
    const link_ack_t C = LINK_ACK_CHIRP, T = LINK_ACK_THINKING, O = LINK_ACK_OFF;
    /* which ends acknowledge: speech heard */
    assert(link_ack_decide(C, "tap", false, 0, false, true) == C);          /* a tap ended the turn: acknowledged like the relay's */
    assert(link_say_stalled(8000, 0) && !link_say_stalled(7999, 0));        /* no audio at all: 8 s */
    assert(link_say_stalled(30000, 1) && !link_say_stalled(29999, 1));      /* mid-stream: 30 s */
    assert(link_ack_decide(C, "turn_end", false, 0, false, true) == C);     /* the relay's end of turn: speech by definition */
    assert(link_ack_decide(C, "turn_end", true, 0, false, true) == C);
    assert(link_ack_decide(C, "silence", true, 900, false, true) == C);
    assert(link_ack_decide(C, "silence", false, 0, false, true) == C);      /* unmeasured: the VAD called it */
    assert(link_ack_decide(C, "silence", true, 0, false, true) == O);       /* measured nothing */
    assert(link_ack_decide(C, "max", true, 160, false, true) == C);
    assert(link_ack_decide(C, "max", true, 159, false, true) == O);
    assert(link_ack_decide(C, "max", false, 5000, false, true) == O);       /* vad:false: unknown */
    /* ...and nothing else */
    const char *never[] = {"no_speech", "error", "cancelled", "ended", "", "TURN_END"};
    for (size_t i = 0; i < sizeof(never) / sizeof(*never); i++)
        assert(link_ack_decide(C, never[i], true, 3000, false, true) == O);
    assert(link_ack_decide(C, NULL, true, 3000, false, true) == O);
    /* opt-out, screen owners, no speaker */
    assert(link_ack_decide(T, "turn_end", true, 900, false, true) == T);
    assert(link_ack_decide(O, "turn_end", true, 900, false, true) == O);
    assert(link_ack_decide(C, "turn_end", true, 900, true, true) == O);     /* explicit mood / ask / update / pairing */
    assert(link_ack_decide(T, "silence", true, 900, true, true) == O);
    assert(link_ack_decide(C, "turn_end", true, 900, false, false) == T);   /* no speaker: thinking only */
    assert(link_ack_decide((link_ack_t)7, "turn_end", true, 900, false, true) == O);
    /* per-listen "ack": absent / true → the setting; "silent" → no chirp; false → off */
    bool ok;
    assert(ack_of("{}", C, &ok) == C && ok);
    assert(ack_of("{\"ack\":true}", T, &ok) == T && ok);
    assert(ack_of("{\"ack\":\"silent\"}", C, &ok) == T && ok);
    assert(ack_of("{\"ack\":\"silent\"}", O, &ok) == O && ok);          /* never turned up */
    assert(ack_of("{\"ack\":true}", O, &ok) == O && ok);
    assert(ack_of("{\"ack\":false}", C, &ok) == O && ok);
    assert(ack_of("{}", (link_ack_t)9, &ok) == LINK_ACK_DEFAULT && ok);       /* a bad setting: the default */
    const char *bad[] = {"{\"ack\":\"loud\"}", "{\"ack\":1}", "{\"ack\":null}", "{\"ack\":\"Silent\"}", "{\"ack\":\"chirp\"}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        assert(ack_of(bad[i], C, &ok) == (link_ack_t)99 && !ok);
    }
    assert(!link_ack_arg(NULL, C, NULL));
    /* the setting's names */
    link_ack_t m;
    assert(link_ack_parse("chirp", &m) && m == C && link_ack_parse("thinking", &m) && m == T && link_ack_parse("off", &m) && m == O);
    assert(!link_ack_parse("silent", &m) && !link_ack_parse("Chirp", &m) && !link_ack_parse("", &m) && !link_ack_parse(NULL, &m));
    assert(!strcmp(link_ack_name(C), "chirp") && !strcmp(link_ack_name(T), "thinking") && !strcmp(link_ack_name(O), "off"));
    assert(!strcmp(link_ack_name((link_ack_t)5), "chirp") && LINK_ACK_DEFAULT == C && LINK_ACK_THINK_S == 30);
}

static void test_event_frames(void)
{
    expect_json(link_evt_shake_frame(1.83456, 4, 412, NULL),
        "{\"t\":\"evt\",\"name\":\"shake\",\"data\":{\"peak_g\":1.83,\"reversals\":4,\"ms\":412}}");
    /* shake to talk: the listen the device began by itself, for the relay to adopt */
    expect_json(link_evt_shake_frame(2.4, 5, 900, "shake-3"),
        "{\"t\":\"evt\",\"name\":\"shake\",\"data\":{\"peak_g\":2.4,\"reversals\":5,\"ms\":900,\"listen\":\"shake-3\"}}");
    cJSON *w = cJSON_Parse("{\"t\":\"welcome\",\"features\":[\"x\",\"listen_adopt\"]}");
    assert(link_welcome_has(w, LINK_FEATURE_ADOPT) && !link_welcome_has(w, "listen") && !link_welcome_has(w, NULL));
    cJSON_Delete(w);
    w = cJSON_Parse("{\"t\":\"welcome\"}"); assert(!link_welcome_has(w, LINK_FEATURE_ADOPT)); cJSON_Delete(w);
    w = cJSON_Parse("{\"t\":\"welcome\",\"features\":\"listen_adopt\"}"); assert(!link_welcome_has(w, LINK_FEATURE_ADOPT)); cJSON_Delete(w);
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

/* say frames' word cues: "w":[[word, sample], ...] */
static bool cues(const char *w, link_frame_t *got)
{
    char frame[LINK_HEADER_MAX + 8];
    int n = snprintf(frame, sizeof(frame), "{\"s\":\"j\",\"seq\":0,\"kind\":\"pcm16\",\"rate\":16000,\"w\":%s}\n", w);
    const uint8_t *pcm; size_t pl;
    return n > 0 && link_frame_parse(frame, (size_t)n, got, &pcm, &pl);
}
static void test_cues(void)
{
    link_frame_t g;
    assert(cues("[[0,0],[1,3000],[511,4294967295]]", &g) && g.ncues == 3 && g.cue_word[1] == 1 && g.cue_sample[1] == 3000 && g.cue_word[2] == 511 && g.cue_sample[2] == 4294967295u);
    assert(cues("[]", &g) && g.ncues == 0);
    char many[LINK_HEADER_MAX]; int o = sprintf(many, "[");
    for (int i = 0; i < LINK_CUES_MAX; i++) o += sprintf(many + o, "%s[%d,%d]", i ? "," : "", i, i);
    strcpy(many + o, "]");
    assert(cues(many, &g) && g.ncues == LINK_CUES_MAX);
    many[o] = 0; strcat(many, ",[1,1]]");
    assert(!cues(many, &g));                                     /* > LINK_CUES_MAX */
    const char *bad[] = { "[[512,0]]", "[[-1,0]]", "[[0,-1]]", "[[0,1.5]]", "[[0.5,1]]", "[[0]]", "[[0,1,2]]", "[\"x\"]", "{}", "1", "[[0,4294967296]]" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) assert(!cues(bad[i], &g));
    /* no "w": no cues */
    link_frame_t h = {.stream = "j"}; uint8_t f[LINK_HEADER_MAX + 2]; const uint8_t *pcm; size_t pl;
    int n = link_frame_header((char *)f, sizeof(f), &h);
    g.ncues = 9; assert(link_frame_parse(f, (size_t)n, &g, &pcm, &pl) && g.ncues == 0);
}

/* say marks (cap say_marks) and act: the names are the creature's moods and actions
 * (cute_act.h), words 0..LINK_MARK_WORD_MAX, at most LINK_MARKS_MAX; anything else refuses
 * the say (the relay only sends valid ones) */
static bool marks(const char *json, link_mark_t *m, int *n)
{
    char buf[1024]; snprintf(buf, sizeof(buf), "{\"marks\":%s}", json);
    cJSON *a = cJSON_Parse(buf); assert(a);
    bool ok = link_marks_parse(a, m, n); cJSON_Delete(a); return ok;
}
static void test_marks(void)
{
    link_mark_t m[LINK_MARKS_MAX]; int n = -1;
    cJSON *none = cJSON_Parse("{}");
    assert(link_marks_parse(none, m, &n) && n == 0); cJSON_Delete(none);
    assert(marks("[[0,\"happy\"],[2,\"thinking\"],[2,\"nod\"],[4096,\"shake_head\"]]", m, &n) && n == 4);
    assert(!m[0].act && m[0].id == 4 && m[0].word == 0 && !strcmp(link_mood_name(m[0].id), "happy"));
    assert(!m[1].act && !strcmp(link_mood_name(m[1].id), "thinking") && m[1].word == 2);
    assert(m[2].act && m[2].id == CA_NOD && m[3].act && m[3].id == CA_SHAKE_HEAD && m[3].word == 4096);
    assert(marks("[]", m, &n) && n == 0);
    char many[1024]; int o = sprintf(many, "[");
    for (int i = 0; i < LINK_MARKS_MAX; i++) o += sprintf(many + o, "%s[%d,\"blink\"]", i ? "," : "", i);
    strcpy(many + o, "]");
    assert(marks(many, m, &n) && n == LINK_MARKS_MAX);
    many[o] = 0; strcat(many, ",[1,\"nod\"]]");
    assert(!marks(many, m, &n) && n == 0);                            /* > LINK_MARKS_MAX */
    const char *bad[] = { "[[0,\"Happy\"]]", "[[0,\"dance\"]]", "[[-1,\"nod\"]]", "[[4097,\"nod\"]]", "[[1.5,\"nod\"]]",
                          "[[0]]", "[[0,\"nod\",1]]", "[\"nod\"]", "{}", "1", "[[\"nod\",0]]", "[[0,1]]" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) assert(!marks(bad[i], m, &n) && n == 0);
    assert(!link_mood_name(-1)[0] && !link_mood_name(7)[0] && !strcmp(link_mood_name(6), "surprised"));
    /* act {action} */
    int act = -1;
    for (int a = 0; a < CUTE_N_ACTS; a++) {
        char b[64]; snprintf(b, sizeof(b), "{\"action\":\"%s\"}", cute_act_name(a));
        cJSON *j = cJSON_Parse(b); assert(link_act_args(j, &act) && act == a); cJSON_Delete(j);
    }
    const char *nope[] = { "{}", "{\"action\":1}", "{\"action\":\"dance\"}", "{\"action\":\"NOD\"}", "{\"action\":\"\"}" };
    for (size_t i = 0; i < sizeof(nope) / sizeof(*nope); i++) { cJSON *j = cJSON_Parse(nope[i]); assert(!link_act_args(j, &act)); cJSON_Delete(j); }
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
    char job[LINK_STREAM_MAX], labels[LINK_ASK_MAX][LINK_ASK_LABEL_MAX + 1]; int32_t cols[LINK_ASK_MAX]; int n = -1; unsigned to = 0;
    bool ok = link_ask_args(a, job, labels, cols, &n, &to);
    if (ok && n_out) *n_out = n;
    cJSON_Delete(a);
    return ok;
}
#define ASK(opts, to) "{\"job\":\"j1\",\"options\":" opts ",\"timeout_ms\":" to "}"

static void test_ask_args(void)
{
    cJSON *a = cJSON_Parse("{\"job\":\"molty-x.j_3\",\"options\":[\"Yes\",\" no \",\"(a)~\"],\"timeout_ms\":15000,\"extra\":1}");
    char job[LINK_STREAM_MAX], labels[LINK_ASK_MAX][LINK_ASK_LABEL_MAX + 1]; int32_t cols[LINK_ASK_MAX]; int n; unsigned to;
    assert(link_ask_args(a, job, labels, cols, &n, &to) && n == 3 && to == 15000);
    for (int i = 0; i < LINK_ASK_MAX; i++) assert(cols[i] == -1);   /* no colors: the screen's defaults */
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

    /* colors: optional, exactly one "#rrggbb" (either case) per option, in order */
    {
        cJSON *c = cJSON_Parse("{\"job\":\"j\",\"options\":[\"a\",\"b\",\"c\"],\"colors\":[\"#3ccf96\",\"#FF6B6B\",\"#000000\"],\"timeout_ms\":5000}");
        char cj[LINK_STREAM_MAX], cl[LINK_ASK_MAX][LINK_ASK_LABEL_MAX + 1]; int32_t cc[LINK_ASK_MAX]; int cn; unsigned cto;
        assert(link_ask_args(c, cj, cl, cc, &cn, &cto) && cn == 3 && cc[0] == 0x3ccf96 && cc[1] == 0xff6b6b && cc[2] == 0 && cc[3] == -1);
        cJSON_Delete(c);
    }
#define ASKC(opts, cols) "{\"job\":\"j1\",\"options\":" opts ",\"colors\":" cols ",\"timeout_ms\":5000}"
    assert(!ask_ok(ASKC("[\"a\",\"b\"]", "[\"#3ccf96\"]"), NULL));                    /* one short */
    assert(!ask_ok(ASKC("[\"a\"]", "[\"#3ccf96\",\"#3ccf96\"]"), NULL));                /* one too many */
    assert(!ask_ok(ASKC("[\"a\"]", "[\"green\"]"), NULL));                             /* names are the relay's */
    assert(!ask_ok(ASKC("[\"a\"]", "[\"#3cf\"]"), NULL) && !ask_ok(ASKC("[\"a\"]", "[\"#3ccf9g\"]"), NULL));
    assert(!ask_ok(ASKC("[\"a\"]", "[\"3ccf96\"]"), NULL) && !ask_ok(ASKC("[\"a\"]", "[\"#3ccf960\"]"), NULL));
    assert(!ask_ok(ASKC("[\"a\"]", "[3985302]"), NULL) && !ask_ok(ASKC("[\"a\"]", "\"#3ccf96\""), NULL) && !ask_ok(ASKC("[\"a\"]", "null"), NULL));
    assert(ask_ok(ASKC("[\"a\"]", "[\"#a1B2c3\"]"), NULL));
    /* link_color_arg: absent → -1; "#rrggbb" → value; anything else → false */
    {
        cJSON *c = cJSON_Parse("{\"x\":\"#00ff00\",\"n\":5,\"b\":\"#00ff0\",\"z\":null}"); int32_t v = 7;
        assert(link_color_arg(c, "missing", &v) && v == -1);
        assert(link_color_arg(c, "x", &v) && v == 0x00ff00);
        assert(!link_color_arg(c, "n", &v) && !link_color_arg(c, "b", &v) && !link_color_arg(c, "z", &v));
        cJSON_Delete(c);
        uint32_t u;
        assert(link_color_parse("#FFFFFF", &u) && u == 0xffffff && !link_color_parse("#fffff", &u) && !link_color_parse(NULL, &u) && !link_color_parse("fffffff", &u));
    }

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

/* the turn trace on listen frames (cap "trace", docs/AGENT-LINK.md "Turn trace"): exact keys,
 * left-out zeros, the end frame's tr; the header never outgrows LINK_HEADER_MAX — the trace
 * gives way (tr, then lv), never the frame */
static void test_trace_frames(void)
{
    char f[LINK_HEADER_MAX + 2];
    link_frame_t h = {.stream = "molty-abcdefghij.j_AbCdEfGhIjKlMnOp", .seq = 3, .trace = true, .ct = 900, .wq = 12, .nlv = 3, .lv = {0, 0x9c, 200}};
    int n = link_frame_header(f, sizeof(f), &h);
    assert(n > 0 && !strcmp(f, "{\"s\":\"molty-abcdefghij.j_AbCdEfGhIjKlMnOp\",\"seq\":3,\"kind\":\"pcm16\",\"rate\":16000,\"end\":false,\"ct\":900,\"lv\":\"009cc8\",\"wq\":12}\n"));
    h.cl = 18; h.rt = 2;
    n = link_frame_header(f, sizeof(f), &h);
    assert(n > 0 && strstr(f, ",\"cl\":18,\"rt\":2,\"wq\":12}"));
    link_trace_end_t tr = {.wall = 8970, .cap = 5400, .sent = 18, .fail = 3, .drop = 1, .dropms = 300, .has_rssi0 = true, .r0 = -69,
                           .has_rssi1 = true, .r1 = -79, .gain = 6, .pk = 998, .shake = true, .set = 340, .setc = false};
    h.end = true; h.reason = "turn_end"; h.speech_ms = 1200; h.speech_known = true; h.tr = &tr; h.cl = 0; h.rt = 0;
    n = link_frame_header(f, sizeof(f), &h);
    assert(n > 0 && !strcmp(f, "{\"s\":\"molty-abcdefghij.j_AbCdEfGhIjKlMnOp\",\"seq\":3,\"kind\":\"pcm16\",\"rate\":16000,\"end\":true,"
                               "\"reason\":\"turn_end\",\"speech_ms\":1200,\"ct\":900,\"lv\":\"009cc8\",\"wq\":12,"
                               "\"tr\":{\"wall\":8970,\"cap\":5400,\"sent\":18,\"fail\":3,\"drop\":1,\"dropms\":300,\"r0\":-69,\"r1\":-79,\"gain\":6,\"pk\":998,\"set\":340,\"setc\":0}}\n"));
    /* no rssi known, not a shake: those keys left out */
    tr.has_rssi0 = tr.has_rssi1 = false; tr.shake = false;
    n = link_frame_header(f, sizeof(f), &h);
    assert(n > 0 && !strstr(f, "\"r0\"") && !strstr(f, "\"set\"") && strstr(f, "\"gain\":6,\"pk\":998}"));
    /* parsing ignores them (the say path reads frames with the same parser) */
    link_frame_t g; const uint8_t *pcm; size_t pl;
    assert(link_frame_parse(f, (size_t)n, &g, &pcm, &pl) && g.seq == 3 && g.end && pl == 0);
    /* worst case: a 128-char stream id and every number at its widest — still a frame */
    link_frame_t w = {.seq = UINT32_MAX, .end = true, .reason = "no_speech", .speech_ms = UINT32_MAX, .speech_known = true,
                      .trace = true, .ct = UINT32_MAX, .cl = UINT32_MAX, .rt = UINT32_MAX, .wq = UINT32_MAX, .nlv = 4, .lv = {200, 200, 200, 200}};
    memset(w.stream, 'x', 128); w.stream[128] = 0;
    link_trace_end_t big = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, true, true, -2147483647, -2147483647,
                            UINT32_MAX, UINT32_MAX, true, UINT32_MAX, true};
    w.tr = &big;
    n = link_frame_header(f, sizeof(f), &w);
    assert(n > 0 && n <= LINK_HEADER_MAX + 1 && !strstr(f, "\"tr\""));    /* the tr gave way */
    assert(link_frame_parse(f, (size_t)n, &g, &pcm, &pl) && g.end);
    /* a realistic job id keeps everything */
    strcpy(w.stream, "molty-abcdefghij.j_AbCdEfGhIjKlMnOp");
    n = link_frame_header(f, sizeof(f), &w);
    assert(n > 0 && n <= LINK_HEADER_MAX + 1 && strstr(f, "\"tr\"") && strstr(f, "\"lv\""));
    /* the level byte: 2 * (dBFS + 100), 0..200 */
    assert(link_level_byte(0, 1600) == 0 && link_level_byte(1, 0) == 0);
    assert(link_level_byte(32768.0 * 32768.0 * 1600, 1600) == 200);           /* 0 dBFS */
    const double a = 32768.0 * pow(10, -20.0 / 20);                             /* -20 dBFS */
    assert(link_level_byte(a * a * 1600, 1600) == 160);
    assert(link_level_byte(1e-6, 1600) == 0);                                    /* below -100 dBFS */
}

int main(void)
{
    test_codes(); test_moods(); test_frames(); test_cues(); test_marks(); test_identity_and_urls(); test_control_frames();
    test_mood_args(); test_listen_args(); test_listen_window(); test_ack(); test_event_frames(); test_restore(); test_ask_args(); test_trace_frames();
    puts("LINK HOST PASS: audio/control/event frames, random IDs, code alphabet, URLs, close retry policy, moods + hold_s, listen args + window, turn ack rules, say cues + marks, act, restore, ask args + answer frames, turn-trace frames (ct lv cl rt wq, end tr, size bound, level byte)");
    return 0;
}
