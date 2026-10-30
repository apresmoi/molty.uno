/*
 * SPDX-License-Identifier: MIT
 *
 * vad_test.c — host test for main/audio/vad.c on synthetic 16 kHz PCM16, fed in the
 * device's 100 ms mic chunks.
 *
 * Checks: room noise / digital silence → no_speech at no_speech_ms; a 1.5 s utterance →
 * silence ~end_silence_ms after it; pauses < 500 ms inside an utterance do not end it;
 * a softer tail stays voiced (hysteresis); a fan switched on mid-listen becomes the
 * floor and does not count as speech for ever; speech over a fan; speech from t = 0;
 * a click is not speech; the max cap (and vad off = the full window); min_record_ms;
 * odd chunk sizes give the same verdict; the verdict is sticky; short replies said
 * at t = 0 (inside calibration) still count; speech_ms is unknown with VAD off or
 * before calibration ends.
 *
 * Build + run: test/host/vad_build.sh   (VAD_SRC overrides vad.c, for mutation checks)
 */
#include "vad.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int s_fail = 0, s_pass = 0;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define PI 3.14159265f
#define QUIET (-65.f)          /* room noise at gain 8 */

/* ── signal: a list of segments, each a level over a time span ─────────────── */
typedef enum { NOISE, SPEECH, ZERO } kind_t;
typedef struct { kind_t kind; float db; uint32_t ms; } seg_t;

static uint32_t s_rng;
static float urand(void) { s_rng = s_rng * 1664525u + 1013904223u; return (float)(s_rng >> 8) / 16777216.f; }
/* ~gaussian, unit RMS (sum of 4 uniforms) */
static float gauss(void) { return (urand() + urand() + urand() + urand() - 2.f) * 1.7320508f; }

static float amp(float db) { return 32768.f * powf(10.f, db / 20.f); }

/* sample n of the signal (n counts from 0); `bg` is a background noise level added
 * under everything (a fan), -inf for none */
static int16_t sample(const seg_t *segs, int nsegs, uint32_t n, float bg_db, uint32_t bg_from_ms)
{
    uint32_t t_ms = n / 16, start = 0;
    const seg_t *s = &segs[nsegs - 1];
    for (int i = 0; i < nsegs; i++) { if (t_ms < start + segs[i].ms) { s = &segs[i]; break; } start += segs[i].ms; }
    float t = (float)n / 16000.f, x = 0.f;
    switch (s->kind) {
    case ZERO: break;
    case NOISE: x = amp(s->db) * gauss(); break;
    case SPEECH: {
        /* voiced-ish: 140 Hz + harmonics + breath noise, syllables at 4 Hz dipping ~10 dB */
        float env = 0.3f + 0.7f * fabsf(sinf(PI * 4.f * t));
        float tone = sinf(2 * PI * 140 * t) + 0.5f * sinf(2 * PI * 280 * t) + 0.3f * sinf(2 * PI * 420 * t);
        x = amp(s->db) * env * (1.2f * tone + 0.3f * gauss());
        x += amp(QUIET) * gauss();
        break;
    }
    }
    if (bg_db > -200.f && t_ms >= bg_from_ms) x += amp(bg_db) * gauss();
    if (x > 32767.f) x = 32767.f;
    if (x < -32768.f) x = -32768.f;
    return (int16_t)lrintf(x);
}

typedef struct { vad_verdict_t v; uint32_t end_ms, speech_ms, first_end_at; } run_t;

