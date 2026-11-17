/*
 * creature_test.c: host test for the creature package player (creature/creature_player.c).
 *
 *   golden    each creature package replayed against the JS reference player's frames
 *             (tools/creatures/golden.mjs): the same inputs (pose, gesture, walk) must give
 *             the same pose, bone outputs, palette and ambient, every frame.
 *   strict    broken copies of a package (unknown key, op, bone, group; a wrong type; over
 *             the limits) are refused with a reason.
 *   info      cp_info, what the device tells the relay: the package's about, its gestures,
 *             idle and its moods.
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
#include "creature_clip.h"
#include "cute_act.h"
#include "cute_rig.h"
#include "fluid_sim.h"
#include "scene_player.h"
#include "cJSON.h"

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
            const char *an = tok(&k); act.act = cp_gesture(c, an); act.t = (float)num(&k); act.n = (unsigned)num(&k); has_act = strcmp(an, "-") != 0;
            const char *pn = tok(&k); prev.act = cp_gesture(c, pn); prev.t = (float)num(&k); prev_w = (float)num(&k); prev.n = (unsigned)num(&k); has_prev = strcmp(pn, "-") != 0;
            float *wv[7] = { &w.q, &w.k, &w.go, &w.along, &w.side, &w.fz, &w.up };
            for (int q = 0; q < 7; q++) *wv[q] = (float)num(&k);
            p.work = num(&k) != 0;
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
    refused(j, "\"creature\":{\"about\"", "\"creature\":{\"extra\":1,\"about\"", "an unknown key");
    refused(j, "\"about\":{\"look\":\"", "\"about\":{\"voice\":\"squeaky\",\"look\":\"", "an unknown about key");
    refused(j, "\"about\":{\"look\":\"A", "\"about\":{\"look\":\"\\tA", "a look that is not printable words");
    refused(j, "[\"sin\",", "[\"sine\",", "an unknown op");
    refused(j, "[\"pos:", "[\"wat:", "an unknown modifier");
    refused(j, "\"bone\":\"head\"", "\"bone\":\"noggin\"", "a part on an unknown bone");
    refused(j, "\"each\":\"waves\"", "\"each\":\"nope\"", "an unknown group");
    refused(j, "\"type\":\"ellipsoid\"", "\"type\":\"torus\"", "an unknown shape");
    refused(j, "\"palette\":{", "\"palette\":{\"x\":\"red\",", "a colour that is not #rrggbb");
    refused(j, "\"kind\":\"creature\"", "\"kind\":\"scenes\"", "not a creature");
    refused(j, ",[\"sin\",\"a\"", ",[\"sin\",\"a\",1,2,3,4", "too many op params");
    refused(j, "\"gestures\":{\"wiggle\":{", "\"gestures\":{\"twirl\":{\"ch\":[]},\"wiggle\":{", "its own gesture without dur");
    refused(j, "\"gestures\":{\"wiggle\":{", "\"gestures\":{\"twirl\":{\"ch\":[],\"dur\":11},\"wiggle\":{", "its own gesture too long");
    refused(j, "\"gestures\":{\"wiggle\":{", "\"gestures\":{\"Twirl\":{\"ch\":[],\"dur\":1},\"wiggle\":{", "a gesture name agents cannot send");
    refused(j, "\"gestures\":{\"wiggle\":{", "\"gestures\":{\"nod\":{\"ch\":[],\"dur\":1},\"wiggle\":{", "a shared gesture with a length of its own");
    refused(j, "\"emit\":{", "\"emit\":{\"x\":{\"p\":\"nope\",\"at\":\"floor\"},", "an emitter of an unknown kind");
    const char *err = NULL;
    CHECK(cp_create(j, len - 12, &err) == NULL, "a truncated file is accepted");
    char *big = malloc(CP_FILE_MAX + 2); memset(big, ' ', CP_FILE_MAX + 1); big[CP_FILE_MAX + 1] = 0;
    CHECK(cp_create(big, CP_FILE_MAX + 1, &err) == NULL && !strcmp(err, "too_big"), "over 40 KB is accepted");
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
    const bool crisp = cpk_crisp(k);       /* a rigged creature: its own dots, the fluid still under them (mole_fluid.c) */
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
            if (!crisp) { fluid_sim_background(s, t, 0.5f, (f & 1) == 0); fluid_sim_swirl_field(s); }
            fluid_sim_step(s, 1.f / 60); fluid_sim_step(s, 1.f / 60);
            fluid_sim_color(s, 1.f / 30);
            if (crisp) cpk_paint(k, s); else fluid_sim_view(s);
            fluid_sim_snapshot(s, fr, false);
            fluid_draw_opts_t o = { FLUID_PAL_PASTEL, FLUID_EDGE_ALL, 0.12f, t, 0.f, NULL, 0, " .:-=+*#", font, NULL, false, 0.f, NULL, 0, NULL };
            /* the drawer's options as mole_fluid.c sets them for a rigged creature: its cells as a scene's dots, with no
             * scene held (scene_hd_t at rest) */
            const scene_hd_t hd = { 0, false, 1.f };
            if (crisp) { const uint32_t *cc; cpk_cells(k, &o.dots, &cc); o.scene = hd.gen != 0 || crisp; o.dust = scene_hd_dust(&hd); }
            int y0, y1;
            fluid_draw(d, fr, fb, &o, &y0, &y1);
            if (cpk_tris(k) > worst) worst = cpk_tris(k);
        }
        if (crisp) {                       /* its dots: an outline, features, the floor and dust */
            const uint8_t *cd; const uint32_t *cc; int dots = 0;
            cpk_cells(k, &cd, &cc);
            for (int q = 0; q < W * H; q++) dots += __builtin_popcount(cd[q]);
            CHECK(dots > 200, "%s %s: only %d dots", id, MOOD_NAMES[m], dots);
            int lit = 0;                   /* ...and they reach the panel: the drawn frame is not black */
            for (int q = 0; q < FLUID_STAGE_REF * FLUID_STAGE_REF; q++) lit += fb[q] != 0;
            CHECK(lit > 1000, "%s %s: the drawn frame has only %d lit pixels", id, MOOD_NAMES[m], lit);
            eyes++; worst = 1000;
            printf("  %-8s %-10s dots %5d\n", id, MOOD_NAMES[m], dots);
            if (outdir) { snprintf(path, sizeof(path), "%s/%s_%s.ppm", outdir, id, MOOD_NAMES[m]); write_ppm(path, fb); }
            continue;
        }
        /* the body only revisits the cells it or the marks wrote: re-rastered now (last frame's
         * marks still in its cells), it matches a body rastered from nothing at this pose */
        {
            cb_t *fresh = cb_create(cpk_player(k)), *live = (cb_t *)cpk_body(k);
            const mood_engine_t *e = cpk_engine(k);
            cb_raster(live, cpk_player(k), W, H, e->cam_zoom, e->cam_focus);
            cb_raster(fresh, cpk_player(k), W, H, e->cam_zoom, e->cam_focus);
            const float *c1[5], *c2[5]; const uint8_t *h1, *h2;
            cb_cells(live, &c1[0], &c1[1], &c1[2], &c1[3], &c1[4], &h1);
            cb_cells(fresh, &c2[0], &c2[1], &c2[2], &c2[3], &c2[4], &h2);
            int stale = 0;
            for (int q = 0; q < W * H; q++) {
                stale += h1[q] != h2[q];
                for (int a = 0; a < 5; a++) stale += c1[a][q] != c2[a][q];
            }
            CHECK(stale == 0, "%s %s: %d cell values differ from a body rastered afresh", id, MOOD_NAMES[m], stale);
            cb_destroy(fresh);
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

/* standing, the feet stay where they are: idle, every mood and every gesture with the walk at
 * zero, the feet (the lowest part of each foot or shin bone, as the device draws it) keep their
 * mean x on the glass within PLANT_PX of where they stood, a lifted foot counted where it left
 * the floor. A foot may move in the air and a leg may fold; only walking carries the body.
 * PLANT_PX is a glyph cell (412 / 52 px): less than the device can draw as a move. */
#define PLANT_PX 8.f
#define LIFT     0.01f    /* above the lowest foot by more than this (body units, ~3 px): in the air */
static void test_planted(const char *pkgdir, const char *id) {
    char path[512]; size_t len;
    snprintf(path, sizeof(path), "%s/%s/%s.json", pkgdir, id, id);
    char *json = slurp(path, &len);
    const char *err = NULL;
    cpk_t *k = json ? cpk_create(json, len, 77, &err) : NULL;
    CHECK(k != NULL, "%s: create (%s)", id, err ? err : "no file");
    if (!k) { free(json); return; }
    fluid_sim_t *s = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    mood_engine_t *e = cpk_engine(k);
    const cp_body_t *B = cp_body(cpk_player(k));
    int foot[16], nf = 0;                    /* a part per foot: the lowest on its bone */
    float rest[16][2], held[16], worst = 0;
    char where[48] = "";
    /* idle, then each mood and back, then each gesture from idle */
    for (int seg = -1; seg < CUTE_N_MOODS - 1 + CUTE_N_ACTS + cp_own_gestures(cpk_player(k)); seg++) {   /* its own gestures too */
        const int mood = seg >= 0 && seg < CUTE_N_MOODS - 1 ? seg + 1 : CM_IDLE, act = seg >= CUTE_N_MOODS - 1 ? seg - (CUTE_N_MOODS - 1) : -1;
        me_set_mood(e, mood);
        if (act >= 0) me_act(e, act);
        const int frames = act >= 0 ? (int)((me_act_dur(e, act) + 0.5f) * 30) : 6 * 30;
        float far = 0;
        for (int f = 0; f < frames; f++) {
            cpk_frame(k, s, 1.f / 30.f, 1.f);
            const cb_t *b = cpk_body(k);
            if (!nf) for (int i = 0; i < B->nbones; i++) {
                if (strncmp(B->bones[i].id, "foot", 4) && strncmp(B->bones[i].id, "shin", 4)) continue;
                int lo = -1; float ylo = -1, g[3];
                for (int q = 0; q < B->nparts; q++) if (B->parts[q].bone == i) { cb_glass(b, i, B->parts[q].pos, g); if (g[1] > ylo) { ylo = g[1]; lo = q; } }
                if (lo >= 0 && nf < 16) foot[nf++] = lo;
            }
            float g[16][3], floor = 1e9f, dx = 0;
            for (int i = 0; i < nf; i++) {
                cb_glass(b, B->parts[foot[i]].bone, B->parts[foot[i]].pos, g[i]);
                g[i][0] *= FLUID_STAGE_REF;
                if (seg < 0 && f == 0) { rest[i][0] = g[i][0]; rest[i][1] = g[i][2]; held[i] = g[i][0]; }
                floor = fminf(floor, g[i][2] - rest[i][1]);
            }
            for (int i = 0; i < nf; i++) {   /* on the floor (no higher than the lowest foot); in the air it counts where it left */
                if (g[i][2] - rest[i][1] <= floor + LIFT) held[i] = g[i][0];
                dx += (held[i] - rest[i][0]) / nf;
            }
            far = fmaxf(far, fabsf(dx));
        }
        const char *name = act >= 0 ? cp_gesture_name(cpk_player(k), act) : MOOD_NAMES[mood];
        if (far > worst) { worst = far; snprintf(where, sizeof(where), "%s", name); }
        printf("  %-8s planted %-12s feet moved %4.1f px\n", id, name, far);
    }
    CHECK(nf >= 2, "%s: %d feet found", id, nf);
    CHECK(worst <= PLANT_PX, "%s: standing, the feet slid %.1f px (%s); at most %.0f", id, worst, where, PLANT_PX);
    fluid_sim_destroy(s); cpk_destroy(k); free(json);
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

/* cp_info lists every gesture and mood the package plays and carries its about, line for line */
static void test_info(const char *pkgdir, const char *id) {
    char path[512]; size_t len;
    snprintf(path, sizeof(path), "%s/%s/%s.json", pkgdir, id, id);
    char *json = slurp(path, &len);
    const char *err = NULL;
    cp_t *c = json ? cp_create(json, len, &err) : NULL;
    CHECK(c != NULL, "%s: create (%s)", id, err ? err : "no file");
    if (!c) { free(json); return; }
    cJSON *pkg = cJSON_Parse(json), *info = cJSON_Parse(cp_info(c));
    const cJSON *cr = cJSON_GetObjectItem(pkg, "creature"), *ab = cJSON_GetObjectItem(info, "about");
    CHECK(info && !strcmp(cJSON_GetObjectItem(info, "id")->valuestring, id), "%s: info id", id);
    CHECK(!strcmp(cJSON_GetObjectItem(info, "name")->valuestring, cp_name(c)), "%s: info name", id);
    CHECK(cJSON_Compare(ab, cJSON_GetObjectItem(cr, "about"), true), "%s: info about is not the package's", id);
    const cJSON *g = cJSON_GetObjectItem(info, "gestures"), *m = cJSON_GetObjectItem(info, "moods"), *e;
    CHECK(cJSON_GetArraySize(g) == cJSON_GetArraySize(cJSON_GetObjectItem(cr, "gestures")), "%s: %d gestures listed", id, cJSON_GetArraySize(g));
    cJSON_ArrayForEach(e, g) CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(cr, "gestures"), e->valuestring) && cJSON_GetObjectItem(cJSON_GetObjectItem(ab, "gestures"), e->valuestring), "%s: gesture %s", id, e->valuestring);
    CHECK(cJSON_GetArraySize(m) >= 1 && !strcmp(cJSON_GetArrayItem(m, 0)->valuestring, "idle"), "%s: idle first", id);
    int own = 0;
    cJSON_ArrayForEach(e, m) { own += !!cJSON_GetObjectItem(cJSON_GetObjectItem(cr, "moods"), e->valuestring); CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(ab, "moods"), e->valuestring) != NULL, "%s: no about for mood %s", id, e->valuestring); }
    CHECK(own == cJSON_GetArraySize(cJSON_GetObjectItem(cr, "moods")), "%s: %d of its moods listed", id, own);
    cJSON_Delete(pkg); cJSON_Delete(info);
    cp_destroy(c);
    free(json);
}

