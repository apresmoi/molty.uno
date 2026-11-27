/*
 * lab_test.c: the frame rate lab (main/diag/fps_lab.c, FPS-LAB.md): its knobs; the frame records' summary (percentiles,
 * the intervals in pulses, the tracked item's moves, completed positions and longest hold) on frames made up to know the
 * answer; the poses' summary (repeats, distinct, the most seen, a simulated cache); and the creature's reuse
 * (creature_dots.c cd_reuse) on the cat: the same cells, colours and creature mask as the raster every frame, over its
 * idle life and two gestures (three creatures in step: none, skip, cache), and over poses forced to repeat (a second
 * clip a few frames behind the first, drawn into the same cache). Prints the host's costs (ratios only).
 * Build + run: test/host/lab_build.sh
 */
#include "fps_lab.h"
#include "creature_pkg.h"
#include "creature_clip.h"
#include "creature_dots.h"
#include "cJSON.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int s_pass, s_fail;
#define CHECK(c, ...) do { if (c) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static double now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }

static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); const long L = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)L + 1);
    if (fread(b, 1, (size_t)L, f) != (size_t)L) { free(b); fclose(f); return NULL; }
    b[L] = 0; fclose(f); *n = (size_t)L;
    return b;
}

static void test_knobs(void) {
    CHECK(!fps_lab_nondefault(), "boot: every knob at its default");
    CHECK(fps_lab(FL_REC) == 0 && fps_lab(FL_TRACK) == -1 && fps_lab(FL_SKIP) == 0 && fps_lab(FL_CACHE) == 0 && fps_lab(FL_CACHE_N) == 16, "defaults");
    CHECK(fps_lab_set("skip", 1) == 0 && fps_lab(FL_SKIP) == 1 && fps_lab_nondefault(), "set skip");
    CHECK(fps_lab_set("nope", 1) == -1, "an unknown knob is refused");
    CHECK(fps_lab_set("cache_n", 0) == -2 && fps_lab_set("cache_n", 129) == -2 && fps_lab(FL_CACHE_N) == 16, "out of range is refused, the value kept");
    CHECK(fps_lab_set("track", -2) == -2 && fps_lab_set("track", 3) == 0 && fps_lab(FL_TRACK) == 3, "track");
    fps_lab_reset();
    CHECK(!fps_lab_nondefault() && fps_lab(FL_TRACK) == -1 && fps_lab(FL_SKIP) == 0, "reset");
    for (int k = 0; k < FL_N; k++) CHECK(fps_lab(k) == fps_lab_default(k), "knob %s at its default", fps_lab_name(k));
}

/* one made-up frame: the sim's side, then the drawer's */
static void frame(uint32_t te, int sim, const fluid_shape_t *sh, int nsh, uint32_t wins, uint32_t owed) {
    const int c1[4] = { sim, 100, 50, 0 };
    fluid_draw_opts_t o; memset(&o, 0, sizeof(o));
    o.shapes = sh; o.nshapes = nsh;
    fps_lab_handoff(c1, 2000, 300, te, &o, 0, false);
    const fl_drawn_t d = { 9000, 4000, 500, wins, wins * 100, wins ? 12 : 0, owed, te, wins > 0, false };
    fps_lab_drawn(&d);
}