/* feed in `chunk`-sample reads (1600 = the device's 100 ms) until a verdict or 12 s */
static run_t run_bg(const vad_cfg_t *cfg, const seg_t *segs, int nsegs, size_t chunk,
                    float bg_db, uint32_t bg_from_ms)
{
    vad_t v; vad_init(&v, cfg);
    s_rng = 12345;
    static int16_t buf[4096];
    uint32_t n = 0;
    run_t r = {0};
    while (n < 16000u * 12) {
        for (size_t i = 0; i < chunk; i++) buf[i] = sample(segs, nsegs, n + (uint32_t)i, bg_db, bg_from_ms);
        n += (uint32_t)chunk;
        r.v = vad_feed(&v, buf, chunk);
        if (r.v != VAD_CONTINUE) break;
    }
    r.end_ms = v.t_ms; r.speech_ms = v.speech_ms;
    /* sticky: more audio does not change it */
    vad_verdict_t again = vad_feed(&v, buf, chunk);
    CHECK(again == r.v && v.t_ms == r.end_ms, "verdict not sticky");
    return r;
}
static run_t run(const vad_cfg_t *cfg, const seg_t *segs, int nsegs) { return run_bg(cfg, segs, nsegs, 1600, -999.f, 0); }
#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

static vad_cfg_t cfg_max(uint32_t max_ms) { vad_cfg_t c = VAD_CFG_DEFAULT; c.max_ms = max_ms; return c; }

static void test_no_speech(void)
{
    vad_cfg_t c = cfg_max(8000);
    seg_t room[] = {{NOISE, QUIET, 60000}};
    run_t r = run(&c, room, N(room));
    CHECK(r.v == VAD_END_NO_SPEECH && r.end_ms == 4000 && r.speech_ms == 0,
          "room noise: verdict %d at %u ms, speech %u ms", r.v, r.end_ms, r.speech_ms);
    CHECK(!strcmp(vad_reason(r.v), "no_speech"), "reason");

    seg_t zero[] = {{ZERO, 0, 60000}};
    r = run(&c, zero, N(zero));
    CHECK(r.v == VAD_END_NO_SPEECH && r.end_ms == 4000, "digital silence: verdict %d at %u ms", r.v, r.end_ms);

    c.no_speech_ms = 2500;                       /* tunable */
    r = run(&c, room, N(room));
    CHECK(r.v == VAD_END_NO_SPEECH && r.end_ms == 2500, "no_speech_ms 2500: verdict %d at %u ms", r.v, r.end_ms);

    /* a single 100 ms click/knock is not speech */
    seg_t click[] = {{NOISE, QUIET, 1000}, {NOISE, -20.f, 100}, {NOISE, QUIET, 60000}};
    c = cfg_max(8000);
    r = run(&c, click, N(click));
    CHECK(r.v == VAD_END_NO_SPEECH && r.end_ms == 4000, "click: verdict %d at %u ms (speech %u)", r.v, r.end_ms, r.speech_ms);
}

static void test_utterance(void)
{
    vad_cfg_t c = cfg_max(8000);
    seg_t u[] = {{NOISE, QUIET, 500}, {SPEECH, -25.f, 1500}, {NOISE, QUIET, 60000}};
    run_t r = run(&c, u, N(u));
    CHECK(r.v == VAD_END_SILENCE, "utterance: verdict %d", r.v);
    CHECK(r.end_ms >= 2000 + 640 && r.end_ms <= 2000 + 780, "utterance: ended %u ms after speech end (want ~700)", r.end_ms - 2000);
    CHECK(r.speech_ms >= 1200 && r.speech_ms <= 1500, "utterance: speech %u ms", r.speech_ms);
    CHECK(!strcmp(vad_reason(r.v), "silence") && vad_reason(VAD_CONTINUE) == NULL, "reason");

    c.end_silence_ms = 1500;                     /* tunable */
    r = run(&c, u, N(u));
    CHECK(r.v == VAD_END_SILENCE && r.end_ms >= 2000 + 1440 && r.end_ms <= 2000 + 1580,
          "end_silence 1500: ended %u ms after speech end", r.end_ms - 2000);

    /* quiet speaker at the soft end (−45 dBFS, 20 dB over the room) still ends on silence */
    c = cfg_max(8000);
    seg_t soft[] = {{NOISE, QUIET, 500}, {SPEECH, -45.f, 1500}, {NOISE, QUIET, 60000}};
    r = run(&c, soft, N(soft));
    CHECK(r.v == VAD_END_SILENCE && r.end_ms <= 2000 + 780 && r.speech_ms >= 800,
          "soft speaker: verdict %d at +%d ms, speech %u", r.v, (int)r.end_ms - 2000, r.speech_ms);

    /* speech already running when listen starts (no quiet calibration) */
    seg_t early[] = {{SPEECH, -25.f, 1500}, {NOISE, QUIET, 60000}};
    r = run(&c, early, N(early));
    CHECK(r.v == VAD_END_SILENCE && r.end_ms >= 1500 + 640 && r.end_ms <= 1500 + 800,
          "speech from t=0: verdict %d at %u ms", r.v, r.end_ms);
}