/* a gesture of the package's own (the cat's eat, its clips in the rig): found by name, listed for agents, played by the
 * engine and routed by the clip player to its clip; a name it lacks is -1 */
/* a rigged creature on a 466 px panel (the 1.75C) fed only the windows fluid_draw_send makes, as mole_fluid.c drives
 * it: every frame it shows what a drawer redrawing every cell shows, or showed a frame before (the skipped cells are
 * the unchanged ones; the water's own changes go every other frame),
 * sitting, through a tap's ripple and walking (the floor stays put); and the windows stay on even rows (that panel's)
 * and cost well under a full frame */
typedef struct { uint16_t *px; int w, h, odd, n; struct { int x, y, w, h; const uint16_t *px; } q; } crisp_panel_t;
/* the panel as the CO5300's asynchronous blit: a window's pixels are read when the next window is sent (or at the
 * wait), so a drawer that drew into the buffer still being read shows it here */
static void crisp_land(crisp_panel_t *p) {
    if (!p->q.px) return;
    for (int r = 0; r < p->q.h; r++) memcpy(p->px + (size_t)(p->q.y + r) * p->w + p->q.x, p->q.px + (size_t)r * p->q.w, (size_t)p->q.w * 2);
    p->q.px = NULL;
}
static int crisp_send(void *ctx, int x, int y, int w, int h, const uint16_t *px) {
    crisp_panel_t *p = ctx;
    if (x < 0 || y < 0 || x + w > p->w || y + h > p->h) return 1;
    p->odd += (y & 1) || (h & 1);
    p->n++;
    crisp_land(p);
    p->q.x = x; p->q.y = y; p->q.w = w; p->q.h = h; p->q.px = px;
    return 0;
}
static void test_crisp_panel(const char *pkgdir) {
    char path[512]; size_t len;
    snprintf(path, sizeof(path), "%s/cat/cat.json", pkgdir);
    char *json = slurp(path, &len);
    const char *err = NULL;
    cpk_t *k = json ? cpk_create(json, len, 7, &err) : NULL;
    CHECK(k != NULL, "crisp panel: cat (%s)", err ? err : "no file");
    if (!k) { free(json); return; }
    const int P = 466, MAXPX = 4096;                 /* mole_fluid.c's two windows (FLUID_WIN2_PX) */
    int W, H; fluid_grid_for_stage(P, &W, &H);
    fluid_sim_t *s = fluid_sim_create(W, H, P);
    fluid_draw_t *d = fluid_draw_create(W, H, P, P), *ref = fluid_draw_create(W, H, P, P);
    fluid_frame_t *fr = fluid_frame_create(W, H);
    uint16_t *truth = calloc((size_t)P * P, 2), *prev = calloc((size_t)P * P, 2), *buf = malloc((size_t)MAXPX * 2), *buf2 = malloc((size_t)MAXPX * 2);
    crisp_panel_t pn = { calloc((size_t)P * P, 2), P, P, 0, 0, { 0 } };
    int walk = -1;
    for (int g = 0; g < CUTE_N_ACTS + cp_own_gestures(cpk_player(k)); g++) if (!strcmp(cp_gesture_name(cpk_player(k), g), "walk")) walk = g;
    CHECK(walk >= 0, "crisp panel: the cat walks");
    const scene_hd_t hd = { 0, false, 1.f };
    const fluid_overlay_t ov = { 0 };
    int bad = -1; long sent[2] = { 0, 0 }, drawn[2] = { 0, 0 }, wins[2] = { 0, 0 }; int nf[2] = { 0, 0 };
    for (int f = 0; f < 240 && bad < 0; f++) {
        if (f == 100) me_act(cpk_engine(k), walk);
        if (f == 40) fluid_sim_impulse(s, 0.3f, 0.2f, 1.6f, 0.09f);      /* a tap */
        cpk_frame(k, s, 1.f / 30.f, 1.f);
        s->p.still = true;
        fluid_sim_step(s, 1.f / 60); fluid_sim_step(s, 1.f / 60);
        fluid_sim_color(s, 1.f / 30);
        cpk_paint(k, s);
        fluid_sim_snapshot(s, fr, false);
        fluid_draw_opts_t o = { FLUID_PAL_PASTEL, FLUID_EDGE_ALL, 0.12f, f / 30.f, 0.f, NULL, 0, " .:-=+*#", font, NULL, false, 0.f, NULL, 0, NULL };
        const uint32_t *cc; cpk_cells(k, &o.dots, &cc); o.scene = true; o.dust = scene_hd_dust(&hd);
        const bool full = f == 0;
        if (full) d->force = true;
        const int n = fluid_draw_cells(d, fr, &o);
        const int n0 = pn.n;
        const int px = fluid_draw_send2(d, &o, &ov, FLUID_MARK_NONE, full, buf, buf2, MAXPX, crisp_send, &pn);
        crisp_land(&pn);                           /* the wait */
        CHECK(px >= 0, "crisp panel: send failed at frame %d", f);
        ref->force = true; fluid_draw_cells(ref, fr, &o);
        const fluid_rect_t all = { truth, P, 0, 0, P, P };
        fluid_draw_rect(ref, &o, &all, true);
        /* each pixel as a full redraw shows it now, or did the frame before: the water's own changes go every other frame
         * (interlaced), the creature's at once */
        for (size_t q = 0; q < (size_t)P * P && bad < 0; q++) if (pn.px[q] != truth[q] && (f == 0 || pn.px[q] != prev[q])) bad = f;
        memcpy(prev, truth, (size_t)P * P * 2);
        const int ph = f >= 130 ? 1 : f >= 70 && f < 100 ? 0 : -1;    /* settled: sitting, then walking */
        if (ph >= 0) { sent[ph] += px; drawn[ph] += n; wins[ph] += pn.n - n0; nf[ph]++; }
    }
    CHECK(bad < 0, "crisp panel: the panel differs from a full redraw at frame %d", bad);
    CHECK(pn.odd == 0, "crisp panel: %d windows on odd rows", pn.odd);
    for (int q = 0; q < 2; q++) {
        const double avg = nf[q] ? (double)sent[q] / nf[q] : 0;
        printf("  crisp panel %s: %.0f px sent a frame (%.0f%% of the panel) in %.0f windows, %.0f cells drawn\n", q ? "walking" : "sitting",
               avg, 100.0 * avg / (P * P), nf[q] ? (double)wins[q] / nf[q] : 0, nf[q] ? (double)drawn[q] / nf[q] : 0);
        /* a window costs about 2500 px (crisp_send): the whole send, in those, well under a full frame's */
        const double wn = nf[q] ? (double)wins[q] / nf[q] : 0;
        CHECK(avg + 2500 * wn < 0.5 * P * P, "crisp panel %s: %.0f px in %.0f windows a frame", q ? "walking" : "sitting", avg, wn);
    }
    free(truth); free(prev); free(buf); free(buf2); free(pn.px); fluid_frame_destroy(fr); fluid_draw_destroy(d); fluid_draw_destroy(ref); fluid_sim_destroy(s);
    cpk_destroy(k); free(json);
}