static void test_frames(void) {
    fluid_shape_t sh[2]; memset(sh, 0, sizeof(sh));
    sh[0].k = FLUID_SHAPE_SEG; sh[1].k = FLUID_SHAPE_DISC; sh[1].x = 7.f;
    fl_sum_t s;
    frame(2, 1, sh, 2, 3, 0);                       /* no ring yet: nothing kept */
    CHECK(!fps_lab_summary(&s) && fps_lab_bytes() == 0, "rec off: no ring");
    fps_lab_set("rec", 1); fps_lab_reconcile();
    CHECK(fps_lab_bytes() > 0, "rec on: a ring");
    /* 200 frames: the item moves every frame but 101..109 (held where it was at 100); every handoff two pulses on but f=60
     * (three: late); f=50 owes cells, f=150 sends nothing; sim (f % 100) + 1 us */
    uint32_t te = 100;
    for (int f = 0; f < 200; f++) {
        te += f == 0 ? 0 : f == 60 ? 3 : 2;
        sh[0].x = (float)(f < 100 ? f : f < 110 ? 100 : f);
        frame(te, f % 100 + 1, sh, 2, f == 150 ? 0 : 3, f == 50 ? 5 : 0);
    }
    CHECK(fps_lab_summary(&s), "summary");
    printf("  frames: n=%d sim p50/p95/p99/max %u/%u/%u/%u mean %.2f wake %u/%u/%u/%u pres %u/%u/%u/%u late %u/%u rep %u moves %u done %u hold %u\n",
           s.n, (unsigned)s.m[FM_SIM].p50, (unsigned)s.m[FM_SIM].p95, (unsigned)s.m[FM_SIM].p99, (unsigned)s.m[FM_SIM].max, s.m[FM_SIM].mean,
           (unsigned)s.wake[0], (unsigned)s.wake[1], (unsigned)s.wake[2], (unsigned)s.wake[3], (unsigned)s.pres[0], (unsigned)s.pres[1], (unsigned)s.pres[2], (unsigned)s.pres[3],
           (unsigned)s.late, (unsigned)s.late_pres, (unsigned)s.repeats, (unsigned)s.moves, (unsigned)s.done, (unsigned)s.hold);
    CHECK(s.n == 200 && s.kind == FL_SCENE, "200 frames of a scene (%d)", s.n);
    CHECK(s.m[FM_SIM].p50 == 50 && s.m[FM_SIM].p95 == 95 && s.m[FM_SIM].p99 == 99 && s.m[FM_SIM].max == 100 && fabsf(s.m[FM_SIM].mean - 50.5f) < 1e-3f, "sim percentiles");
    CHECK(s.m[FM_DUST].p50 == 150 && s.m[FM_PACE].max == 2000 && s.m[FM_CELLS].p50 == 9000 && s.m[FM_OWED].max == 5, "the other stages");
    CHECK(s.wake[0] == 0 && s.wake[1] == 198 && s.wake[2] == 1 && s.wake[3] == 0 && s.late == 1, "wake intervals in pulses, one late");
    CHECK(s.pres[1] == 196 && s.pres[2] == 1 && s.pres[3] == 1 && s.late_pres == 2 && s.repeats == 1, "first windows' intervals, one repeat");
    CHECK(s.moves == 190, "moved in 190 handoffs (%u)", (unsigned)s.moves);
    CHECK(s.done == 188, "188 distinct positions completed (%u)", (unsigned)s.done);
    CHECK(s.hold == 20, "the longest hold: 10 frames of 2 pulses (%u)", (unsigned)s.hold);
    static char csv[4096];
    const int n = fps_lab_csv(csv, sizeof(csv), 5);
    int lines = 0; for (int i = 0; i < n; i++) lines += csv[i] == '\n';
    CHECK(lines == 6 && !strncmp(csv, "seq,t_us,te_wake", 16), "csv: a header and 5 frames (%d lines)", lines);
    /* the tracked item one shape (the still one): it never moves */
    fps_lab_set("rec", 0); fps_lab_reconcile();
    CHECK(!fps_lab_summary(&s) && fps_lab_bytes() == 0, "rec off again: the ring freed");
    fps_lab_set("rec", 1); fps_lab_set("track", 1); fps_lab_reconcile();
    for (int f = 0; f < 20; f++) { te += 2; sh[0].x = (float)f; frame(te, 5, sh, 2, 3, 0); }
    CHECK(fps_lab_summary(&s) && s.n == 20 && s.moves == 0 && s.done == 0 && s.hold == 38, "track 1: held all along (%u %u %u)", (unsigned)s.moves, (unsigned)s.done, (unsigned)s.hold);
    CHECK(!strcmp(fps_lab_shapes(), "sd"), "the shapes by kind (%s)", fps_lab_shapes());
    {   /* the lab's own cost a frame, a scene's worth: 64 shapes and 2,124 dots signed, a record each side */
        static fluid_shape_t many[64]; static uint8_t dots[59 * 36];
        fluid_draw_opts_t o; memset(&o, 0, sizeof(o)); o.shapes = many; o.nshapes = 64; o.dots = dots;
        fps_lab_set("track", -1);
        const int c1[4] = { 1, 2, 3, 4 }; const fl_drawn_t dr = { 1, 2, 3, 4, 5, 6, 0, 7, true, false };
        const double t0 = now_us();
        for (int f = 0; f < 10000; f++) { many[f % 64].x = (float)f; dots[f % 2124]++; fps_lab_handoff(c1, 1, 1, (uint32_t)f, &o, 59 * 36, false); fps_lab_drawn(&dr); }
        printf("  the lab's own cost: host %.2f us a frame (64 shapes, 2,124 dots)\n", (now_us() - t0) / 10000);
    }
    fps_lab_reset(); fps_lab_reconcile();
    CHECK(fps_lab_bytes() == 0, "reset: no rings");
}