static void test_pauses(void)
{
    /* three phrases with 450 ms pauses: one utterance */
    vad_cfg_t c = cfg_max(8000);
    seg_t u[] = {{NOISE, QUIET, 500}, {SPEECH, -25.f, 800}, {NOISE, QUIET, 450}, {SPEECH, -28.f, 700},
                 {NOISE, QUIET, 450}, {SPEECH, -25.f, 900}, {NOISE, QUIET, 60000}};
    uint32_t speech_end = 500 + 800 + 450 + 700 + 450 + 900;
    run_t r = run(&c, u, N(u));
    CHECK(r.v == VAD_END_SILENCE && r.end_ms >= speech_end + 640 && r.end_ms <= speech_end + 780,
          "pauses: verdict %d at %u ms (speech ended %u)", r.v, r.end_ms, speech_end);
}

static void test_soft_tail(void)
{
    /* loud, then a soft tail 7.5 dB over the room (inside the hysteresis band) */
    vad_cfg_t c = cfg_max(8000);
    seg_t u[] = {{NOISE, QUIET, 500}, {SPEECH, -25.f, 1000}, {NOISE, QUIET + 7.5f, 1200}, {NOISE, QUIET, 60000}};
    run_t r = run(&c, u, N(u));
    CHECK(r.v == VAD_END_SILENCE && r.end_ms >= 2700 + 640, "soft tail cut at %u ms (tail ends 2700)", r.end_ms);
}

static void test_fan(void)
{
    vad_cfg_t c = cfg_max(10000);
    seg_t room[] = {{NOISE, QUIET, 60000}};
    /* a fan 30 dB over the room switches on at 1 s: the floor follows, the listen ends */
    run_t r = run_bg(&c, room, N(room), 1600, -35.f, 1000);
    CHECK(r.v != VAD_END_MAX && r.v != VAD_CONTINUE, "fan from 1 s: never ended (verdict %d)", r.v);
    CHECK(r.end_ms <= 1000 + 3000 + 900 && r.speech_ms <= 3200, "fan from 1 s: ended at %u ms, speech %u", r.end_ms, r.speech_ms);

    /* fan running from the start, louder than init_floor_max_dbfs: it counts until the
     * floor window has filled, then it is the floor and the listen ends */
    r = run_bg(&c, room, N(room), 1600, -35.f, 0);
    CHECK(r.v == VAD_END_SILENCE && r.end_ms <= 3000 + 800, "fan from 0: verdict %d at %u ms", r.v, r.end_ms);
    /* a quieter hum from the start (under the ceiling): the floor at once → no_speech */
    r = run_bg(&c, room, N(room), 1600, -52.f, 0);
    CHECK(r.v == VAD_END_NO_SPEECH && r.end_ms == 4000, "hum from 0: verdict %d at %u ms", r.v, r.end_ms);

    /* speech over a hum: ends ~700 ms after the speech */
    seg_t u[] = {{NOISE, QUIET, 1000}, {SPEECH, -15.f, 1500}, {NOISE, QUIET, 60000}};
    r = run_bg(&c, u, N(u), 1600, -52.f, 0);
    CHECK(r.v == VAD_END_SILENCE && r.end_ms >= 2500 + 640 && r.end_ms <= 2500 + 800,
          "speech over hum: verdict %d at %u ms", r.v, r.end_ms);
    /* speech over a loud fan: the fan counts until the floor window fills (3 s),
     * so the end comes ~700 ms after that instead of after the speech */
    r = run_bg(&c, u, N(u), 1600, -40.f, 0);
    CHECK(r.v == VAD_END_SILENCE && r.end_ms >= 2500 + 640 && r.end_ms <= 3000 + 800,
          "speech over fan: verdict %d at %u ms", r.v, r.end_ms);
}

