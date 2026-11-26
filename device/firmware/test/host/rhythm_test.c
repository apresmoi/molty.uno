/*
 * rhythm_test.c: the beat player (main/rhythm/rhythm_player.c) and its wire format (rhythm_wire.c), on the host.
 *   argv[1]: the molty-rhythm skill's library as it sends it (rhythm_build.sh makes it with library.mjs)
 * Timing is proven against an independent computation in long double: every hit of five minutes, at several tempi,
 * uneven steps and swing, lands on the first sample at or after its ideal time (so within one sample of it).
 */
#include "rhythm_player.h"
#include "rhythm_wire.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails, passes;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static rhy_pattern_t LIB[RHY_LIB];
static int NLIB;
static int lib_index(const char *id) { for (int i = 0; i < NLIB; i++) if (!strcmp(LIB[i].id, id)) return i; return -1; }

/* ── a log of every hit ─────────────────────────────────────────────────────────────────────────────────────────── */
typedef struct { uint64_t n; int layer, step; bool acc; } hit_t;
static hit_t *s_log; static size_t s_nlog, s_cap;
static void logit(void *ctx, uint64_t n, int layer, int step, bool acc) {
    (void)ctx;
    if (s_nlog == s_cap) { s_cap = s_cap ? s_cap * 2 : 4096; s_log = realloc(s_log, s_cap * sizeof(hit_t)); }
    s_log[s_nlog++] = (hit_t){ n, layer, step, acc };
}
static rhy_player_t P;
static void start(const rhy_pattern_t *pt, float bpm) {
    rhy_init(&P);
    P.hit_fn = logit;
    s_nlog = 0;
    rhy_load(&P, pt, 1, 0, bpm, 0, false);
}
static int16_t BUF[256];
static void run(uint64_t samples) { for (uint64_t k = 0; k < samples; k += 256) rhy_fill(&P, BUF, 256); }

/* the ideal: independent of the player, in long double, from the pattern's own numbers */
static long double onset_ld(const rhy_pattern_t *pt, int s) {
    long double o = 0.L;
    for (int q = 0; q < s; q++) o += (long double)(pt->start[q + 1] - pt->start[q]);
    if (pt->swing > 0.f && (s & 1)) o += (long double)pt->swing * (long double)(pt->start[s] - pt->start[s - 1]);
    return o;
}
/* every event in [0, samples) at a constant tempo, counted as the log counts them; compares */
static void check_timing(const char *what, const rhy_pattern_t *pt, float bpm, uint64_t samples) {
    start(pt, bpm);
    run(samples);
    const long double du = (long double)bpm * pt->per / 60.L / RHY_RATE, L = onset_ld(pt, pt->steps) + 0.L;
    long double Lb = 0.L; for (int q = 0; q < pt->steps; q++) Lb += (long double)(pt->start[q + 1] - pt->start[q]);
    (void)L;
    size_t k = 0, bad = 0, n = 0;
    long double worst = 0.L;
    for (int64_t b = -pt->count_in; ; b++) {
        bool past = false;
        for (int s = 0; s < pt->steps && !past; s++) {
            const long double u = (long double)(b + pt->count_in) * Lb + onset_ld(pt, s), t = u / du;   /* samples from the start */
            if (t >= (long double)P.n - 1) { past = true; break; }
            const bool cin = b < 0;
            for (int l = cin ? -1 : 0; l < (cin ? 0 : pt->nlayers); l++) {
                if (cin ? !(pt->beats >> s & 1) : !(rhy_bar_hits(pt, (int)b, l) >> s & 1)) continue;
                n++;
                if (k >= s_nlog || s_log[k].layer != l || s_log[k].step != s) { bad++; k++; continue; }
                const long double err = (long double)s_log[k].n - t;
                if (err < -1e-5L || err >= 1.L) bad++;
                if (fabsl(err) > worst) worst = fabsl(err);
                k++;
            }
        }
        if (past) break;
    }
    CHECK(!bad && k == s_nlog && n > 100, "%s: %zu of %zu hits off (log %zu)", what, bad, n, s_nlog);
    printf("  timing  %-34s %6zu hits in %.0f s, worst %.3f samples from the ideal\n", what, n, (double)samples / RHY_RATE, (double)worst);
}

