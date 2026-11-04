/*
 * creature_test.c: host test for the creature package player (creature/creature_player.c).
 *
 *   golden    each creature package replayed against the JS reference player's frames
 *             (tools/creatures/golden.mjs): the same inputs (pose, gesture, walk) must give
 *             the same pose, bone outputs, palette and ambient, every frame.
 *   strict    broken copies of a package (unknown key, op, bone, group; a wrong type; over
 *             the limits) are refused with a reason.
 *
 *   render    each creature through the device's whole frame (mood engine, player, body,
 *             ambient, fill, fluid, glyphs): every mood fills the target, the eyes are holes,
 *             and with an out dir, one 412 px frame per mood (<id>_<mood>.ppm) to compare
 *             with the lab's clips.
 *   bench     the frame's creature work against the .molty duck's on the host, scaled to the
 *             device's ~11 ms for the latter at 33 fps (as scene_test does).
 *
 * Usage: creature_test <packages dir> <golden dir> <duck.molty> [out dir]   (test/host/creature_build.sh)
 */
#include "creature_player.h"
#include "creature_pkg.h"
#include "cute_act.h"
#include "cute_rig.h"
#include "fluid_sim.h"

#include <time.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_pass, s_fail;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[n] = 0; *len = (size_t)n; }
    return b;
}

/* tokens of one golden line */
typedef struct { char *t[4096]; int n, at; } toks_t;
static void split(char *line, toks_t *k) {
    k->n = k->at = 0;
    for (char *s = strtok(line, " \n"); s && k->n < 4096; s = strtok(NULL, " \n")) k->t[k->n++] = s;
}
static const char *tok(toks_t *k) { return k->at < k->n ? k->t[k->at++] : ""; }
static double num(toks_t *k) { return strtod(tok(k), NULL); }

/* what the ambient did to the fluid this frame */
typedef struct { float v[5]; } call_t;
static call_t imp[64], push[8];
static int nimp, npush;
static void imp_cb(void *ctx, float x, float y, float s, float r) { (void)ctx; if (nimp < 64) imp[nimp++] = (call_t){ { x, y, s, r, 0 } }; }
static void push_cb(void *ctx, float x, float y, float vx, float vy, float r) { (void)ctx; if (npush < 8) push[npush++] = (call_t){ { x, y, vx, vy, r } }; }

static bool close_to(double a, double b, double tol) { return fabs(a - b) <= tol * (1 + fabs(b)); }

