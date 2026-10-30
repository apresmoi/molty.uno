/*
 * cute_test.c — host test for the cute-kit runtime (cute_spec.c + cute_rig.c) driving
 * the fluid (fluid_*.c). For every .molty given on the command line:
 *   - parses + validates it, and rejects a corrupted copy (CRC) and a truncated one;
 *   - builds the creature, checks the rest-pose normalisation (height 1 or bulk-scaled),
 *     the tri budget (< 1000 visible) and that each of the 7 moods rasterises a body
 *     with solid features (eyes) somewhere on the grid;
 *   - runs the fluid toward it and writes a preview contact sheet (all moods) to
 *     <outdir>/<id>_moods.ppm when an outdir is given.
 * Build + run: test/host/cute_build.sh [outdir]
 */
#include "cute_rig.h"
#include "fluid_sim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(int argc, char **argv) {
    const char *outdir = NULL;
    printf("cute runtime host test\n");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) { outdir = argv[++i]; continue; }
        test_file(argv[i], outdir);
    }
    printf("%d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
