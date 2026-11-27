/*
 * SPDX-License-Identifier: MIT
 *
 * wave_test.c: the tone player (main/rhythm/wave_player.c) on its own.
 *   tables     the hit at a 0.5 peak, each cycle under 7.2 kHz (no harmonic past it), the picture's cycle at a 1 peak,
 *              the drum and its cycle one wave through the hand-over
 *   rates      one voice hits exactly its rate a second, slow and fast; the clock is its rate over its whole number
 *   ratios     whole-number tuning locks voice i's phase to n_i times the clock: a fifth hits 3 against 2, a major
 *              chord 4, 5 and 6 a turn, for ever; equal tuning plays the piano's pitches
 *   clicks     the design's offline sessions (audiotest.mjs): a sweep from 1 Hz to the top and back a semitone a click
 *              (the drum handing over to the cycles on the way), chords slow to fast, mutes, tuning switches, previews
 *              in a row, entering, stopping: energy over 7.5 kHz under -55 dB of the loudest window, every step within
 *              what a 7.2 kHz sine of that level makes; a deliberate hard cut is caught (the check is not blind)
 *   level      the loudest chord no further into the soft knee than the design's (0.651 of full scale)
 *   speaker    a job's yield ducks it out within 15 ms and it waits; back, it sounds again
 *   view       the picture's clock at a sample the speaker plays, from the snapshots, against the clock itself
 *   trace      a chord in whole numbers, fast, draws the same outline frame after frame; slow, a shiver behind the hand
 *              that settles back to the circle
 */
#include "wave_player.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int s_pass, s_fail;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define SR WAV_RATE

static wav_tables_t *T;
static wav_chord_t chord(int n, const int *count, const int *semi) {
    wav_chord_t c = { n, { 0 }, { 0 } };
    for (int i = 0; i < n; i++) { c.count[i] = (uint8_t)count[i]; c.semi[i] = (int8_t)semi[i]; }
    return c;
}
static const int ALONE_N[] = { 1 }, ALONE_S[] = { 0 }, FIFTH_N[] = { 2, 3 }, FIFTH_S[] = { 0, 7 }, MAJOR_N[] = { 4, 5, 6 }, MAJOR_S[] = { 0, 4, 7 };
static const int MINOR_N[] = { 10, 12, 15 }, MINOR_S[] = { 0, 3, 7 }, DOM7_N[] = { 4, 5, 6, 7 }, DOM7_S[] = { 0, 4, 7, 10 };
static const int DIM7_N[] = { 15, 18, 21, 25 }, DIM7_S[] = { 0, 3, 6, 9 }, SUS4_N[] = { 6, 8, 9 }, SUS4_S[] = { 0, 5, 7 };
static const int MAJ9_N[] = { 8, 10, 12, 15, 18 }, MAJ9_S[] = { 0, 4, 7, 11, 14 }, CLUS_N[] = { 15, 16, 17 }, CLUS_S[] = { 0, 1, 2 };
static const int DOM9_N[] = { 4, 5, 6, 7, 9 }, DOM9_S[] = { 0, 4, 7, 10, 14 };
#define CH(name) chord((int)(sizeof(name##_N) / sizeof(int)), name##_N, name##_S)

