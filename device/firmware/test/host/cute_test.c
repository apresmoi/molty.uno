/*
 * cute_test.c — host test for the cute-kit runtime (cute_spec.c + cute_rig.c) driving
 * the fluid (fluid_*.c). For every .molty given on the command line:
 *   - parses + validates it, and rejects a corrupted copy (CRC) and a truncated one;
 *   - builds the creature, checks the rest-pose normalisation (height 1 or bulk-scaled),
 *     the tri budget (< 1000 visible) and that each of the 7 moods rasterises a body
 *     with solid features (eyes) somewhere on the grid;
 *   - runs the fluid toward it and writes a preview contact sheet (all moods) to
 *     <outdir>/<id>_moods.ppm when an outdir is given;
 *   - plays every one-shot action (cute_act.h) over idle: each one moves the creature's
 *     target, ends back where it started, and costs little per frame (timed against idle);
 *     <outdir>/<id>_acts.ppm shows each action at its peak.
 * And once: the actions' curves equal their JS twin's (device/creatures/js/actions.js),
 * sample for sample, as pinned in act_golden.txt (-g <file>); every action starts and ends
 * at a zero offset and never jumps between frames.
 * Build + run: test/host/cute_build.sh [outdir]
 */
#include "cute_rig.h"
#include "cute_act.h"
#include "fluid_sim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int s_fail = 0, s_pass = 0;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const uint8_t *font(char c) {
    static const uint8_t full[7] = { 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f };
    (void)c; return full;
}

static void write_sheet(const char *path, uint16_t **fbs, int n) {
    int cols = 4, rows = (n + cols - 1) / cols, W = FLUID_STAGE_REF * cols, H = FLUID_STAGE_REF * rows;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int k = (y / FLUID_STAGE_REF) * cols + x / FLUID_STAGE_REF;
        uint16_t be = k < n ? fbs[k][(y % FLUID_STAGE_REF) * FLUID_STAGE_REF + x % FLUID_STAGE_REF] : 0, le = (uint16_t)((be >> 8) | (be << 8));
        unsigned char c[3] = { (unsigned char)((le >> 11) << 3), (unsigned char)(((le >> 5) & 63) << 2), (unsigned char)((le & 31) << 3) };
        fwrite(c, 1, 3, f);
    }
    fclose(f);
}

static cute_spec_t S;