static void test_max_and_off(void)
{
    vad_cfg_t c = cfg_max(3000);
    seg_t talk[] = {{NOISE, QUIET, 300}, {SPEECH, -25.f, 60000}};
    run_t r = run(&c, talk, N(talk));
    CHECK(r.v == VAD_END_MAX && r.end_ms == 3000, "max: verdict %d at %u ms", r.v, r.end_ms);
    CHECK(!strcmp(vad_reason(r.v), "max"), "reason");

    /* vad off: the full window, whatever is heard */
    c = cfg_max(8000); c.enabled = false;
    seg_t room[] = {{NOISE, QUIET, 60000}};
    r = run(&c, room, N(room));
    CHECK(r.v == VAD_END_MAX && r.end_ms == 8000, "vad off, silence: verdict %d at %u ms", r.v, r.end_ms);
    seg_t u[] = {{NOISE, QUIET, 500}, {SPEECH, -25.f, 1500}, {NOISE, QUIET, 60000}};
    r = run(&c, u, N(u));
    CHECK(r.v == VAD_END_MAX && r.end_ms == 8000, "vad off, utterance: verdict %d at %u ms", r.v, r.end_ms);

    /* window shorter than no_speech_ms: max wins */
    c = cfg_max(2000);
    r = run(&c, room, N(room));
    CHECK(r.v == VAD_END_MAX && r.end_ms == 2000, "short window: verdict %d at %u ms", r.v, r.end_ms);
}

static void test_min_record_and_chunks(void)
{
    vad_cfg_t c = cfg_max(8000);
    c.calib_ms = 20; c.min_speech_ms = 20; c.end_silence_ms = 300;
    seg_t blip[] = {{NOISE, QUIET, 40}, {NOISE, -25.f, 60}, {NOISE, QUIET, 60000}};
    run_t r = run(&c, blip, N(blip));
    CHECK(r.v == VAD_END_SILENCE && r.end_ms == 600, "min_record: verdict %d at %u ms", r.v, r.end_ms);

    /* odd read sizes: the same frames, the same verdict */
    c = cfg_max(8000);
    seg_t u[] = {{NOISE, QUIET, 500}, {SPEECH, -25.f, 1500}, {NOISE, QUIET, 60000}};
    run_t a = run(&c, u, N(u));
    run_t b = run_bg(&c, u, N(u), 777, -999.f, 0);
    CHECK(a.v == b.v && a.end_ms == b.end_ms && a.speech_ms == b.speech_ms,
          "chunking: %d/%u/%u vs %d/%u/%u", a.v, a.end_ms, a.speech_ms, b.v, b.end_ms, b.speech_ms);
}

/* A short reply right at t = 0, inside the 300 ms calibration: the calibration frames
 * are classified retroactively, so "yes" is speech (silence + speech_ms), never
 * no_speech (the relay would skip STT and lose it). Also mid-listen, for parity. */