/* ── a session: steps of (ms, what changes first), into one float buffer ──────────────────────────────────────── */
typedef struct { float *x; int n, cap; } buf_t;
static void render(wav_player_t *p, buf_t *b, int ms) {
    const int n = SR * ms / 1000;
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2; b->x = realloc(b->x, sizeof(float) * (size_t)b->cap); }
    int16_t blk[256];
    for (int done = 0; done < n;) {
        const int k = n - done < 256 ? n - done : 256, got = (int)wav_fill(p, blk, (size_t)k);
        for (int i = 0; i < k; i++) b->x[b->n + done + i] = i < got ? blk[i] / 32000.f : 0.f;
        done += k;
    }
    b->n += n;
}
typedef struct { float worst_db, step, bound, peak; } clean_t;
/* the design's check: a 127-tap high-pass at 7.5 kHz, 4 ms windows against the loudest; the largest step */
static clean_t clean(const buf_t *b) {
    enum { TAPS = 127 };
    static float hp[TAPS];
    const float fc = 7500.f / SR;
    for (int i = 0; i < TAPS; i++) {
        const float m = i - (TAPS - 1) * 0.5f, sinc = m == 0.f ? 2.f * fc : sinf(6.2831853f * fc * m) / (3.1415927f * m);
        const float win = 0.42f - 0.5f * cosf(6.2831853f * i / (TAPS - 1)) + 0.08f * cosf(12.566371f * i / (TAPS - 1));
        hp[i] = (m == 0.f ? 1.f : 0.f) - sinc * win;
    }
    const int win = SR * 4 / 1000;
    double loud = 0.0;
    for (int i = 0; i + win <= b->n; i += win) { double e = 0.0; for (int k = i; k < i + win; k++) e += (double)b->x[k] * b->x[k]; if (e / win > loud) loud = e / win; }
    clean_t c = { -200.f, 0.f, 0.f, 0.f };
    for (int i = TAPS; i + win <= b->n; i += win) {
        double e = 0.0;
        for (int k = i; k < i + win; k++) { double y = 0.0; for (int j = 0; j < TAPS; j++) y += hp[j] * b->x[k - j]; e += y * y; }
        const float db = (float)(10.0 * log10(e / win / loud + 1e-20));
        if (db > c.worst_db) c.worst_db = db;
    }
    for (int i = 1; i < b->n; i++) { c.step = fmaxf(c.step, fabsf(b->x[i] - b->x[i - 1])); c.peak = fmaxf(c.peak, fabsf(b->x[i])); }
    c.bound = 2.f * sinf(3.1415927f * 7200.f / SR) * c.peak;
    return c;
}
static void turns(wav_player_t *p, buf_t *b, int from, int to, int ms) {
    for (int m = from; ; m += from < to ? 1 : -1) { wav_set_root(p, wav_hz((float)m)); render(p, b, ms); if (m == to) break; }
}
static void session(const char *name, void (*steps)(wav_player_t *, buf_t *)) {
    wav_player_t p;
    wav_init(&p, T);
    buf_t b = { 0 };
    steps(&p, &b);
    const clean_t c = clean(&b);
    printf("  %-26s %6.2f s  peak %.3f  over 7.5 kHz %6.1f dB  step %.3f (a 7.2 kHz sine's %.3f)\n", name, (double)b.n / SR, c.peak, c.worst_db, c.step, c.bound);
    CHECK(c.worst_db <= -55.f, "%s: %.1f dB over 7.5 kHz (a click)", name, c.worst_db);
    CHECK(c.step <= c.bound * 1.05f, "%s: a step of %.3f, past a 7.2 kHz sine's %.3f", name, c.step, c.bound);
    CHECK(c.peak <= 0.66f, "%s: peak %.3f, at most a little into the soft knee (the design's 0.651)", name, c.peak);
    free(b.x);
}
static void start(wav_player_t *p, wav_chord_t c, int midi, int tuning) { wav_chord(p, &c, wav_hz((float)midi), WAV_HOLD, tuning, true); }