static rhy_pattern_t make(int steps, float per, const char *hits, float swing, const float *lengths) {
    rhy_pattern_t pt = { 0 };
    strcpy(pt.id, "t"); strcpy(pt.label, "T");
    pt.steps = (uint8_t)steps; pt.per = per; pt.swing = swing; pt.count_in = 1; pt.nlayers = 1;
    for (int s = 0; s < steps; s++) pt.start[s + 1] = pt.start[s] + (lengths ? lengths[s] : 1.f);
    pt.beats = 1;
    for (int s = 0; hits[s]; s++) if (hits[s] == 'x') pt.layer[0].hits |= 1u << s;
    pt.layer[0].sound = RHY_RIM;
    return pt;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: rhythm_test <library.json>\n"); return 2; }
    /* ── the skill's library, parsed as Molty parses it ── */
    FILE *f = fopen(argv[1], "rb");
    static char text[65536];
    const size_t got = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
    if (f) fclose(f);
    text[got] = 0;
    cJSON *lib = cJSON_Parse(text);
    const char *err = NULL;
    static rhy_region_t REG[RHY_REGIONS];
    const int nreg = rhy_parse_regions(cJSON_GetObjectItem(lib, "regions"), REG, &err);
    NLIB = nreg < 1 ? -1 : rhy_parse_lib(cJSON_GetObjectItem(lib, "rhythms"), nreg, LIB, &err);
    CHECK(nreg >= 10 && NLIB >= 50, "the library parses: %d rhythms in %d regions (%s)", NLIB, nreg, err ? err : "");
    int bad_region = 0;
    for (int i = 0; i < NLIB; i++) bad_region += LIB[i].region >= nreg || (i && LIB[i].region < LIB[i - 1].region);
    CHECK(!bad_region, "each rhythm in a region, the regions in order (%d not)", bad_region);
    printf("  library %d rhythms, %zu bytes\n", NLIB, got);
    const int cha = lib_index("chacarera"), aks = lib_index("aksak-9"), son = lib_index("son-clave"), bell = lib_index("bell-12");
    CHECK(cha >= 0 && aks >= 0 && son >= 0 && bell >= 0, "the library has chacarera, aksak-9, son-clave, bell-12");
    if (cha < 0 || aks < 0 || son < 0 || bell < 0) return 1;
    CHECK(LIB[cha].norder == 4 && LIB[cha].nbars == 2 && LIB[cha].layer[1].accent == 1u << 4, "chacarera: A A B A, parche accent on 5");
    CHECK(fabsf(LIB[aks].start[4] - 9.f) < 1e-6f && fabsf(LIB[aks].start[3] - 6.f) < 1e-6f, "aksak: 2+2+2+3");

    /* ── refused ── */
    static const char *const BAD[] = {
        "[]", "{}", "[{\"id\":\"x\",\"label\":\"X\",\"steps\":4,\"per\":1,\"beats\":[1],\"layers\":[{\"id\":\"a\",\"sound\":\"rim\",\"color\":\"#ffffff\",\"hits\":[5]}]}]",
        "[{\"id\":\"x\",\"label\":\"X\",\"steps\":4,\"per\":1,\"beats\":[1],\"layers\":[{\"id\":\"a\",\"sound\":\"gong\",\"color\":\"#ffffff\",\"hits\":[1]}]}]",
        "[{\"id\":\"x\",\"label\":\"X\",\"steps\":4,\"per\":1,\"beats\":[1],\"layers\":[{\"id\":\"a\",\"sound\":\"rim\",\"color\":\"#ffffff\",\"hits\":[1],\"accent\":[2]}]}]",
        "[{\"id\":\"x\",\"label\":\"X\",\"steps\":4,\"per\":1,\"beats\":[1],\"layers\":[{\"id\":\"a\",\"sound\":\"rim\",\"color\":\"#ffffff\",\"hits\":[1]}],\"bars\":[{\"name\":\"A\",\"hits\":[[1]]}]}]",
        "[{\"id\":\"x\",\"label\":\"X\",\"steps\":33,\"per\":1,\"beats\":[1],\"layers\":[{\"id\":\"a\",\"sound\":\"rim\",\"color\":\"#ffffff\",\"hits\":[1]}]}]",
        "[{\"id\":\"x\",\"label\":\"X\",\"steps\":4,\"per\":1,\"beats\":[1],\"tempo\":1,\"layers\":[{\"id\":\"a\",\"sound\":\"rim\",\"color\":\"#ffffff\",\"hits\":[1]}]}]",
    };
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        cJSON *j = cJSON_Parse(BAD[i]);
        static rhy_pattern_t tmp[RHY_LIB];
        CHECK(rhy_parse_lib(j, 1, tmp, &err) < 0 && err, "bad library %zu refused", i);
        cJSON_Delete(j);
    }
    uint32_t m = 0;
    cJSON *ids = cJSON_Parse("[\"aro\"]"), *wrong = cJSON_Parse("[\"bell\"]");
    CHECK(rhy_parse_mute(&LIB[cha], ids, &m) && m == 1u, "mute aro: layer 0");
    CHECK(!rhy_parse_mute(&LIB[cha], wrong, &m), "mute an unknown drum: refused");
    cJSON_Delete(ids); cJSON_Delete(wrong);

    /* ── timing: five minutes, several tempi, uneven steps, swing ── */
    const uint64_t FIVE = 5ull * 60 * RHY_RATE;
    static const float L5[5] = { 1.f, 1.5f, 1.f, 0.75f, 1.25f };
    const rhy_pattern_t odd = make(5, 1.3f, "xx.xx", 0.f, L5), swung = make(8, 2.f, "xxxxxxxx", 0.33f, NULL), lilt = make(12, 3.f, "x.xx.xx.x.xx", 0.1f, NULL);
    check_timing("chacarera at 104", &LIB[cha], 104.f, FIVE);
    check_timing("chacarera at 40", &LIB[cha], 40.f, FIVE);
    check_timing("chacarera at 220", &LIB[cha], 220.f, FIVE);
    check_timing("son clave at 97", &LIB[son], 97.f, FIVE);
    check_timing("bell 12/8 at 133", &LIB[bell], 133.f, FIVE);
    check_timing("aksak 9/8 (2+2+2+3) at 137", &LIB[aks], 137.f, FIVE);
    check_timing("uneven 1 1.5 1 0.75 1.25 at 71.3", &odd, 71.3f, FIVE);
    check_timing("shuffle (swing 0.33) at 119", &swung, 119.f, FIVE);
    check_timing("lilt (swing 0.1) at 188", &lilt, 188.f, FIVE);

    /* ── a tempo change: from the sample it lands on, the new tempo; nothing restarts, the sound is untouched ── */
    {
        const rhy_pattern_t *pt = &LIB[cha];
        start(pt, 100.f);
        run(256 * 300);                                     /* 4.8 s: into the second bar of the phrase */
        const uint64_t X = P.n;
        const double uX = rhy_u_at(&P, X);
        const size_t before = s_nlog;
        rhy_player_t A = P;                                 /* the same player, without the change */
        A.hit_fn = NULL;
        rhy_set_bpm(&P, 137.f);
        static int16_t a[16000], b[16000];
        rhy_fill(&A, a, 16000);
        const size_t na = s_nlog; (void)na;
        rhy_fill(&P, b, 16000);
        CHECK(fabs(rhy_u_at(&P, X) - uX) < 1e-9, "the clock does not jump at a tempo change");
        /* every hit after it: at u's ideal under the new tempo */
        const long double du2 = 137.L * pt->per / 60.L / RHY_RATE;
        size_t bad = 0, after = 0;
        for (size_t k = before; k < s_nlog; k++) {
            /* this hit's u: the bar it is in, from its sample under the new tempo, then its onset */
            const long double u = (long double)uX + ((long double)s_log[k].n - (long double)X) * du2;
            const long double L = pt->start[pt->steps], bar = floorl((u + 1e-6L) / L), ue = bar * L + pt->start[s_log[k].step];
            const long double ideal = (long double)X + (ue - (long double)uX) / du2;
            if ((long double)s_log[k].n - ideal < -1e-5L || (long double)s_log[k].n - ideal >= 1.L) { bad++; printf("    off by %.4f\n", (double)((long double)s_log[k].n - ideal)); }
            after++;
        }
        CHECK(after > 3 && !bad, "after the change: %zu of %zu hits off", bad, after);
        size_t same = 0;
        while (same < 16000 && a[same] == b[same]) same++;
        const uint64_t next_hit = after ? s_log[before].n - X : 16000;
        CHECK(same >= next_hit, "the sound is untouched until the next hit (%zu same samples, next hit at %llu)", same, (unsigned long long)next_hit);
        printf("  tempo   100 -> 137 at %.2f s: no jump, %zu hits on the new grid, sound unchanged for %zu samples\n", (double)X / RHY_RATE, after, same);
    }

    /* ── mixing: never a clipped sample, at the slowest and the fastest, every rhythm and a worst case ── */
    {
        int peak = 0;
        size_t flat = 0;
        for (int i = 0; i < NLIB; i++) for (int t = 0; t < 3; t++) {
            static const float T[3] = { 40.f, 120.f, 220.f };
            rhy_init(&P);
            rhy_load(&P, LIB, NLIB, i, T[t], 0, false);
            for (int k = 0; k < 20 * RHY_RATE / 256; k++) {
                rhy_fill(&P, BUF, 256);
                for (int q = 0; q < 256; q++) { const int a = abs(BUF[q]); if (a > peak) peak = a; if (a >= 32000) flat++; }
            }
        }
        rhy_pattern_t worst = make(4, 1.f, "xxxx", 0.f, NULL);
        worst.nlayers = 4;
        const int snd[4] = { RHY_LOW, RHY_HEAD, RHY_CLAP, RHY_BELL };
        for (int l = 0; l < 4; l++) { worst.layer[l] = worst.layer[0]; worst.layer[l].sound = (uint8_t)snd[l]; worst.layer[l].accent = worst.layer[l].hits = 15; }
        int wpeak = 0;
        rhy_init(&P); rhy_load(&P, &worst, 1, 0, 220.f, 0, false);
        for (int k = 0; k < 20 * RHY_RATE / 256; k++) { rhy_fill(&P, BUF, 256); for (int q = 0; q < 256; q++) { const int a = abs(BUF[q]); if (a > wpeak) wpeak = a; if (a >= 32000) flat++; } }
        CHECK(peak < 32000 && wpeak < 32000 && !flat, "no clipped sample (library peak %d, worst case %d)", peak, wpeak);
        printf("  mix     the library at 40/120/220: peak %.1f dBFS; four accented drums at once, 220 bpm: %.1f dBFS; 0 clipped\n",
               20.0 * log10(peak / 32768.0), 20.0 * log10(wpeak / 32768.0));
    }

    /* ── mute: the drum fades out in RHY_FADE samples, no step in the wave ── */
    {
        rhy_pattern_t one = make(4, 1.f, "x...", 0.f, NULL);
        one.layer[0].sound = RHY_BELL; one.count_in = 0;
        rhy_init(&P); rhy_load(&P, &one, 1, 0, 60.f, 0, false);
        static int16_t w[4000];
        rhy_fill(&P, w, 1600);                              /* 100 ms into the bell's ring */
        int dmax = 0;
        for (int q = 1; q < 1600; q++) if (abs(w[q] - w[q - 1]) > dmax) dmax = abs(w[q] - w[q - 1]);
        rhy_set_mute(&P, 1);
        rhy_fill(&P, w, 4000);
        int jump = 0, after = 0;
        for (int q = 1; q < RHY_FADE + 2; q++) if (abs(w[q] - w[q - 1]) > jump) jump = abs(w[q] - w[q - 1]);
        for (int q = RHY_FADE + 1; q < 4000; q++) if (w[q]) after++;
        CHECK(jump <= dmax && after == 0, "mute: largest step %d (the ring's own %d), %d samples after the fade", jump, dmax, after);
        printf("  mute    a ringing bell: gone in %d samples, its largest step %d against %d before\n", RHY_FADE, jump, dmax);
    }

    /* ── pause and resume: the fade, silence, then a count-in and the bar it paused in ── */
    {
        start(&LIB[cha], 104.f);
        run(256 * 200);                                     /* in bar 1 */
        const double u = rhy_u_at(&P, P.n), L = LIB[cha].start[6];
        rhy_pause(&P, true);
        size_t made = rhy_fill(&P, BUF, 256), more = 0;
        while (made == 256) made = rhy_fill(&P, BUF, 256), more++;
        CHECK(made > 0 && more < 3 && rhy_fill(&P, BUF, 256) == 0 && !rhy_sounding(&P), "pause: a short fade, then nothing (%zu)", made);
        rhy_view_t v; rhy_view(&P, P.n, &v);
        CHECK(v.on && v.paused, "paused: still on");
        const size_t k0 = s_nlog;
        rhy_pause(&P, false);
        run(256 * 120);
        CHECK(s_nlog > k0 && s_log[k0].layer == -1, "resume: a count-in click first");
        size_t k = k0; while (k < s_nlog && s_log[k].layer == -1) k++;
        const double ufirst = rhy_u_at(&P, s_log[k].n);
        CHECK(k < s_nlog && fabs(ufirst - floor(u / L) * L - LIB[cha].start[s_log[k].step]) < 0.01, "resume: the bar it paused in, from its start");
    }

    /* ── count-in, phrase and next ── */
    {
        start(&LIB[cha], 104.f);
        run(RHY_RATE * 20);
        int clicks = 0; while (s_log[clicks].layer == -1) clicks++;
        CHECK(clicks == 2 && s_log[0].acc && !s_log[1].acc, "count-in: two beats, the first high (%d)", clicks);
        /* bar slot 2 is B: no aro */
        const double L = LIB[cha].start[6];
        int aro_in_b = 0, aro_in_a = 0;
        for (size_t k = 0; k < s_nlog; k++) {
            if (s_log[k].layer != 0) continue;
            const int bar = (int)floor(rhy_u_at(&P, s_log[k].n) / L + 1e-6);
            if (bar % 4 == 2) aro_in_b++; else aro_in_a++;
        }
        CHECK(!aro_in_b && aro_in_a > 10, "phrase A A B A: bar B has no aro (%d, %d)", aro_in_b, aro_in_a);
        rhy_next(&P);
        CHECK(P.next_req, "next: waits for the bar line");
        run(RHY_RATE * 2);
        CHECK(!P.next_req && P.shift == 1, "next: the phrase moved one bar on");
    }

    /* ── the view follows the sample clock ── */
    {
        start(&LIB[aks], 100.f);
        run(RHY_RATE * 10);
        int bad = 0;
        for (uint64_t n = RHY_RATE * 3; n < (uint64_t)RHY_RATE * 9; n += 97) {
            rhy_view_t v; rhy_view(&P, n, &v);
            const double u = rhy_u_at(&P, n), um = u - floor(u / 9.0) * 9.0;
            const int want = um < 2 ? 0 : um < 4 ? 1 : um < 6 ? 2 : 3;
            if (v.count_in || v.step != want || v.since < 0.f) bad++;
        }
        CHECK(!bad, "view: the step at each sample, aksak's uneven steps (%d off)", bad);
        rhy_view_t v; rhy_view(&P, RHY_RATE / 2, &v);
        CHECK(v.count_in && v.step == -1, "view: the count-in");
    }

    /* ── pick, stop, take_beat ── */
    {
        rhy_init(&P);
        rhy_load(&P, LIB, NLIB, cha, 104.f, 0, false);
        run(RHY_RATE * 4);
        bool first = false; int beats = 0, firsts = 0;
        while (rhy_take_beat(&P, P.n, &first)) { beats++; firsts += first; }
        CHECK(beats >= 1 && beats <= RHY_BEATS && firsts >= 1, "beats to buzz on: %d, %d first", beats, firsts);
        CHECK(rhy_pick(&P, son) && P.cur == son, "pick: another rhythm");
        rhy_view_t v; rhy_view(&P, P.n, &v);
        CHECK(v.count_in, "pick: a count-in first");
        /* at rest: silent, back at the phrase's start; a pick at rest stays silent; played again: a count-in, bar 1 */
        rhy_init(&P);
        rhy_load(&P, LIB, NLIB, cha, 200.f, 0, false);
        run(RHY_RATE * 5);
        rhy_rest(&P);
        run(RHY_RATE);
        rhy_view(&P, P.n, &v);
        CHECK(P.paused && !rhy_sounding(&P) && v.count_in && v.slot == 0, "rest: silent, at the phrase's start");
        CHECK(rhy_pick(&P, son) && P.paused && !rhy_sounding(&P), "rest: a pick stays silent");
        rhy_pause(&P, false);
        rhy_view(&P, P.n, &v);
        CHECK(rhy_sounding(&P) && v.count_in, "played again: a count-in first");
        run(RHY_RATE * 4);
        rhy_view(&P, P.n, &v);
        CHECK(!v.count_in && v.slot == 0, "... then its first bars");
        rhy_init(&P);
        rhy_load(&P, LIB, NLIB, cha, 104.f, 0, false);
        run(RHY_RATE * 2);
        rhy_stop(&P);
        size_t made = 256; int blocks = 0;
        while ((made = rhy_fill(&P, BUF, 256)) == 256 && blocks < 10) blocks++;
        CHECK(!P.on && blocks < 2, "stop: fades, then off");
    }

    /* ── the speaker's clock as the picture reads it: an I2S ring of 6 x 240 frames (the default config, both boards),
     * 256-sample writes that wait for room, the DAC 50 ppm off the timer; the picture reads at 30 frames a second.
     * rhythm_dev.c's measure (written less the ring, plus half a buffer, plus the time since the write) smoothed by
     * rhy_clock_heard against the sample truly heard: no offset, and the same after ten minutes ── */
    {
        const double rate = RHY_RATE * (1.0 + 50e-6);  /* the DAC's real rate, timer us */
        const double desc = 240.0, ring = 1440.0;
        rhy_clock_t clk = { 0 };
        double t = 0.0, freed = 0.0, written = 0.0, w_ret = 0.0, tw = 0.0, next_frame = 0.0;   /* w_ret: as of the last write's return */
          /* freed: room handed back, frames (the ring starts full of silence) */
        double e_first = 0.0, e_last = 0.0, worst = 0.0; int n_first = 0, n_last = 0;
        const double T = 600.0 * 1e6;
        while (t < T) {
            /* the writer: a 256-block, copied as buffers free (each frees when the DMA is done with it) */
            double left = 256.0;
            while (left > 0.0) {
                const double room = freed - written;
                if (room <= 0.0) {                          /* wait for the next buffer to finish */
                    const double done = floor(t * rate / 1e6 / desc + 1e-9) + 1.0;   /* buffers played by then */
                    const double at = done * desc / rate * 1e6;
                    while (next_frame < at) {               /* the picture's frames meanwhile */
                        const double meas = w_ret - ring + desc / 2.0 + (next_frame - tw) * (RHY_RATE / 1e6);
                        const double h = rhy_clock_heard(&clk, meas, (int64_t)next_frame);
                        const double truth = next_frame * rate / 1e6 - ring;   /* (the ring's first silence ahead of it) */
                        if (next_frame > 2e6) {
                            const double e = (h - truth) / RHY_RATE * 1e3;   /* ms */
                            if (fabs(e) > worst) worst = fabs(e);
                            if (next_frame < 62e6) { e_first += e; n_first++; }
                            if (next_frame > T - 60e6) { e_last += e; n_last++; }
                        }
                        next_frame += 1e6 / 30.0;
                    }
                    t = at; freed = done * desc;
                    continue;
                }
                const double k = room < left ? room : left;
                written += k; left -= k;
            }
            tw = t; w_ret = written;
        }
        e_first /= n_first; e_last /= n_last;
        CHECK(fabs(e_first) < 1.5 && fabs(e_last) < 1.5 && fabs(e_last - e_first) < 0.2 && worst < 4.0,
              "the picture's clock: %.2f ms off in the first minute, %.2f ms after ten, worst %.2f ms", e_first, e_last, worst);
        printf("  clock   the speaker's clock read at 30 fps, the DAC 50 ppm off the timer: %.2f ms off at first, %.2f ms after ten minutes, worst %.2f ms\n", e_first, e_last, worst);
    }

    /* ── cost: the busiest rhythm, a minute ── */
    {
        rhy_init(&P);
        rhy_load(&P, LIB, NLIB, bell, 220.f, 0, false);
        const clock_t c0 = clock();
        for (int k = 0; k < 60 * RHY_RATE / 256; k++) rhy_fill(&P, BUF, 256);
        const double s = (double)(clock() - c0) / CLOCKS_PER_SEC;
        printf("  cost    a minute of the 12/8 bell at 220 on this computer: %.1f ms (%.2f us a 16 ms block)\n", s * 1e3, s * 1e6 / (60.0 * RHY_RATE / 256));
        printf("  memory  the player: %zu bytes\n", sizeof(rhy_player_t));
    }
    cJSON_Delete(lib);
    printf("rhythm_test: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