static void test_poses(void) {
    fps_lab_set("pose", 1); fps_lab_reconcile();
    /* idle: 10 poses in turn, each 3 frames (300 frames); then a gesture of 100 poses never seen again */
    for (int f = 0; f < 400; f++) {
        const uint64_t key = f < 300 ? (uint64_t)(100 + (f / 3) % 10) << 8 : (uint64_t)(1000 + f) << 8;
        fps_lab_pose(key, f >= 300, (uint32_t)(f * 33));
    }
    fl_pose_sum_t p;
    CHECK(fps_lab_pose_summary(&p), "pose summary");
    printf("  poses: n=%d ng=%d same=%d/%d distinct %d (%d idle, %d gesture) top %u lru %.3f %.3f %.3f %.3f\n", p.n, p.ng, p.same, p.same_g,
           p.distinct, p.distinct_i, p.distinct_g, (unsigned)p.top[0].n, p.lru[0], p.lru[1], p.lru[2], p.lru[3]);
    CHECK(p.n == 400 && p.ng == 100 && p.same == 200 && p.same_g == 0, "repeats of the frame before");
    CHECK(p.distinct == 110 && p.distinct_i == 10 && p.distinct_g == 100, "distinct poses");
    CHECK(p.ntop == FL_TOP && p.top[0].n == 30 && !p.top[0].g && p.top[FL_TOP - 1].n == 30, "the most seen: each idle pose 30 times");
    CHECK(fabsf(p.lru[0] - 0.5f) < 1e-4f && fabsf(p.lru[1] - 0.5f) < 1e-4f && fabsf(p.lru[2] - 0.725f) < 1e-4f && fabsf(p.lru[3] - 0.725f) < 1e-4f, "a cache of 1, 4, 16, 64");
    CHECK(p.total == 400 && p.total_same == 200, "since it came on");
    /* the window is the last minute */
    for (int f = 400; f < 2400; f++) fps_lab_pose((uint64_t)7 << 8, false, (uint32_t)(f * 33));
    CHECK(fps_lab_pose_summary(&p) && p.span_s <= 60.f && p.n == 1819 && p.distinct == 1, "a minute's window (%d frames, %.1f s)", p.n, p.span_s);
    fps_lab_reset(); fps_lab_reconcile();
}

/* ── the creature's reuse ─────────────────────────────────────────────── */

typedef struct { cpk_t *k; double us; const char *name; } who_t;
static bool same_cells(const uint8_t *da, const uint32_t *ca, const uint8_t *ma, const uint8_t *db, const uint32_t *cb, const uint8_t *mb, int N) {
    for (int q = 0; q < N; q++) if (da[q] != db[q] || ca[q] != cb[q] || ma[q] != mb[q]) return false;
    return true;
}