static void s_sweep(wav_player_t *p, buf_t *b) { start(p, CH(ALONE), -36, WAV_JUST); render(p, b, 60); turns(p, b, -36, 113, 40); render(p, b, 300); turns(p, b, 113, -36, 25); render(p, b, 500); }
static void s_fifth(wav_player_t *p, buf_t *b) { start(p, CH(FIFTH), -12, WAV_JUST); render(p, b, 1560); turns(p, b, -12, 72, 60); render(p, b, 600); }
static void s_major(wav_player_t *p, buf_t *b) {
    start(p, CH(MAJOR), 60, WAV_JUST); render(p, b, 460);
    wav_set_mute(p, 2); render(p, b, 400); wav_set_mute(p, 0); render(p, b, 400); wav_set_mute(p, 5); render(p, b, 400); wav_set_mute(p, 0); render(p, b, 400);
    wav_set_tuning(p, WAV_EQUAL); render(p, b, 600); wav_set_tuning(p, WAV_JUST); render(p, b, 600); wav_silence(p); render(p, b, 600);
}
static void s_previews(wav_player_t *p, buf_t *b) {
    const wav_chord_t cs[] = { CH(MAJOR), CH(MINOR), CH(DOM7), CH(DIM7), CH(SUS4), CH(MAJ9), CH(CLUS) };
    for (int i = 0; i < 7; i++) { wav_chord(p, &cs[i], wav_hz(60.f + i), WAV_PLUCK, WAV_JUST, true); render(p, b, i % 2 ? 140 : 380); }
    render(p, b, 900);
    start(p, CH(MINOR), 57, WAV_JUST); render(p, b, 900); turns(p, b, 57, 20, 40); render(p, b, 500); wav_silence(p); render(p, b, 600);
}
static void s_minor_equal(wav_player_t *p, buf_t *b) { start(p, CH(MINOR), -6, WAV_EQUAL); render(p, b, 1560); turns(p, b, -6, 84, 50); render(p, b, 800); }
static void s_ninth(wav_player_t *p, buf_t *b) { start(p, CH(DOM9), 84, WAV_JUST); render(p, b, 60); turns(p, b, 84, 96, 50); render(p, b, 500); wav_silence(p); render(p, b, 500); }
static void s_cut(wav_player_t *p, buf_t *b) {   /* the control: a held note cut dead and started again with no ramp */
    start(p, CH(ALONE), 81, WAV_JUST); render(p, b, 460);
    p->v[0].env = p->v[0].envT = 0.f; render(p, b, 100);
    p->v[0].env = p->v[0].envT = 1.f; render(p, b, 300);
}

/* ── the rest ───────────────────────────────────────────────────────────────────────────────────────────────────── */
/* one sample at a time, counting each voice's hits (its phase wrapping) */
static void hits(wav_player_t *p, int samples, int *count) {
    int16_t s;
    uint32_t last[WAV_VOICES];
    for (int i = 0; i < WAV_VOICES; i++) last[i] = p->v[i].ph;
    for (int k = 0; k < samples; k++) {
        wav_fill(p, &s, 1);
        for (int i = 0; i < WAV_VOICES; i++) { if (p->v[i].ph < last[i]) count[i]++; last[i] = p->v[i].ph; }
    }
}