/* a listen's ring (and the agent's progress ring) over a rigged creature, as mole_fluid.c runs a frame (the creature, the
 * rings into the target, the water's steps, colour, paint): its wave runs out from the centre and its crest reaches the
 * rim, the ring lands there within 1.4 s (a line, a band thinning inward behind it), never popping; let go, it is gone
 * within about a second; and a forming or releasing frame costs the panel no more than a touch's ripple */
static int ring_dots(const cpk_t *k, const fluid_sim_t *s, float r0, float r1, int *all) {
    const uint8_t *cells; const uint32_t *cc; cpk_cells(k, &cells, &cc);
    int lit = 0, n = 0;
    for (int j = 0; j < s->H; j++) for (int i = 0; i < s->W; i++) {
        const int c = j * s->W + i;
        if (!s->mask[c]) continue;
        const fluid_rgba16_t *q = &s->col[c];
        const bool red = q->r > 2 * q->g && q->r > 2 * q->b && q->r > 40 * 257;
        for (int b = 0; b < 8; b++) {
            const int col = b >> 2, row = b & 3;
            const float x = s->xs[i] + (col ? 0.5f : -0.5f) / s->W, y = s->ys[j] + ((row + 0.5f) / 4.f - 0.5f) * 2.f / s->H, r = sqrtf(x * x + y * y);
            if (r < r0 || r >= r1) continue;
            n++;
            if (red && (cells[c] >> fluid_braille_bit(col, row) & 1)) lit++;
        }
    }
    if (all) *all = n;
    return lit;
}
typedef struct { fluid_draw_t *d; fluid_frame_t *fr; uint16_t *buf, *buf2; crisp_panel_t pn; } ring_panel_t;
static double ring_send(ring_panel_t *rp, const cpk_t *k, fluid_sim_t *s, int f) {   /* this frame to the panel, ~ms (80 MHz) */
    fluid_sim_snapshot(s, rp->fr, false);
    fluid_draw_opts_t o = { FLUID_PAL_PASTEL, FLUID_EDGE_ALL, 0.12f, f / 30.f, 0.f, NULL, 0, " .:-=+*#", font, NULL, false, 0.f, NULL, 0, NULL };
    const uint32_t *cc; cpk_cells(k, &o.dots, &cc); o.scene = true;
    const fluid_overlay_t ov = { 0 };
    if (f == 0) rp->d->force = true;
    fluid_draw_cells(rp->d, rp->fr, &o);
    const int n0 = rp->pn.n, px = fluid_draw_send2(rp->d, &o, &ov, FLUID_MARK_NONE, f == 0, rp->buf, rp->buf2, 4096, crisp_send, &rp->pn);
    crisp_land(&rp->pn);
    return f == 0 ? 0.0 : px / 3900.0 + 0.25 * (rp->pn.n - n0);     /* measured: ~3.9 px a us, ~0.25 ms a window */
}
static void ring_frame(cpk_t *k, fluid_sim_t *s, fluid_ring_t *ring, bool on) {
    cpk_frame(k, s, 1.f / 30.f, 1.f);
    fluid_ring_tick(ring, 1.f / 30.f, on);
    fluid_target_ring(s, ring, 0xff2a2a, 0.f);
    s->p.still = true;
    fluid_sim_step(s, 1.f / 60); fluid_sim_step(s, 1.f / 60);
    fluid_sim_color(s, 1.f / 30);
    cpk_paint(k, s);
}
static void test_crisp_ring(const char *pkgdir) {
    char path[512]; size_t len;
    snprintf(path, sizeof(path), "%s/cat/cat.json", pkgdir);
    char *json = slurp(path, &len);
    const char *err = NULL;
    cpk_t *k = json ? cpk_create(json, len, 3, &err) : NULL;
    CHECK(k != NULL, "crisp ring: cat (%s)", err ? err : "no file");
    if (!k) { free(json); return; }
    const int P = 466; int W, H; fluid_grid_for_stage(P, &W, &H);
    fluid_sim_t *s = fluid_sim_create(W, H, P);
    ring_panel_t rp = { fluid_draw_create(W, H, P, P), fluid_frame_create(W, H), malloc(4096 * 2), malloc(4096 * 2), { calloc((size_t)P * P, 2), P, P, 0, 0, { 0 } } };
    fluid_ring_t ring = { 0 }; fluid_progress_t prog = { 0 }; prog.pct = -1.f;
    /* a finger's touch at the centre first: its ripple's heaviest frame (running out to the rim) is the budget the
     * ring's wave may take */
    double touch = 0.0;
    for (int f = 0; f < 80; f++) {
        if (f == 20) fluid_sim_impulse(s, 0.f, 0.f, 1.6f, 0.09f);
        ring_frame(k, s, &ring, false);
        const double ms = ring_send(&rp, k, s, f);
        if (f > 20 && ms > touch) touch = ms;
    }
    fluid_sim_destroy(s); s = fluid_sim_create(W, H, P);   /* the ring on calm water */
    cpk_destroy(k); k = cpk_create(json, len, 3, &err);
    ring = (fluid_ring_t){ 0 };
    enum { N = 150, ON0 = 10, OFF0 = 90 };
    int line[N] = { 0 }, all[N] = { 0 }, crest = -1, nline = 1;
    double heavy = 0.0;
    for (int f = 0; f < N; f++) {                  /* 10 frames none, 80 a listen, 60 after it */
        ring_frame(k, s, &ring, f >= ON0 && f < OFF0);
        line[f] = ring_dots(k, s, 0.962f, 1.f, &nline);
        all[f] = ring_dots(k, s, 0.08f, 1.f, NULL);
        if (crest < 0 && f >= ON0 && ring_dots(k, s, 0.8f, 0.962f, NULL) > 0 && line[f] == 0) crest = f;   /* red near the rim, the line not yet */
        const double ms = ring_send(&rp, k, s, 70 + f);
        if ((f >= ON0 && f < ON0 + 45) || (f >= OFF0 && f < OFF0 + 45)) if (ms > heavy) heavy = ms;
        if (f == OFF0 - 1) {                       /* settled: the line, thinning inward behind it */
            int n0, n1, n2, n3, n4;
            const int a0 = ring_dots(k, s, 0.962f, 0.988f, &n0), a1 = ring_dots(k, s, 0.9f, 0.962f, &n1), a2 = ring_dots(k, s, 0.82f, 0.9f, &n2),
                      a3 = ring_dots(k, s, 0.74f, 0.82f, &n3), a4 = ring_dots(k, s, 0.08f, 0.64f, &n4);
            const double f0 = (double)a0 / n0, f1 = (double)a1 / n1, f2 = (double)a2 / n2, f3 = (double)a3 / n3;
            printf("  crisp ring, settled: lit %.2f on the line, %.2f, %.2f, %.2f inward, %d dots inside\n", f0, f1, f2, f3, a4);
            CHECK(f0 > 0.6, "crisp ring: the line %.2f lit", f0);
            CHECK(f1 < f0 && f2 < f1 && f3 <= f2 && f1 < 0.6, "crisp ring: not thinning inward (%.2f %.2f %.2f %.2f)", f0, f1, f2, f3);
            CHECK(a4 == 0, "crisp ring: %d dots inside the band", a4);
        }
    }
    const int full = line[OFF0 - 1];
    int formed = -1, gone = -1, jump = 0;
    for (int f = ON0; f < OFF0 && formed < 0; f++) if (line[f] >= 0.8 * full) formed = f;
    for (int f = OFF0; f < N && gone < 0; f++) if (all[f] == 0) gone = f;
    for (int f = ON0 + 1; f < N; f++) { const int dd = abs(line[f] - line[f - 1]); if (dd > jump) jump = dd; }
    printf("  crisp ring: the crest near the rim at %.2f s, the line formed at %.2f s, gone %.2f s after; largest step %d of %d line dots\n",
           crest < 0 ? -1.0 : (crest - ON0) / 30.0, formed < 0 ? -1.0 : (formed - ON0) / 30.0, gone < 0 ? -1.0 : (gone - OFF0) / 30.0, jump, full);
    printf("  crisp ring: heaviest forming / releasing frame ~%.1f ms on the panel, a touch's %.1f ms\n", heavy, touch);
    CHECK(line[ON0 - 1] == 0 && all[ON0 - 1] == 0, "crisp ring: dots before a listen");
    CHECK(crest >= 0 && crest < formed, "crisp ring: no crest reaching the rim before the line");
    CHECK(formed >= 0 && formed - ON0 <= 33, "crisp ring: formed only %.2f s after the start", (formed - ON0) / 30.0);
    CHECK(gone >= 0 && gone - OFF0 <= 33, "crisp ring: still there %.2f s after", gone < 0 ? 99.0 : (gone - OFF0) / 30.0);
    CHECK(jump < full / 3, "crisp ring: pops (a step of %d of %d line dots)", jump, full);
    /* the panel's cost (reported, not yet held to a touch's): a wave across the whole disc changes every cell row,
     * ~0.6 ms a row on the CO5300's windows; see the report */
    int lit = 0;                                   /* the progress comet shows too */
    for (int f = 0; f < 60; f++) {
        cpk_frame(k, s, 1.f / 30.f, 1.f);
        fluid_progress_tick(&prog, 1.f / 30.f, true, -1.f, false);
        fluid_target_progress(s, &prog, 0x2a6aff, 0.f);
        fluid_sim_step(s, 1.f / 60); fluid_sim_step(s, 1.f / 60);
        fluid_sim_color(s, 1.f / 30);
        cpk_paint(k, s);
    }
    for (int c = 0; c < s->N; c++) { const fluid_rgba16_t *q = &s->col[c]; if (q->b > 2 * q->r && q->b > 40 * 257) lit++; }
    printf("  crisp ring, progress: %d cells\n", lit);
    CHECK(lit > 5, "crisp ring: the progress comet does not show (%d cells)", lit);
    free(rp.buf); free(rp.buf2); free(rp.pn.px); fluid_frame_destroy(rp.fr); fluid_draw_destroy(rp.d); fluid_sim_destroy(s); cpk_destroy(k); free(json);
}