static void test_file(const char *path, const char *outdir) {
    FILE *f = fopen(path, "rb");
    if (!f) { CHECK(0, "open %s", path); return; }
    static uint8_t buf[CUTE_MAX_FILE + 16];
    size_t n = fread(buf, 1, sizeof(buf), f); fclose(f);
    const char *err;
    int rc = cute_spec_parse(buf, n, &S, &err);
    CHECK(rc == 0, "%s: parse rc=%d (%s)", path, rc, err);
    if (rc) return;
    /* corruption is rejected */
    buf[n / 2] ^= 0x5a;
    static cute_spec_t S2;
    CHECK(cute_spec_parse(buf, n, &S2, &err) != 0, "%s: corrupted copy accepted", path);
    buf[n / 2] ^= 0x5a;
    CHECK(cute_spec_parse(buf, n - 7, &S2, &err) != 0, "%s: truncated copy accepted", path);

    cute_rt_t *rt = cute_rt_create(&S, 1234);
    CHECK(rt != NULL, "%s: create", path);
    if (!rt) return;
    const int W = 52, H = 32;
    fluid_sim_t *s = fluid_sim_create(W, H, FLUID_STAGE_REF);
    fluid_draw_t *d = fluid_draw_create(W, H, FLUID_STAGE_REF, FLUID_STAGE_REF);
    fluid_frame_t *fr = fluid_frame_create(W, H);
    uint16_t *fbs[CUTE_N_MOODS];
    s->edge_w = 1.f;
    int worst_tris = 0;
    for (int m = 0; m < CUTE_N_MOODS; m++) {
        cute_rt_set_mood(rt, m);
        fbs[m] = calloc(FLUID_STAGE_REF * FLUID_STAGE_REF, 2);
        float cov = 0; int feats = 0;
        for (int fno = 0; fno < 75; fno++) {       /* 2.5 s at 30 fps */
            float t = fno / 30.f;
            s->p.hold = fminf(1.f, t / 1.2f);
            s->p.relax_up = 2.6f; s->p.relax_down = 3.4f; s->p.tint_rate = 3 + 9 * s->p.hold;
            cute_rt_update(rt, 1.f / 30.f);
            cute_rt_raster(rt, W, H);
            cute_rt_fill(rt, s, 1.f);
            fluid_sim_background(s, t, 0.5f, (fno & 1) == 0);
            fluid_sim_swirl_field(s);
            fluid_sim_step(s, 1.f / 60); fluid_sim_step(s, 1.f / 60);
            fluid_sim_color(s, 1.f / 30);
            fluid_sim_view(s);
            fluid_sim_snapshot(s, fr, false);
            fluid_draw_opts_t o = { FLUID_PAL_PASTEL, FLUID_EDGE_ALL, 0.12f, t, 0.f, NULL, 0, " .:-=+*#", font };
            int y0, y1;
            fluid_draw(d, fr, fbs[m], &o, &y0, &y1);
            if (cute_rt_tris(rt) > worst_tris) worst_tris = cute_rt_tris(rt);
        }
        for (int k = 0; k < W * H; k++) { cov += s->T[k] / FQ_UNIT; feats += s->tf[k]; }
        CHECK(cov > 20.f, "%s mood %s: target nearly empty (Σink %.1f)", S.id, CUTE_MOOD_NAMES[m], cov);
        if (m != CM_HAPPY && m != CM_SLEEPY && m != CM_SURPRISED)   /* arcs ^ ^ ‿ ‿; O O is a white sclera ring */
            CHECK(feats > 0, "%s mood %s: no solid features (eyes)", S.id, CUTE_MOOD_NAMES[m]);
        printf("  %-14s %-10s Σtarget %6.1f  feature cells %3d  tris %d\n", S.id, CUTE_MOOD_NAMES[m], cov, feats, cute_rt_tris(rt));
    }
    CHECK(worst_tris > 50 && worst_tris < 1000, "%s: tris %d outside (50, 1000)", S.id, worst_tris);
    if (outdir) {
        char p[512]; const char *id = strncmp(S.id, "cute-", 5) == 0 ? S.id + 5 : S.id;
        snprintf(p, sizeof(p), "%s/%s_moods.ppm", outdir, id);
        write_sheet(p, fbs, CUTE_N_MOODS);
    }
    for (int m = 0; m < CUTE_N_MOODS; m++) free(fbs[m]);
    fluid_frame_destroy(fr); fluid_draw_destroy(d); fluid_sim_destroy(s);
    cute_rt_destroy(rt);
}

/* ── actions ─────────────────────────────────────────────────────────────── */

static float pose_mag(const cute_act_pose_t *p) {
    const float *f = (const float *)p; float m = 0.f;
    for (size_t k = 0; k < sizeof(*p) / sizeof(float); k++) m = fmaxf(m, fabsf(f[k]));
    return m;
}

/* the JS twin's samples (act_golden.txt) against cute_act_pose */
static void test_act_golden(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { CHECK(0, "open %s", path); return; }
    char line[512], name[32];
    int rows = 0, bad = 0, seen[CUTE_N_ACTS] = { 0 };
    float last_t[CUTE_N_ACTS] = { 0 };
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        float t, v[17];
        int n = sscanf(line, "%31s %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f", name, &t,
                       &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15], &v[16]);
        if (n != 19) { CHECK(0, "act golden: bad row %s", line); continue; }
        int a = cute_act_lookup(name);
        if (a < 0) { CHECK(0, "act golden: the JS has an action the firmware lacks: %s", name); continue; }
        seen[a]++; last_t[a] = t; rows++;
        cute_act_pose_t p; cute_act_pose(a, t, &p);
        const float *c = (const float *)&p;
        for (int k = 0; k < 17; k++) if (fabsf(c[k] - v[k]) > 2e-4f) {
            if (bad++ < 5) printf("  act %s t=%.2f field %d: C %.6f JS %.6f\n", name, t, k, c[k], v[k]);
        }
    }
    fclose(f);
    CHECK(rows > 100 && bad == 0, "act golden: %d of %d samples differ from the JS twin", bad, rows);
    for (int a = 0; a < CUTE_N_ACTS; a++)
        CHECK(seen[a] > 3 && fabsf(last_t[a] - cute_act_dur(a)) < 1e-4f, "act %s: in the golden %d times, last t %.2f vs dur %.2f",
              cute_act_name(a), seen[a], last_t[a], cute_act_dur(a));
}