static void test_tables(void) {
    float pk = 0.f;
    for (int i = 0; i < WAV_HIT_LEN; i++) pk = fmaxf(pk, fabsf(T->hit[i]));
    CHECK(fabsf(pk - 0.5f) < 1e-4f && T->hit_peak == 0.5f, "the hit at a 0.5 peak (%.4f)", pk);
    /* each cycle: no harmonic past what fits under 7.2 kHz at the next semitone */
    float worst = -200.f;
    for (int c = 0; c < WAV_TABS; c += 8) {
        const float f = wav_hz((float)(WAV_TAB_LO + c)), K = floorf(WAV_TOP / wav_hz((float)(WAV_TAB_LO + c + 1)));
        double tot = 0.0, over = 0.0;
        for (int h = 1; h < WAV_TAB_N / 2; h++) {
            double re = 0.0, im = 0.0;
            for (int i = 0; i < WAV_TAB_N; i += 1) { re += T->tab[c][i] * cos(6.283185307 * h * i / WAV_TAB_N); im -= T->tab[c][i] * sin(6.283185307 * h * i / WAV_TAB_N); }
            const double e = re * re + im * im;
            tot += e; if (h > K) over += e;
            if (h * f > 16000.f) break;
        }
        worst = fmaxf(worst, (float)(10.0 * log10(over / tot + 1e-20)));
    }
    CHECK(worst < -80.f, "every cycle under 7.2 kHz: its harmonics past it at %.0f dB", worst);
    {   /* the picture's cycle, each pitch at its own peak: a held A4 traced with no circle and a unit reach */
        wav_player_t p;
        wav_init(&p, T);
        const wav_chord_t c = CH(ALONE);
        wav_chord(&p, &c, 440.f, WAV_HOLD, WAV_JUST, true);
        int16_t s[256];
        for (int i = 0; i < 20; i++) wav_fill(&p, s, 256);
        static float out[1001];
        wav_view_t v;
        wav_view(&p, p.n, &v);
        wav_trace(T, &v, 1.f, 0.f, 1.f, 0.12f, out, 1000);
        float wp = 0.f;
        for (int i = 0; i < 1000; i++) wp = fmaxf(wp, fabsf(out[i]));
        CHECK(fabsf(wp - 1.f) < 0.03f, "the picture's cycle at a 1 peak (%.3f)", wp);
    }
    /* the hand-over: through the zone the hit table and the cycles are one waveform at one phase (the cycle the hit
     * makes, choked by the next, against the cycle the tables hold for that semitone) */
    for (int c = 0; c <= 5; c += 5) {
        const float f = wav_hz((float)(WAV_TAB_LO + c)), P = SR / f, d = fminf(WAV_CHOKE, P * 0.5f);
        float diff = 0.f;
        for (int i = 0; i < WAV_TAB_N; i++) {
            const float tau = i * P / WAV_TAB_N;
            const int k = (int)tau, k2 = (int)(tau + P);
            float y = T->hit[k] + (T->hit[k + 1] - T->hit[k]) * (tau - k);
            if (tau < d && k2 < WAV_HIT_LEN) y += (1.f - tau / d) * (T->hit[k2] + (T->hit[k2 + 1] - T->hit[k2]) * (tau + P - k2));
            diff = fmaxf(diff, fabsf(y - T->tab[c][i]));
        }
        CHECK(diff < 0.02f, "at %.1f Hz the drum and its cycle are one wave: %.4f apart (of a 0.5 peak)", f, diff);
    }
}

static void test_rates(void) {
    const float rates[] = { 1.f, 4.f, 16.f, 64.f, 440.f, 3000.f };
    for (int r = 0; r < 6; r++) {
        wav_player_t p;
        wav_init(&p, T);
        wav_chord_t c = CH(ALONE);
        wav_chord(&p, &c, rates[r], WAV_HOLD, WAV_JUST, true);
        int n[WAV_VOICES] = { 0 };
        hits(&p, 4 * SR, n);
        CHECK(abs(n[0] - (int)lrintf(rates[r] * 4.f)) <= 1, "%.0f a second: %d hits in 4 s", rates[r], n[0]);
        CHECK(fabsf(p.fc - rates[r]) / rates[r] < 1e-3f, "the clock turns at the rate over its whole number (%.4f)", p.fc);
    }
}