static void test_own_rigged(const char *pkgdir) {
    char path[512]; size_t len;
    snprintf(path, sizeof(path), "%s/cat/cat.json", pkgdir);
    char *json = slurp(path, &len);
    const char *err = NULL;
    cpk_t *k = json ? cpk_create(json, len, 5, &err) : NULL;
    CHECK(k != NULL, "cat: create (%s)", err ? err : "no file");
    if (!k) { free(json); return; }
    const cp_t *c = cpk_player(k);
    const int eat = cp_gesture(c, "eat");
    CHECK(cpk_crisp(k) && cp_rigged(c), "cat: rigged");
    CHECK(eat >= CUTE_N_ACTS && !strcmp(cp_gesture_name(c, eat), "eat"), "cat: eat is its own gesture (%d)", eat);
    CHECK(cp_gesture(c, "nod") == CA_NOD && cp_gesture(c, "fly") < 0, "cat: gesture lookup");
    CHECK(strstr(cp_info(c), "\"eat\"") != NULL, "cat: eat is not in its info");
    fluid_sim_t *s = fluid_sim_create(52, 32, FLUID_STAGE_REF);
    mood_engine_t *e = cpk_engine(k);
    const cc_t *cl = cpk_clip(k);
    cpk_frame(k, s, 1.f / 30, 1.f);
    me_act(e, eat);
    bool played = false;
    for (int f = 0; f < 400; f++) { cpk_frame(k, s, 1.f / 30, 1.f); float t; played |= !strcmp(cc_clip_name(cl, cc_clip(cl, &t)), "eat"); }
    CHECK(played, "cat: the eat clip never played");
    fluid_sim_destroy(s); cpk_destroy(k); free(json);
}

int main(int argc, char **argv) {
    if (argc < 4) { printf("usage: creature_test <packages dir> <golden dir> <duck.molty> [out dir]\n"); return 2; }
    printf("creature player host test\n");
    test_hash();
    for (const char *const *id = (const char *const[]){ "duck", "cat", "octopus", NULL }; *id; id++) test_golden(argv[1], argv[2], *id);
    test_strict(argv[1]);
    test_own_rigged(argv[1]);
    test_crisp_panel(argv[1]);
    test_crisp_ring(argv[1]);
    for (const char *const *id = (const char *const[]){ "duck", "cat", "octopus", NULL }; *id; id++) test_info(argv[1], *id);
    for (const char *const *id = (const char *const[]){ "duck", NULL }; *id; id++) test_planted(argv[1], *id);   /* the cat: clip_test.c */
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