/* every action: zero at both ends, no jump between 60 Hz frames, and it does something */
static void test_act_curves(void) {
    for (int a = 0; a < CUTE_N_ACTS; a++) {
        cute_act_pose_t p0, p1, prev, cur;
        const float d = cute_act_dur(a);
        cute_act_pose(a, 0.f, &p0); cute_act_pose(a, d, &p1);
        CHECK(pose_mag(&p0) < 1e-3f && pose_mag(&p1) < 1e-3f, "act %s: starts %.4f / ends %.4f away from rest", cute_act_name(a), pose_mag(&p0), pose_mag(&p1));
        float peak = 0.f, jump = 0.f;
        prev = p0;
        for (float t = 1.f / 60.f; t <= d; t += 1.f / 60.f) {
            cute_act_pose(a, t, &cur);
            const float *x = (const float *)&cur, *y = (const float *)&prev;
            for (size_t k = 0; k < sizeof(cur) / sizeof(float); k++) jump = fmaxf(jump, fabsf(x[k] - y[k]));
            peak = fmaxf(peak, pose_mag(&cur));
            prev = cur;
        }
        CHECK(peak > 0.05f, "act %s: peak offset %.3f (does nothing)", cute_act_name(a), peak);
        CHECK(jump < 0.25f, "act %s: jumps %.3f between frames", cute_act_name(a), jump);
        cute_act_pose(a, d + 0.01f, &cur);
        CHECK(pose_mag(&cur) == 0.f, "act %s: nonzero after its end", cute_act_name(a));
    }
    CHECK(cute_act_lookup("nod") == CA_NOD && cute_act_lookup("Nod") < 0 && cute_act_lookup(NULL) < 0 && cute_act_lookup("") < 0,
          "act lookup is exact");
}

static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

/* each action on the creature: the target moves while it plays and comes back after; the
 * runtime's update costs about the same with an action playing */