static void test_ratios(void) {
    wav_player_t p;
    wav_init(&p, T);
    wav_chord_t c = CH(MAJOR);
    wav_chord(&p, &c, 8.f, WAV_HOLD, WAV_JUST, true);   /* the clock: 2 turns a second */
    int n[WAV_VOICES] = { 0 };
    hits(&p, 10 * SR, n);
    const int turn = (int)lrintf(p.fc * 10.f);
    CHECK(n[0] + 1 == 4 * turn && n[1] + 1 == 5 * turn && n[2] + 1 == 6 * turn, "major in whole numbers: %d %d %d hits in %d turns (4:5:6, the first at the start)", n[0] + 1, n[1] + 1, n[2] + 1, turn);
    bool locked = true;
    for (int i = 0; i < 3; i++) locked &= p.v[i].ph == p.v[i].n * p.H;
    CHECK(locked, "each voice's phase is its whole number times the clock, exactly");
    wav_chord_t f = CH(FIFTH);
    wav_chord(&p, &f, 300.f, WAV_HOLD, WAV_JUST, true);
    int m[WAV_VOICES] = { 0 };
    hits(&p, SR, m);
    CHECK(m[0] * 3 == m[1] * 2 || abs(m[0] * 3 - m[1] * 2) <= 3, "a fifth: %d against %d (3:2)", m[1], m[0]);
    CHECK(fabsf(p.v[1].f / p.v[0].f - 1.5f) < 1e-3f, "the fifth's top at 3/2 of its root (%.4f)", p.v[1].f / p.v[0].f);
    wav_player_t q;
    wav_init(&q, T);
    wav_chord(&q, &c, 261.63f, WAV_HOLD, WAV_EQUAL, true);
    int16_t s[512];
    wav_fill(&q, s, 512);
    CHECK(fabsf(q.v[1].f / q.v[0].f - powf(2.f, 4.f / 12.f)) < 2e-3f && fabsf(q.v[2].f / q.v[0].f - powf(2.f, 7.f / 12.f)) < 2e-3f, "equal tuning: the piano's third and fifth");
    CHECK(wav_top_midi(&c, WAV_JUST) == (int)floorf(69.f + 12.f * log2f(WAV_MAX_HZ / 1.5f / 440.f)), "the highest major root keeps its fifth under 5760 Hz");
}

static void test_speaker(void) {
    wav_player_t p;
    wav_init(&p, T);
    wav_chord_t c = CH(MAJOR);
    wav_chord(&p, &c, 220.f, WAV_HOLD, WAV_JUST, true);
    int16_t s[256];
    for (int i = 0; i < 10; i++) wav_fill(&p, s, 256);
    wav_yield(&p, true);
    int made = 0;
    for (int i = 0; i < 10; i++) { const size_t got = wav_fill(&p, s, 256); made += (int)got; if (got < 256) break; }
    CHECK(made > 0 && made < SR * 15 / 1000, "a job wants the speaker: ducked out in %d samples", made);
    CHECK(wav_fill(&p, s, 256) == 0 && wav_active(&p), "then nothing made, the chord still held");
    wav_yield(&p, false);
    CHECK(wav_fill(&p, s, 256) == 256, "back: it sounds again");
    wav_silence(&p);
    int left = 0;
    for (int i = 0; i < 100 && wav_fill(&p, s, 256) == 256; i++) left++;
    CHECK(left < 40 && !wav_active(&p), "silence: faded out and done in %d blocks", left);
    wav_set_mute(&p, 7);
    wav_chord(&p, &c, 220.f, WAV_HOLD, WAV_JUST, true);
    CHECK(wav_fill(&p, s, 256) == 256, "every voice muted, held: samples still made (the clock runs)");
}

static void test_view(void) {
    wav_player_t p;
    wav_init(&p, T);
    wav_chord_t c = CH(FIFTH);
    wav_chord(&p, &c, 3.f, WAV_HOLD, WAV_JUST, true);
    static float H[SR];
    int16_t s;
    for (int i = 0; i < SR; i++) { wav_fill(&p, &s, 1); H[i] = (float)p.H / 4294967296.f; }
    float worst = 0.f;
    for (int back = 100; back < 3800; back += 333) {
        wav_view_t v;
        const uint64_t at = p.n - (uint64_t)back;
        wav_view(&p, at, &v);
        float d = fabsf(v.H - H[(int)(at - (p.n - SR)) - 1]);
        d = fminf(d, 1.f - d);
        worst = fmaxf(worst, d);
        CHECK(fabsf(v.ph[1] - fmodf(3.f * v.H, 1.f)) < 1e-3f, "the view's fifth at 3 times its clock");
    }
    CHECK(worst < 2e-4f, "the picture's clock against the sound's, up to 240 ms back: %.6f of a turn", worst);
}