static void test_golden(const char *pkgdir, const char *goldir, const char *id) {
    char path[512]; size_t len;
    snprintf(path, sizeof(path), "%s/%s/%s.json", pkgdir, id, id);
    char *json = slurp(path, &len);
    snprintf(path, sizeof(path), "%s/%s.golden", goldir, id);
    size_t glen; char *gold = slurp(path, &glen);
    CHECK(json && gold, "%s: package or golden missing", id);
    if (!json || !gold) { free(json); free(gold); return; }
    const char *err = NULL;
    cp_t *c = cp_create(json, len, &err);
    CHECK(c != NULL, "%s: %s", id, err ? err : "");
    if (!c) { free(json); free(gold); return; }
    static toks_t k;
    char *save = NULL, *line = strtok_r(gold, "\n", &save);
    split(line, &k);
    tok(&k); tok(&k); tok(&k);
    const int nb = (int)num(&k);
    int bone[CP_BONES_MAX + 1];
    for (int b = 0; b < nb; b++) { const char *bid = tok(&k); bone[b] = cp_bone_find(c, bid); CHECK(bone[b] >= 0, "%s: bone %s", id, bid); }
    tok(&k); const int nd = (int)num(&k); float depth[16];
    for (int d = 0; d < nd; d++) depth[d] = (float)num(&k);
    tok(&k); const int np = (int)num(&k);
    CHECK(np == cp_body(c)->npal, "%s: palette %d vs %d", id, np, cp_body(c)->npal);
    int frames = 0, bad_pose = 0, bad_bone = 0, bad_tint = 0, bad_amb = 0, marks = 0, worst_f = 0;
    double err_pose = 0, err_bone = 0;
    cp_pose_t p; cp_action_t act, prev; cp_walk_t w; double dt = 0; float prev_w = 0; bool has_act = false, has_prev = false;
    static cp_mark_t mk[512];
    static float cov[52 * 32];
    for (int j = 0; j < 32; j++) for (int i = 0; i < 52; i++) {
        double x = ((i + 0.5) / 52 * 2 - 1) * 0.85, y = (1 - (j + 0.5) / 32 * 2) * 0.85;   /* golden.mjs's disc, in double as there */
        cov[j * 52 + i] = hypot(x, y + 0.1) < 0.42 ? 1.f : 0.f;
    }
    for (line = strtok_r(NULL, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        split(line, &k);
        const char *t0 = tok(&k);
        if (!strcmp(t0, "i")) {
            dt = num(&k);
            float *pv[18] = { &p.pos[0], &p.pos[1], &p.pos[2], &p.rot[0], &p.rot[1], &p.rot[2], &p.head[0], &p.head[1], &p.head[2],
                              &p.spine[0], &p.spine[1], &p.spine[2], &p.squash, &p.lid, &p.mouth, &p.look[0], &p.look[1], &p.prowl };
            for (int q = 0; q < 18; q++) *pv[q] = (float)num(&k);
            p.eye = (int)num(&k); p.mood = (int)num(&k);
            const char *an = tok(&k); act.act = cute_act_lookup(an); act.t = (float)num(&k); act.n = (unsigned)num(&k); has_act = strcmp(an, "-") != 0;
            const char *pn = tok(&k); prev.act = cute_act_lookup(pn); prev.t = (float)num(&k); prev_w = (float)num(&k); prev.n = (unsigned)num(&k); has_prev = strcmp(pn, "-") != 0;
            float *wv[7] = { &w.q, &w.k, &w.go, &w.along, &w.side, &w.fz, &w.up };
            for (int q = 0; q < 7; q++) *wv[q] = (float)num(&k);
            cp_pose(c, &p, dt, has_act ? &act : NULL, has_prev ? &prev : NULL, prev_w, &w);
            if (cp_factors(c) > worst_f) worst_f = cp_factors(c);
            frames++;
        } else if (!strcmp(t0, "o")) {
            const float pv[18] = { p.pos[0], p.pos[1], p.pos[2], p.rot[0], p.rot[1], p.rot[2], p.head[0], p.head[1], p.head[2],
                                   p.spine[0], p.spine[1], p.spine[2], p.squash, p.lid, p.mouth, p.look[0], p.look[1], p.prowl };
            bool bp = false, bb = false, bt = false;
            for (int q = 0; q < 18; q++) { double g = num(&k), e = fabs(pv[q] - g) / (1 + fabs(g)); if (e > err_pose) err_pose = e; if (!close_to(pv[q], g, 5e-4)) { if (!bad_pose && !bp) printf("  pose[%d] %g vs JS %g\n", q, pv[q], g); bp = true; } }
            for (int b = 0; b < nb; b++) {
                const float *o = cp_bone_out(c, bone[b]);
                for (int q = 0; q < 9; q++) { double g = num(&k), e = fabs(o[q] - g) / (1 + fabs(g)); if (e > err_bone) err_bone = e; if (!close_to(o[q], g, 5e-4)) bb = true; }
            }
            for (int d = 0; d < nd; d++) {
                uint32_t tn[CP_PALETTE_MAX];
                cp_tints(c, depth[d], tn);
                for (int q = 0; q < np; q++) {
                    uint32_t g = (uint32_t)strtoul(tok(&k), NULL, 16);
                    for (int s = 0; s < 24; s += 8) if (abs((int)((tn[q] >> s) & 255) - (int)((g >> s) & 255)) > 1) bt = true;
                }
            }
            if (bp && bad_pose++ < 3) printf("  %s frame %d: pose differs\n", id, frames);
            if (bb && bad_bone++ < 3) printf("  %s frame %d: bones differ\n", id, frames);
            if (bt && bad_tint++ < 3) printf("  %s frame %d: tints differ\n", id, frames);
        } else if (!strcmp(t0, "a")) {
            nimp = npush = 0;
            cp_env_t env = { (float)dt, cov, 52, 32, 0.85f, NULL, imp_cb, push_cb };
            const int n = cp_ambient(c, &env, mk, 512), gn = (int)num(&k);
            bool ba = n != gn;
            static const char *const KIND[] = { "dot", "star", "blot" };
            for (int q = 0; q < gn && q < n; q++) {
                const cp_mark_t *m = &mk[q];
                const char *kn = tok(&k);
                double x = num(&k), y = num(&k), sz = num(&k); uint32_t col = (uint32_t)strtoul(tok(&k), NULL, 16); int d = (int)num(&k); double rot = num(&k);
                if (strcmp(kn, KIND[m->kind]) || !close_to(m->x, x, 5e-4) || !close_to(m->y, y, 5e-4) || !close_to(m->size, sz, 5e-4) || m->d != d || !close_to(m->rot, rot, 5e-4)
                    || col != ((uint32_t)m->rgb[0] << 16 | (uint32_t)m->rgb[1] << 8 | m->rgb[2])) { if (!bad_amb && !ba) printf("  mark %d: %s %g %g %g d%d rot %g vs JS %s %g %g %g d%d rot %g\n", q, KIND[m->kind], m->x, m->y, m->size, m->d, m->rot, kn, x, y, sz, d, rot); ba = true; }
            }
            for (int q = n; q < gn; q++) for (int s = 0; s < 7; s++) tok(&k);
            const int gi = (int)num(&k);
            ba |= gi != nimp;
            for (int q = 0; q < gi; q++) for (int s = 0; s < 4; s++) { double g = num(&k); if (q < nimp && !close_to(imp[q].v[s], g, 5e-4)) ba = true; }
            const int gp = (int)num(&k);
            ba |= gp != npush;
            for (int q = 0; q < gp; q++) for (int s = 0; s < 5; s++) { double g = num(&k); if (q < npush && !close_to(push[q].v[s], g, 5e-4)) ba = true; }
            marks += n;
            if (ba && bad_amb++ < 3) printf("  %s frame %d: ambient differs (%d marks vs %d)\n", id, frames, n, gn);
        }
    }
    printf("  %-8s %d frames, worst %d factors/frame, max error pose %.2g bones %.2g, %d ambient marks\n", id, frames, worst_f, err_pose, err_bone, marks);
    CHECK(frames > 600, "%s: %d golden frames", id, frames);
    CHECK(bad_pose == 0, "%s: pose differs from the JS player in %d of %d frames", id, bad_pose, frames);
    CHECK(bad_bone == 0, "%s: bones differ from the JS player in %d of %d frames", id, bad_bone, frames);
    CHECK(bad_tint == 0, "%s: tints differ from the JS player in %d of %d frames", id, bad_tint, frames);
    CHECK(bad_amb == 0, "%s: ambient differs from the JS player in %d of %d frames", id, bad_amb, frames);
    cp_destroy(c);
    free(json); free(gold);
}

/* a copy of the package with `from` replaced by `to` (first occurrence) must be refused */
static void refused(const char *json, const char *from, const char *to, const char *why) {
    const char *at = strstr(json, from);
    CHECK(at != NULL, "mutation '%s' not found", from);
    if (!at) return;
    size_t a = (size_t)(at - json), fl = strlen(from), tl = strlen(to), n = strlen(json);
    char *m = malloc(n - fl + tl + 1);
    memcpy(m, json, a); memcpy(m + a, to, tl); memcpy(m + a + tl, at + fl, n - a - fl + 1);
    const char *err = NULL;
    cp_t *c = cp_create(m, strlen(m), &err);
    CHECK(c == NULL && err, "%s: accepted", why);
    if (!c) printf("  refused (%s): %s\n", why, err);
    cp_destroy(c);
    free(m);
}

static void test_strict(const char *pkgdir) {
    char path[512]; size_t len;
    snprintf(path, sizeof(path), "%s/octopus/octopus.json", pkgdir);
    char *j = slurp(path, &len);
    CHECK(j != NULL, "octopus.json");
    if (!j) return;
    refused(j, "\"creature\":{\"body\"", "\"creature\":{\"extra\":1,\"body\"", "an unknown key");
    refused(j, "[\"sin\",", "[\"sine\",", "an unknown op");
    refused(j, "[\"pos:", "[\"wat:", "an unknown modifier");
    refused(j, "\"bone\":\"head\"", "\"bone\":\"noggin\"", "a part on an unknown bone");
    refused(j, "\"each\":\"waves\"", "\"each\":\"nope\"", "an unknown group");
    refused(j, "\"type\":\"ellipsoid\"", "\"type\":\"torus\"", "an unknown shape");
    refused(j, "\"palette\":{", "\"palette\":{\"x\":\"red\",", "a colour that is not #rrggbb");
    refused(j, "\"kind\":\"creature\"", "\"kind\":\"scenes\"", "not a creature");
    refused(j, ",[\"sin\",\"a\"", ",[\"sin\",\"a\",1,2,3,4", "too many op params");
    refused(j, "\"emit\":{", "\"emit\":{\"x\":{\"p\":\"nope\",\"at\":\"floor\"},", "an emitter of an unknown kind");
    const char *err = NULL;
    CHECK(cp_create(j, len - 12, &err) == NULL, "a truncated file is accepted");
    char *big = malloc(CP_FILE_MAX + 2); memset(big, ' ', CP_FILE_MAX + 1); big[CP_FILE_MAX + 1] = 0;
    CHECK(cp_create(big, CP_FILE_MAX + 1, &err) == NULL && !strcmp(err, "too_big"), "over 32 KB is accepted");
    free(big);
    free(j);
}

static void test_hash(void) {
    /* the JS player's hash (creature-player.js `hash`), bit for bit */
    static const double in[5] = { 0, 1.37, -5.3, 123456.789, 9973 * 7.31 + 1000 * 1.37 };
    static const double js[5] = { 0.57339758006855845, 0.96578984591178596, 0.84421899612061679, 0.26516305725090206, 0.50762761430814862 };
    for (int i = 0; i < 5; i++) CHECK(cp_hash(in[i]) == js[i], "hash(%g) = %.17g, JS %.17g", in[i], cp_hash(in[i]), js[i]);
}


/* ── the device's frame, end to end ───────────────────────────────────────── */

static const uint8_t *font(char ch) {
    static const uint8_t full[7] = { 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f };
    (void)ch; return full;
}
static void write_ppm(const char *path, const uint16_t *fb) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", FLUID_STAGE_REF, FLUID_STAGE_REF);
    for (int i = 0; i < FLUID_STAGE_REF * FLUID_STAGE_REF; i++) {
        uint16_t le = (uint16_t)((fb[i] >> 8) | (fb[i] << 8));
        unsigned char c[3] = { (unsigned char)((le >> 11) << 3), (unsigned char)(((le >> 5) & 63) << 2), (unsigned char)((le & 31) << 3) };
        fwrite(c, 1, 3, f);
    }
    fclose(f);
}
static double now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }

static const char *const MOOD_NAMES[CP_N_MOODS] = { "idle", "listening", "thinking", "speaking", "happy", "sleepy", "surprised" };

/* every mood for 2.5 s (a gesture in each), through the fluid and the glyphs, as mole_fluid.c runs it */
static double test_render(const char *pkgdir, const char *id, const char *outdir) {
    char path[512]; size_t len;
    snprintf(path, sizeof(path), "%s/%s/%s.json", pkgdir, id, id);
    char *json = slurp(path, &len);
    const char *err = NULL;
    cpk_t *k = json ? cpk_create(json, len, 1234, &err) : NULL;
    CHECK(k != NULL, "%s: create (%s)", id, err ? err : "no file");
    if (!k) { free(json); return 0; }
    const int W = 52, H = 32;
    fluid_sim_t *s = fluid_sim_create(W, H, FLUID_STAGE_REF);
    fluid_draw_t *d = fluid_draw_create(W, H, FLUID_STAGE_REF, FLUID_STAGE_REF);
    fluid_frame_t *fr = fluid_frame_create(W, H);
    uint16_t *fb = calloc(FLUID_STAGE_REF * FLUID_STAGE_REF, 2);
    s->edge_w = 1.f;
    int worst = 0, eyes = 0; double us = 0; int timed = 0;
    for (int m = 0; m < CP_N_MOODS; m++) {
        me_set_mood(cpk_engine(k), m);
        float ink = 0; int holes = 0;
        for (int f = 0; f < 75; f++) {
            const float t = f / 30.f;
            if (f == 20) me_act(cpk_engine(k), (m * 3 + 1) % CA_INSPECT);
            s->p.hold = fminf(1.f, t / 1.2f);
            s->p.relax_up = 2.6f; s->p.relax_down = 3.4f; s->p.tint_rate = 3 + 9 * s->p.hold;
            const double t0 = now_us();
            cpk_frame(k, s, 1.f / 30.f, 1.f);
            us += now_us() - t0; timed++;
            fluid_sim_background(s, t, 0.5f, (f & 1) == 0);
            fluid_sim_swirl_field(s);
            fluid_sim_step(s, 1.f / 60); fluid_sim_step(s, 1.f / 60);
            fluid_sim_color(s, 1.f / 30);
            fluid_sim_view(s);
            fluid_sim_snapshot(s, fr, false);
            fluid_draw_opts_t o = { FLUID_PAL_PASTEL, FLUID_EDGE_ALL, 0.12f, t, 0.f, NULL, 0, " .:-=+*#", font };
            int y0, y1;
            fluid_draw(d, fr, fb, &o, &y0, &y1);
            if (cpk_tris(k) > worst) worst = cpk_tris(k);
        }
        const float *cov, *lum, *r, *g, *b; const uint8_t *hole;
        cb_cells(cpk_body(k), &cov, &lum, &r, &g, &b, &hole);
        for (int q = 0; q < W * H; q++) { ink += s->T[q] / FQ_UNIT; holes += hole[q]; }
        CHECK(ink > 20.f, "%s %s: the target is nearly empty (%.1f)", id, MOOD_NAMES[m], ink);
        eyes += holes;
        printf("  %-8s %-10s ink %6.1f  holes %3d  tris %d\n", id, MOOD_NAMES[m], ink, holes, cpk_tris(k));
        if (outdir) { snprintf(path, sizeof(path), "%s/%s_%s.ppm", outdir, id, MOOD_NAMES[m]); write_ppm(path, fb); }
    }
    CHECK(eyes > 0, "%s: no dark holes (the eyes) in any mood", id);
    CHECK(worst > 100 && worst < 4000, "%s: %d triangles drawn", id, worst);
    free(fb); fluid_frame_destroy(fr); fluid_draw_destroy(d); fluid_sim_destroy(s);
    cpk_destroy(k); free(json);
    return us / timed;
}