/* three cats in step (one seed, the same calls): the raster, skip, a cache of 32; idle 20 s, nod, idle, stretch, idle */
static void test_life(const char *json, size_t len) {
    const char *err = NULL;
    who_t w[3] = { { cpk_create(json, len, 4242, &err), 0, "raster" }, { cpk_create(json, len, 4242, &err), 0, "skip" }, { cpk_create(json, len, 4242, &err), 0, "cache 32" } };
    CHECK(w[0].k && w[1].k && w[2].k, "cat: create (%s)", err ? err : "");
    if (!w[0].k || !w[1].k || !w[2].k) return;
    cpk_reuse(w[0].k, false, false, 0); cpk_reuse(w[1].k, true, true, 0); cpk_reuse(w[2].k, true, false, 32);
    const int W = 59, H = 36;
    fluid_sim_t *s = fluid_sim_create(W, H, FLUID_STAGE_REF);
    fps_lab_set("pose", 1); fps_lab_reconcile();
    int bad = 0, frames = 0, gframes = 0, started = 0;
    for (int f = 0; f < 1500; f++) {
        const char *g = started == 0 && f >= 600 ? "nod" : started == 1 && f >= 900 ? "stretch" : NULL;   /* (each when it is free) */
        bool go = false;
        for (int r = 0; r < 3; r++) {   /* (in turn first: the host's cache favours none) */
            const int i = (f + r) % 3;
            if (g) go = cpk_gesture(w[i].k, g);
            cpk_set_idle(w[i].k, f / 30.f, 1.f);
            const double t0 = now_us();
            cpk_frame(w[i].k, s, 1.f / 30.f, 1.f);
            w[i].us += now_us() - t0;
        }
        started += go;
        const uint8_t *d[3], *m[3]; const uint32_t *c[3];
        for (int i = 0; i < 3; i++) { cpk_cells(w[i].k, &d[i], &c[i]); m[i] = cpk_cells_creature(w[i].k); }
        bad += !same_cells(d[0], c[0], m[0], d[1], c[1], m[1], W * H) || !same_cells(d[0], c[0], m[0], d[2], c[2], m[2], W * H);
        const cd_reuse_t *st = cpk_reuse_stats(w[1].k);
        fps_lab_pose(st->key, st->gesture, (uint32_t)(f * 1000 / 30));
        frames++; gframes += st->gesture;
    }
    CHECK(started == 2, "both gestures played (%d)", started);
    CHECK(bad == 0, "the idle life and two gestures: %d of %d frames differ from the raster", bad, frames);
    const cd_reuse_t *a = cpk_reuse_stats(w[1].k), *b = cpk_reuse_stats(w[2].k);
    fl_pose_sum_t p; fps_lab_pose_summary(&p);
    printf("  cat life, %d frames (%d in a gesture): skip kept %u (%u in gestures), cache hit %u (%u in gestures) of %u, cache %u bytes\n",
           frames, gframes, (unsigned)a->frames[CD_SAME], (unsigned)a->gframes[CD_SAME], (unsigned)b->frames[CD_CACHED], (unsigned)b->gframes[CD_CACHED],
           (unsigned)(b->frames[0] + b->frames[2]), (unsigned)b->bytes);
    printf("  cat life, last minute: same as the frame before %d of %d (%d of %d in gestures); distinct %d (idle %d, gestures %d); most seen %u; an LRU of 1/4/16/64 would hit %.1f/%.1f/%.1f/%.1f%%\n",
           p.same, p.n, p.same_g, p.ng, p.distinct, p.distinct_i, p.distinct_g, (unsigned)p.top[0].n, 100 * p.lru[0], 100 * p.lru[1], 100 * p.lru[2], 100 * p.lru[3]);
    printf("  cat life, host us a frame (cpk_frame): raster %.1f, skip %.1f (x%.3f), cache %.1f (x%.3f)\n", w[0].us / frames, w[1].us / frames, w[1].us / w[0].us, w[2].us / frames, w[2].us / w[0].us);
    CHECK(b->bytes == 32u * (uint32_t)(sizeof(void *) * 4 + 16 + 12 + ((7 * W * H + 3) & ~3)), "the cache's bytes (%u)", (unsigned)b->bytes);
    cpk_reuse(w[2].k, false, false, 0); cpk_frame(w[2].k, s, 1.f / 30.f, 1.f);
    CHECK(cpk_reuse_stats(w[2].k)->bytes == 0, "the cache freed when its knob goes back");
    fps_lab_reset(); fps_lab_reconcile();
    for (int i = 0; i < 3; i++) cpk_destroy(w[i].k);
    fluid_sim_destroy(s);
}

/* poses forced to repeat: clip b plays what clip a played L frames before; both are drawn into one cache (and each into
 * its own raster); a third draws a's every frame twice (skip: the second is the first's cells) */