static void test_trace(void) {
    wav_player_t p;
    wav_init(&p, T);
    wav_chord_t c = CH(MAJOR);
    wav_chord(&p, &c, 220.f, WAV_HOLD, WAV_JUST, true);
    int16_t s[256];
    for (int i = 0; i < 40; i++) wav_fill(&p, s, 256);
    enum { N = 720 };
    static float a[N + 1], b[N + 1];
    wav_view_t v;
    wav_view(&p, p.n, &v); wav_trace(T, &v, 1.f, 0.5f, 0.14f, 0.12f, a, N);
    for (int i = 0; i < 7; i++) wav_fill(&p, s, 256);
    wav_view(&p, p.n, &v); wav_trace(T, &v, 1.f, 0.5f, 0.14f, 0.12f, b, N);
    float d = 0.f, lo = 1.f, hi = 0.f;
    for (int q = 0; q <= N; q++) { d = fmaxf(d, fabsf(a[q] - b[q])); lo = fminf(lo, a[q]); hi = fmaxf(hi, a[q]); }
    CHECK(d < 2e-3f && a[N] == a[0], "a major chord in whole numbers, fast: the same outline 112 ms later (%.5f)", d);
    CHECK(hi > 0.55f && lo > 0.3f && hi < 0.66f, "pushed out by its wave, within reach (%.3f .. %.3f)", lo, hi);
    /* slow: a shiver just behind the hand, the circle further back */
    wav_player_t q;
    wav_init(&q, T);
    wav_chord_t one = CH(ALONE);
    wav_chord(&q, &one, 1.f, WAV_HOLD, WAV_JUST, true);
    for (int i = 0; i < 10; i++) wav_fill(&q, s, 256);
    wav_view(&q, q.n, &v); wav_trace(T, &v, 0.f, 0.5f, 0.14f, 0.12f, a, N);
    const int hand = (int)lrintf(v.H * N);
    float near = 0.f, far = 0.f;
    for (int k = 1; k < 30; k++) near = fmaxf(near, fabsf(a[(hand - k + N) % N] - 0.5f));
    for (int k = N / 2; k < N / 2 + 60; k++) far = fmaxf(far, fabsf(a[(hand - k + N) % N] - 0.5f));
    CHECK(near > 0.01f && far < 1e-4f, "one a second: it shivers behind the hand (%.3f), the circle half a turn back (%.6f)", near, far);
}

int main(void) {
    T = wav_tables_make();
    if (!T) { printf("no memory\n"); return 1; }
    test_tables();
    test_rates();
    test_ratios();
    test_speaker();
    test_view();
    test_trace();
    printf("sessions (the design's audiotest):\n");
    session("alone-sweep", s_sweep);
    session("fifth-slow-to-fast", s_fifth);
    session("major-mute-tuning", s_major);
    session("previews-then-enter", s_previews);
    session("minor-equal-slow-to-fast", s_minor_equal);
    session("ninth-top", s_ninth);
    {
        wav_player_t p;
        wav_init(&p, T);
        buf_t b = { 0 };
        s_cut(&p, &b);
        const clean_t c = clean(&b);
        printf("  %-26s %6.1f dB: %s\n", "control-hard-cut", c.worst_db, c.worst_db > -55.f ? "caught" : "MISSED");
        CHECK(c.worst_db > -55.f, "the control (a hard cut) is caught: %.1f dB", c.worst_db);
        free(b.x);
    }
    {   /* the cost: a minute of the busiest chord (five voices) through the hand-over and up, as the audio task makes it */
        wav_player_t p;
        wav_init(&p, T);
        const wav_chord_t c = CH(DOM9);
        wav_chord(&p, &c, 30.f, WAV_HOLD, WAV_JUST, true);
        int16_t blk[256];
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int b = 0; b < 60 * SR / 256; b++) { if (b % 100 == 0) wav_set_root(&p, 30.f * (1 + b / 100)); wav_fill(&p, blk, 256); }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        const double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) * 1e-6;
        printf("  cost    a minute of a five-voice chord: %.1f ms here (%.2f us a 16 ms block); the player %zu bytes, its tables %zu\n", ms, ms * 1000. / (60 * SR / 256), sizeof(wav_player_t), sizeof(wav_tables_t));
    }
    wav_tables_free(T);
    printf("wave_test: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