/* the .molty duck's update + raster + fill on the host: the work the device does in ~11 ms */
static double bench_v1(const char *molty) {
    static uint8_t buf[CUTE_MAX_FILE + 16];
    static cute_spec_t spec;
    FILE *f = fopen(molty, "rb");
    size_t n = f ? fread(buf, 1, sizeof(buf), f) : 0;
    if (f) fclose(f);
    const char *err;
    if (!n || cute_spec_parse(buf, n, &spec, &err) != 0) return 0;
    cute_rt_t *rt = cute_rt_create(&spec, 99);
    fluid_sim_t *s = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    for (int k = 0; k < 30; k++) cute_rt_update(rt, 1.f / 30);
    double t0 = now_us();
    for (int k = 0; k < 1500; k++) { if (k % 200 == 0) cute_rt_set_mood(rt, k / 200 % CUTE_N_MOODS); cute_rt_update(rt, 1.f / 30); cute_rt_raster(rt, 52, 32); cute_rt_fill(rt, s, 1.f); }
    const double us = (now_us() - t0) / 1500;
    fluid_sim_destroy(s); cute_rt_destroy(rt);
    return us;
}

int main(int argc, char **argv) {
    if (argc < 4) { printf("usage: creature_test <packages dir> <golden dir> <duck.molty> [out dir]\n"); return 2; }
    printf("creature player host test\n");
    test_hash();
    for (const char *const *id = (const char *const[]){ "duck", "cat", "octopus", NULL }; *id; id++) test_golden(argv[1], argv[2], *id);
    test_strict(argv[1]);
    const double v1 = bench_v1(argv[3]);
    CHECK(v1 > 0, "the .molty duck to compare with (%s)", argv[3]);
    for (const char *const *id = (const char *const[]){ "duck", "cat", "octopus", NULL }; *id; id++) {
        const double us = test_render(argv[1], *id, argc > 4 ? argv[4] : NULL), ms = 11.0 * us / (v1 > 0 ? v1 : 1), frame = 30.0 - 11.0 + ms;
        printf("  bench %-8s host %.1f us a frame (the .molty duck %.1f us): device estimate %.1f ms, frame %.1f ms = %.0f fps\n",
               *id, us, v1, ms, frame, 1000.0 / (frame > 30.0 ? frame : 30.0));
    }
    printf("%d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