static void test_act_render(const char *path, const char *outdir) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    static uint8_t buf[CUTE_MAX_FILE + 16];
    size_t n = fread(buf, 1, sizeof(buf), f); fclose(f);
    const char *err;
    if (cute_spec_parse(buf, n, &S, &err) != 0) return;
    const int W = 52, H = 32, N = W * H;
    fluid_sim_t *s = fluid_sim_create(W, H, FLUID_STAGE_REF);
    fluid_draw_t *d = fluid_draw_create(W, H, FLUID_STAGE_REF, FLUID_STAGE_REF);
    fluid_frame_t *fr = fluid_frame_create(W, H);
    uint16_t *fbs[CUTE_N_ACTS];
    float *rest = calloc(N, sizeof(float));
    for (int a = 0; a < CUTE_N_ACTS; a++) {
        /* two runtimes with the same seed: one plays the action, the twin does not */
        cute_rt_t *rt = cute_rt_create(&S, 99), *twin = cute_rt_create(&S, 99);
        for (int k = 0; k < 30; k++) { cute_rt_update(rt, 1.f / 30.f); cute_rt_update(twin, 1.f / 30.f); }
        cute_rt_act(rt, a);
        CHECK(cute_rt_acting(rt) == a, "act %s: playing", cute_act_name(a));
        float diff = 0.f; int peak_k = 0;
        fbs[a] = calloc(FLUID_STAGE_REF * FLUID_STAGE_REF, 2);
        const int frames = (int)ceilf(cute_act_dur(a) * 30.f) + 2;   /* + float accumulation */
        for (int k = 0; k < frames; k++) {
            cute_rt_update(rt, 1.f / 30.f); cute_rt_update(twin, 1.f / 30.f);
            cute_rt_raster(twin, W, H); cute_rt_fill(twin, s, 1.f);
            for (int c = 0; c < N; c++) rest[c] = s->T[c] / FQ_UNIT;
            cute_rt_raster(rt, W, H); cute_rt_fill(rt, s, 1.f);
            float dk = 0.f;
            for (int c = 0; c < N; c++) dk += fabsf(s->T[c] / FQ_UNIT - rest[c]);
            if (dk > diff) { diff = dk; peak_k = k; }
        }
        CHECK(diff > 2.f, "act %s: the target barely moves (Σ|ΔT| %.2f)", cute_act_name(a), diff);
        CHECK(cute_rt_acting(rt) < 0, "act %s: still playing after %d frames", cute_act_name(a), frames);
        /* the peak frame as a still (fluid run toward it, as in the mood sheet) */
        if (outdir) {
            cute_rt_t *pk = cute_rt_create(&S, 99);
            for (int k = 0; k < 30; k++) cute_rt_update(pk, 1.f / 30.f);
            cute_rt_act(pk, a);
            for (int k = 0; k <= peak_k; k++) cute_rt_update(pk, 1.f / 30.f);
            fluid_sim_t *ps = fluid_sim_create(W, H, FLUID_STAGE_REF);
            ps->edge_w = 1.f;
            cute_rt_raster(pk, W, H);
            for (int fno = 0; fno < 60; fno++) {
                float t = fno / 30.f;
                ps->p.hold = fminf(1.f, t / 1.2f); ps->p.relax_up = 2.6f; ps->p.relax_down = 3.4f; ps->p.tint_rate = 3 + 9 * ps->p.hold;
                cute_rt_fill(pk, ps, 1.f);
                fluid_sim_background(ps, t, 0.5f, (fno & 1) == 0); fluid_sim_swirl_field(ps);
                fluid_sim_step(ps, 1.f / 60); fluid_sim_step(ps, 1.f / 60); fluid_sim_color(ps, 1.f / 30); fluid_sim_view(ps);
                fluid_sim_snapshot(ps, fr, false);
                fluid_draw_opts_t o = { FLUID_PAL_PASTEL, FLUID_EDGE_ALL, 0.12f, t, 0.f, NULL, 0, " .:-=+*#", font };
                int y0, y1; fluid_draw(d, fr, fbs[a], &o, &y0, &y1);
            }
            fluid_sim_destroy(ps); cute_rt_destroy(pk);
        }
        printf("  %-14s %-12s peak Σ|ΔT| %6.1f at %.2f s\n", S.id, cute_act_name(a), diff, peak_k / 30.f);
        cute_rt_destroy(rt); cute_rt_destroy(twin);
    }
    /* frame cost: the runtime's update (where the action layer runs) with and without one */
    cute_rt_t *rt = cute_rt_create(&S, 7);
    const int REPS = 3000;
    double t0 = now_s();
    for (int k = 0; k < REPS; k++) cute_rt_update(rt, 1.f / 30.f);
    double idle = now_s() - t0;
    t0 = now_s();
    for (int k = 0; k < REPS; k++) { if (cute_rt_acting(rt) < 0) cute_rt_act(rt, k % CUTE_N_ACTS); cute_rt_update(rt, 1.f / 30.f); }
    double acting = now_s() - t0;
    cute_rt_raster(rt, W, H);
    t0 = now_s();
    for (int k = 0; k < 300; k++) { cute_rt_update(rt, 1.f / 30.f); cute_rt_raster(rt, W, H); cute_rt_fill(rt, s, 1.f); }
    double whole = (now_s() - t0) / 300;
    printf("  frame cost (host): update %.2f us idle, %.2f us acting (+%.2f us); update+raster+fill %.1f us\n",
           idle / REPS * 1e6, acting / REPS * 1e6, (acting - idle) / REPS * 1e6, whole * 1e6);
    CHECK((acting - idle) / REPS < 0.05 * whole + 2e-6, "acting adds %.2f us per update (whole creature frame %.1f us)",
          (acting - idle) / REPS * 1e6, whole * 1e6);
    cute_rt_destroy(rt);
    if (outdir) {
        char p[512]; const char *id = strncmp(S.id, "cute-", 5) == 0 ? S.id + 5 : S.id;
        snprintf(p, sizeof(p), "%s/%s_acts.ppm", outdir, id);
        write_sheet(p, fbs, CUTE_N_ACTS);
        for (int a = 0; a < CUTE_N_ACTS; a++) free(fbs[a]);
    } else for (int a = 0; a < CUTE_N_ACTS; a++) free(fbs[a]);
    free(rest);
    fluid_frame_destroy(fr); fluid_draw_destroy(d); fluid_sim_destroy(s);
}