static void test_forced(const char *json, size_t len) {
    cJSON *j = cJSON_ParseWithLength(json, len);
    const cJSON *cr = cJSON_GetObjectItemCaseSensitive(j, "creature");
    const char *err = NULL;
    cc_t *ca = cc_create(cr, &err), *cb = cc_create(cr, &err);
    CHECK(ca && cb, "cat clips (%s)", err ? err : "");
    if (!ca || !cb) { cJSON_Delete(j); return; }
    cc_seed(ca, 9); cc_seed(cb, 9);
    const int W = 59, H = 36, N = W * H, L = 20, F = 900;
    cd_t *ra = cd_create(ca, W, H), *rb = cd_create(cb, W, H), *cache = cd_create(ca, W, H), *skip = cd_create(ca, W, H), *ra2 = cd_create(ca, W, H);
    cd_reuse(cache, true, false, 64); cd_reuse(skip, true, true, 0); cd_reuse(ra, true, false, 0);
    static const char *const ACT[] = { "nod", "stretch", "yawn", "wiggle", "bounce", "peek" };
    int bad = 0, bad_skip = 0; double us[3] = { 0 }; int n[3] = { 0 };
    for (int st = 0; st < F + L; st++) {
        for (int who = 0; who < 2; who++) {
            const int t = who ? st - L : st;
            if (t < 0 || t >= F) continue;
            cc_t *c = who ? cb : ca;
            if (t % 150 == 75) cc_act(c, ACT[(t / 150) % 6]);
            cc_update(c, 1.f / 30.f);
            cd_t *ref = who ? rb : ra;
            cd_draw(ref, c, (uint32_t)st);
            const double t0 = now_us();
            cd_draw(cache, c, (uint32_t)st);
            const int how = cd_reuse_stats(cache)->last;
            us[how] += now_us() - t0; n[how]++;
            const uint8_t *d0, *d1; const uint32_t *c0, *c1;
            cd_cells(ref, &d0, &c0); cd_cells(cache, &d1, &c1);
            bad += !same_cells(d0, c0, cd_creature(ref), d1, c1, cd_creature(cache), N);
            if (!who) {   /* the skip: a's pose drawn twice, the world a frame on */
                for (int r = 0; r < 2; r++) {
                    cd_draw(ra2, c, (uint32_t)(2 * st + r)); cd_draw(skip, c, (uint32_t)(2 * st + r));
                    cd_cells(ra2, &d0, &c0); cd_cells(skip, &d1, &c1);
                    bad_skip += !same_cells(d0, c0, cd_creature(ra2), d1, c1, cd_creature(skip), N);
                }
            }
        }
    }
    const cd_reuse_t *k = cd_reuse_stats(cache), *sk = cd_reuse_stats(skip);
    CHECK(bad == 0, "forced repeats: %d of %d draws differ from the raster", bad, n[0] + n[2]);
    CHECK(bad_skip == 0, "skip: %d of %d draws differ from the raster", bad_skip, 2 * F);
    CHECK(n[CD_CACHED] >= F * 9 / 10, "forced repeats: the lagging clip's poses hit (%d of %d)", n[CD_CACHED], F);
    CHECK(sk->frames[CD_SAME] == (uint32_t)F, "skip: every second draw kept (%u)", (unsigned)sk->frames[CD_SAME]);
    printf("  forced repeats: %d hits, %d rasters; host us a draw: raster %.1f, cache hit %.1f (x%.3f); skip kept %u of %d\n",
           n[CD_CACHED], n[CD_RASTER], us[0] / n[0], us[2] / (n[2] ? n[2] : 1), (us[2] / (n[2] ? n[2] : 1)) / (us[0] / n[0]), (unsigned)sk->frames[CD_SAME], 2 * F);
    (void)k;
    cd_destroy(ra); cd_destroy(rb); cd_destroy(cache); cd_destroy(skip); cd_destroy(ra2);
    cc_destroy(ca); cc_destroy(cb); cJSON_Delete(j);
}

/* a miss's cost: the key and a raster's cells kept, every frame, with no hit (the cache's poses all new) */
static void test_miss_cost(const char *json, size_t len) {
    cJSON *j = cJSON_ParseWithLength(json, len);
    const char *err = NULL;
    cc_t *c = cc_create(cJSON_GetObjectItemCaseSensitive(j, "creature"), &err);
    if (!c) { cJSON_Delete(j); return; }
    cd_t *off = cd_create(c, 59, 36), *on = cd_create(c, 59, 36);
    cd_reuse(on, true, true, 16);
    double t_off = 0, t_on = 0;
    {   /* the key alone */
        uint64_t x = 0; const double t0 = now_us();
        for (int r = 0; r < 10000; r++) x ^= cd_pose_key(c);
        printf("  the pose key: host %.2f us (%d nodes)%s\n", (now_us() - t0) / 10000, cc_nodes(c), x ? "" : " ");
    }
    for (int f = 0; f < 900; f++) {
        if (f % 150 == 75) cc_act(c, "nod");
        cc_update(c, 1.f / 30.f);
        double t0 = now_us(); cd_draw(off, c, (uint32_t)f); double t1 = now_us(); cd_draw(on, c, (uint32_t)f); t_on += now_us() - t1; t_off += t1 - t0;
    }
    const cd_reuse_t *st = cd_reuse_stats(on);
    printf("  a miss: host us a draw %.1f with skip and a cache of 16 on, %.1f off (x%.3f); %u hits of 900\n", t_on / 900, t_off / 900, t_on / t_off,
           (unsigned)(st->frames[CD_SAME] + st->frames[CD_CACHED]));
    cd_destroy(off); cd_destroy(on); cc_destroy(c); cJSON_Delete(j);
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: lab_test <cat.json>\n"); return 2; }
    size_t len; char *json = slurp(argv[1], &len);
    CHECK(json != NULL, "read %s", argv[1]);
    test_knobs();
    test_frames();
    test_poses();
    if (json) { test_life(json, len); test_forced(json, len); test_miss_cost(json, len); }
    free(json);
    printf("lab_test: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