static void test_immediate_reply(void)
{
    vad_cfg_t c = cfg_max(8000);
    const uint32_t lens[] = {200, 300, 400};
    for (int i = 0; i < 3; i++) {
        seg_t u[] = {{SPEECH, -25.f, lens[i]}, {NOISE, QUIET, 60000}};
        run_t r = run(&c, u, N(u));
        CHECK(r.v == VAD_END_SILENCE && r.speech_ms >= lens[i] - 60 && r.speech_ms <= lens[i] + 20,
              "immediate %u ms reply: verdict %d at %u ms, speech %u", lens[i], r.v, r.end_ms, r.speech_ms);
        CHECK(r.end_ms <= lens[i] + 800, "immediate %u ms reply ended late: %u ms", lens[i], r.end_ms);
        seg_t m[] = {{NOISE, QUIET, 1000}, {SPEECH, -25.f, lens[i]}, {NOISE, QUIET, 60000}};
        r = run(&c, m, N(m));
        CHECK(r.v == VAD_END_SILENCE && r.speech_ms > 0, "mid-listen %u ms reply: verdict %d, speech %u", lens[i], r.v, r.speech_ms);
    }
}

static void test_speech_known(void)
{
    vad_t v;
    static int16_t quiet[16000];                 /* 1 s: every feed below fits in it */
    vad_cfg_t c = cfg_max(8000);
    CHECK(c.calib_ms > 100 && c.calib_ms <= 400, "calib_ms %u: the steps below assume 100 < calib <= 400", (unsigned)c.calib_ms);
    vad_init(&v, &c);
    CHECK(!vad_speech_known(&v), "known before any audio");
    vad_feed(&v, quiet, 1600);                   /* 100 ms in: < calib_ms */
    CHECK(!vad_speech_known(&v), "known inside calibration");
    vad_feed(&v, quiet, 4800);                   /* 400 ms in: >= calib_ms */
    CHECK(vad_speech_known(&v), "unknown after calibration");
    c.enabled = false;
    vad_init(&v, &c);
    vad_feed(&v, quiet, N(quiet));               /* a full second with VAD off */
    CHECK(!vad_speech_known(&v), "known with VAD off");
}

/* the mic level for the screen's ring (vad_level): ~0 for the room, ~1 for close speech,
 * the loudest frame since the last read (the peak resets), VAD on or off */
static void test_level(void)
{
    const seg_t segs[] = { { NOISE, QUIET, 1000 }, { SPEECH, -25.f, 1000 }, { NOISE, QUIET, 1000 } };
    for (int on = 0; on < 2; on++) {
        vad_cfg_t c = VAD_CFG_DEFAULT; c.enabled = on; c.max_ms = 10000; c.no_speech_ms = 10000;
        vad_t v; vad_init(&v, &c);
        CHECK(vad_level(&v) == 0.f, "vad=%d: level before any audio", on);
        static int16_t buf[1600];
        float room = 0.f, speech = 0.f, after = 1.f;
        s_rng = 7;
        for (uint32_t ch = 0; ch < 30; ch++) {
            for (uint32_t i = 0; i < 1600; i++) buf[i] = sample(segs, 3, ch * 1600 + i, -999.f, 0);
            vad_feed(&v, buf, 1600);
            float l = vad_level(&v);
            CHECK(l >= 0.f && l <= 1.f, "level %f out of 0..1", l);
            if (ch >= 3 && ch < 10 && l > room) room = l;
            if (ch >= 12 && ch < 19 && l > speech) speech = l;
            if (ch == 29) after = l;
        }
        CHECK(room < 0.15f && speech > 0.8f && after < 0.15f, "vad=%d: room %.2f, speech %.2f, after %.2f", on, room, speech, after);
        CHECK(vad_level(&v) == 0.f, "vad=%d: the peak did not reset", on);
    }
}

int main(void)
{
    test_no_speech();
    test_utterance();
    test_pauses();
    test_soft_tail();
    test_fan();
    test_max_and_off();
    test_min_record_and_chunks();
    test_immediate_reply();
    test_speech_known();
    test_level();
    printf("vad_test: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