/* the reflex over a listen is visible: from the listening mood, each gesture the relay's reflex
 * or the device's baseline plays (services/relay REFLEX_SHOW / REFLEX_SOFT, listen_react.h) and each
 * react mood moves the creature's target well away from a twin that only keeps listening */
static void test_listen_visible(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    static uint8_t buf[CUTE_MAX_FILE + 16];
    size_t n = fread(buf, 1, sizeof(buf), f); fclose(f);
    const char *err;
    if (cute_spec_parse(buf, n, &S, &err) != 0) return;
    const int W = 52, H = 32, N = W * H;
    fluid_sim_t *s = fluid_sim_create(W, H, FLUID_STAGE_REF);
    float *rest = calloc(N, sizeof(float));
    /* gestures (act), then the react moods (-1 - mood) */
    static const int shows[] = { CA_NOD, CA_BOUNCE, CA_PEEK, CA_LOOK_UP, CA_WIGGLE, CA_WOBBLE, CA_GLANCE_LEFT, CA_GLANCE_RIGHT, CA_BLINK,
                                 -1 - CM_SURPRISED, -1 - CM_HAPPY };
    for (size_t i = 0; i < sizeof(shows) / sizeof(shows[0]); i++) {
        cute_rt_t *rt = cute_rt_create(&S, 99), *twin = cute_rt_create(&S, 99);
        cute_rt_set_mood(rt, CM_LISTENING); cute_rt_set_mood(twin, CM_LISTENING);
        for (int k = 0; k < 60; k++) { cute_rt_update(rt, 1.f / 30.f); cute_rt_update(twin, 1.f / 30.f); }
        const int a = shows[i];
        if (a >= 0) cute_rt_act(rt, a); else cute_rt_set_mood(rt, -1 - a);
        const char *name = a >= 0 ? cute_act_name(a) : (-1 - a) == CM_SURPRISED ? "mood surprised" : "mood happy";
        const int frames = a >= 0 ? (int)ceilf(cute_act_dur(a) * 30.f) : 45;   /* a react mood: the relay's 1.5 s */
        float diff = 0.f;
        for (int k = 0; k < frames; k++) {
            cute_rt_update(rt, 1.f / 30.f); cute_rt_update(twin, 1.f / 30.f);
            cute_rt_raster(twin, W, H); cute_rt_fill(twin, s, 1.f);
            for (int c = 0; c < N; c++) rest[c] = s->T[c] / FQ_UNIT;
            cute_rt_raster(rt, W, H); cute_rt_fill(rt, s, 1.f);
            float dk = 0.f;
            for (int c = 0; c < N; c++) dk += fabsf(s->T[c] / FQ_UNIT - rest[c]);
            if (dk > diff) diff = dk;
        }
        CHECK(diff > 15.f, "over listening, %s barely shows (Σ|ΔT| %.2f)", name, diff);
        printf("  %-14s listening + %-15s peak Σ|ΔT| %6.1f\n", S.id, name, diff);
        cute_rt_destroy(rt); cute_rt_destroy(twin);
    }
    free(rest);
    fluid_sim_destroy(s);
}

int main(int argc, char **argv) {
    const char *outdir = NULL, *golden = NULL, *first = NULL;
    printf("cute runtime host test\n");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) { outdir = argv[++i]; continue; }
        if (!strcmp(argv[i], "-g") && i + 1 < argc) { golden = argv[++i]; continue; }
        if (!first) first = argv[i];
        test_file(argv[i], outdir);
    }
    test_act_curves();
    if (golden) test_act_golden(golden);
    else CHECK(0, "no act golden given (-g act_golden.txt)");
    if (first) test_act_render(first, outdir);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "-g")) { i++; continue; }
        test_listen_visible(argv[i]);
    }
    printf("%d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
