/*
 * clip_test.c: the rigged creature player and its dots (main/creature/creature_clip.c, creature_dots.c) against the JS
 * reference (device/creatures/js/clip-player.js, clip-dots.js): the goldens tools/creatures/clip-golden.mjs writes, frame
 * by frame (what plays, the world matrices, the camera, and every 10th frame's braille cells). Also: every ordered pair of
 * pose families has an authored route, a broken rig is refused, and the cost of a frame.
 * Build + run: test/host/clip_build.sh
 */
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

static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); const long L = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)L + 1);
    if (fread(b, 1, (size_t)L, f) != (size_t)L) { free(b); fclose(f); return NULL; }
    b[L] = 0; fclose(f); *n = (size_t)L;
    return b;
}
static double now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }

static cc_t *load(const char *pkgdir, const char *id, cJSON **jout) {
    char path[512]; size_t len;
    snprintf(path, sizeof(path), "%s/%s/%s.json", pkgdir, id, id);
    char *json = slurp(path, &len);
    if (!json) return NULL;
    cJSON *j = cJSON_ParseWithLength(json, len);
    free(json);
    const char *err = NULL;
    cc_t *c = j ? cc_create(cJSON_GetObjectItemCaseSensitive(j, "creature"), &err) : NULL;
    CHECK(c != NULL, "%s: cc_create (%s)", id, err ? err : "json");
    if (jout) *jout = j; else cJSON_Delete(j);
    return c;
}

static void test_routes(cc_t *c, const char *id) {
    const int F = cc_families(c);
    printf("  %s routes (from row to column):\n", id);
    for (int a = 0; a < F; a++) {
        printf("    %-7s", cc_family_name(c, a));
        for (int b = 0; b < F; b++) {
            if (a == b) { printf(" %-28s", "-"); continue; }
            int r[8]; const int n = cc_route(c, a, b, r, 8);
            CHECK(n >= 1 && n <= 2, "%s: %s -> %s: %d hops", id, cc_family_name(c, a), cc_family_name(c, b), n);
            if (n >= 1) {
                bool chain = cc_clip_from(c, r[0]) == a && cc_clip_to(c, r[n - 1]) == b;
                for (int i = 1; i < n; i++) chain &= cc_clip_from(c, r[i]) == cc_clip_to(c, r[i - 1]);
                CHECK(chain, "%s: %s -> %s: the clips do not chain", id, cc_family_name(c, a), cc_family_name(c, b));
                char hop[64]; snprintf(hop, sizeof(hop), "%s%s%s", cc_clip_name(c, r[0]), n > 1 ? "+" : "", n > 1 ? cc_clip_name(c, r[1]) : "");
                printf(" %-28s", hop);
            }
        }
        printf("\n");
    }
}

static void test_golden(const char *pkgdir, const char *gdir, const char *id) {
    char path[512]; size_t len;
    snprintf(path, sizeof(path), "%s/%s.clip.golden", gdir, id);
    char *g = slurp(path, &len);
    CHECK(g != NULL, "%s: no golden", id);
    if (!g) return;
    cc_t *c = load(pkgdir, id, NULL);
    if (!c) { free(g); return; }
    test_routes(c, id);
    cd_t *d = cd_create(c, 52, 32);
    const int N = cc_nodes(c);
    int frame = 0, bad_clip = 0, frames = 0, bits_on = 0, bits_off = 0;
    double worst_w = 0, worst_t = 0, worst_cam = 0, us_play = 0, us_draw = 0; int draws = 0;
    char *save = NULL;
    for (char *line = strtok_r(g, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (line[0] == 'm') cc_mood(c, line + 2);
        else if (line[0] == 'a') cc_act(c, line + 2);
        else if (line[0] == 'c' && !line[1]) cc_come(c, NULL);
        else if (line[0] == 'c' && line[1] == ' ') cc_come(c, line + 2);
        else if (line[0] == 'f') {
            double t0 = now_us();
            cc_update(c, 1.f / 30.f);
            us_play += now_us() - t0;
            char name[64]; double t, cx, cz, cy; int off = 0;
            sscanf(line + 2, "%63s %lf %lf %lf %lf%n", name, &t, &cx, &cz, &cy, &off);
            float ct; const int k = cc_clip(c, &ct);
            if (strcmp(cc_clip_name(c, k), name)) { if (bad_clip++ < 3) printf("  %s frame %d: plays %s, the reference %s\n", id, frame, cc_clip_name(c, k), name); }
            else {
                worst_t = fmax(worst_t, fabs(ct - t));
                float x, z, yaw; cc_cam(c, &x, &z, &yaw);
                worst_cam = fmax(worst_cam, fmax(fabs(x - cx), fmax(fabs(z - cz), fabs(yaw - cy) / 100.0)));
                const char *p = line + 2 + off; const float *w = cc_world(c);
                for (int i = 0; i < N * 12; i++) { char *e; const double v = strtod(p, &e); p = e; worst_w = fmax(worst_w, fabs(v - w[i])); }
            }
            frame++; frames++;
        } else if (line[0] == 'd') {
            double t0 = now_us();
            cd_draw(d, c, (uint32_t)(frame - 1));
            us_draw += now_us() - t0; draws++;
            const uint8_t *cells; const uint32_t *rgb; cd_cells(d, &cells, &rgb);
            const char *h = line + 2;
            for (int q = 0; q < 52 * 32 && h[2 * q]; q++) {
                unsigned v; sscanf(h + 2 * q, "%2x", &v);
                bits_on += __builtin_popcount(v);
                bits_off += __builtin_popcount(v ^ cells[q]);
            }
        }
    }
    printf("  %s: %d frames, clip mismatches %d, worst |t| %.2g, world %.2g, camera %.2g; dots differ %d of %d (%.2f%%)\n",
           id, frames, bad_clip, worst_t, worst_w, worst_cam, bits_off, bits_on, 100.0 * bits_off / (bits_on ? bits_on : 1));
    CHECK(bad_clip == 0, "%s: %d frames play another clip than the reference", id, bad_clip);
    CHECK(worst_t < 1e-3 && worst_w < 2e-3 && worst_cam < 2e-3, "%s: the pose drifts from the reference (t %.2g, world %.2g, cam %.2g)", id, worst_t, worst_w, worst_cam);
    CHECK(bits_off <= bits_on / 50, "%s: %d of %d dots differ from the reference", id, bits_off, bits_on);
    printf("  %s: host %.1f us a frame to play, %.1f us to draw the dots\n", id, us_play / frames, us_draw / (draws ? draws : 1));
    cd_destroy(d); cc_destroy(c); free(g);
}

/* a rig that breaks a rule is refused with a reason, never half loaded */
static void test_strict(const char *pkgdir) {
    cJSON *j = NULL;
    cc_t *c = load(pkgdir, "cat", &j);
    if (!c) { cJSON_Delete(j); return; }
    cc_destroy(c);
    cJSON *rig = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "creature"), "rig");
    struct { const char *what; void (*f)(cJSON *); } cases[] = {
        { "a node before its parent", NULL }, { "an edge to a missing clip", NULL }, { "a channel on a missing node", NULL },
    };
    (void)cases;
    const char *err = NULL;
    /* 1: a node whose parent comes later */
    cJSON *n1 = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(rig, "nodes"), 1), *par = cJSON_GetObjectItemCaseSensitive(n1, "parent");
    const double keep = par->valuedouble; cJSON_SetNumberValue(par, 5);
    c = cc_create(cJSON_GetObjectItemCaseSensitive(j, "creature"), &err);
    CHECK(!c && err && strstr(err, "parent"), "a node before its parent: %s", err ? err : "accepted");
    cc_destroy(c); cJSON_SetNumberValue(par, keep);
    /* 2: an edge naming a clip it lacks */
    cJSON *edges = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(rig, "graph"), "edges");
    cJSON_AddStringToObject(edges, "sit>stand", "no_such_clip");
    c = cc_create(cJSON_GetObjectItemCaseSensitive(j, "creature"), &err);
    CHECK(!c && err, "an edge to a missing clip: %s", err ? err : "accepted");
    cc_destroy(c);
    cJSON_Delete(j);
}

/* a gesture played as a chain (graph.chains): plays it under a seed until the creature is back in its mood, writing the
 * clips it went through; the largest jump of any node but the root from one frame to the next where one link hands over
 * to the next; the root's heading (rad, about up) at the end of the chain and a second later in its mood */
static int play_chain(cc_t *c, uint32_t seed, const char *g, int *seq, int max, float *jump, float *yaw_end, float *yaw_after) {
    cc_seed(c, seed);
    CHECK(cc_act(c, g), "chain: cc_act %s", g);
    const int N = cc_nodes(c);
    float *prev = malloc(sizeof(float) * 12 * (size_t)N);
    int n = 0, last = -1, chain_from = -1; *jump = 0.f; *yaw_end = *yaw_after = 0.f;
    bool in_chain = false, done = false;
    for (int f = 0; f < 30 * 60 && !done; f++) {
        memcpy(prev, cc_world(c), sizeof(float) * 12 * (size_t)N);
        cc_update(c, 1.f / 30.f);
        const int k = cc_clip(c, NULL);
        const char *name = cc_clip_name(c, k);
        const bool link = !strncmp(name, g, strlen(g)) && name[strlen(g)] == '_';
        if (k != last) {
            if (link && in_chain && last >= 0) {      /* a link handing over: nothing but the place may jump */
                const float *w = cc_world(c);
                for (int i = 1; i < N; i++) {
                    if (i == cc_node_find(c, "root") || fabsf(w[i * 12]) + fabsf(w[i * 12 + 4]) + fabsf(w[i * 12 + 8]) < 1e-3f) continue;   /* a hidden prop is nowhere */
                    const float d = fabsf(w[i * 12 + 3] - prev[i * 12 + 3]) + fabsf(w[i * 12 + 7] - prev[i * 12 + 7]) + fabsf(w[i * 12 + 11] - prev[i * 12 + 11]);
                    if (d > *jump) *jump = d;
                }
            }
            if (link) { in_chain = true; if (n < max) seq[n++] = k; }
            else if (in_chain) { *yaw_end = atan2f(-cc_world(c)[8], cc_world(c)[0]); chain_from = f; }
            last = k;
        }
        if (chain_from >= 0 && f == chain_from + 30) { *yaw_after = atan2f(-cc_world(c)[8], cc_world(c)[0]); done = true; }
    }
    free(prev);
    return n;
}

static void test_chains(const char *pkgdir) {
    cJSON *j = NULL;
    cc_t *probe = load(pkgdir, "cat", &j);
    if (!probe) { cJSON_Delete(j); return; }
    const cJSON *chains = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "creature"), "rig"), "graph"), "chains");
    CHECK(chains && cJSON_GetObjectItemCaseSensitive(chains, "yarn"), "chains: the cat's yarn is a chain");
    cc_destroy(probe);
    const char *const G[] = { "yarn", "eat" };
    for (int gi = 0; gi < 2; gi++) {
        const cJSON *h = cJSON_GetObjectItemCaseSensitive(chains, G[gi]);
        if (!h) continue;
        int seqs[8][24], ns[8], distinct = 0, repeats = 0, bad_end = 0;
        float worst = 0.f;
        for (int s = 0; s < 8; s++) {
            cc_t *c = load(pkgdir, "cat", NULL);
            float jump, ye, ya;
            ns[s] = play_chain(c, 1000u + 77u * (uint32_t)s, G[gi], seqs[s], 24, &jump, &ye, &ya);
            if (jump > worst) worst = jump;
            if (fabsf(remainderf(ye - ya, 6.28318530718f)) > 0.05f) bad_end++;       /* it keeps facing the way it ended */
            for (int i = 1; i < ns[s]; i++) if (seqs[s][i] == seqs[s][i - 1]) repeats++;
            bool fresh = true;
            for (int q = 0; q < s; q++) if (ns[q] == ns[s] && !memcmp(seqs[q], seqs[s], sizeof(int) * (size_t)ns[s])) fresh = false;
            distinct += fresh;
            cc_t *again = load(pkgdir, "cat", NULL);       /* the same seed: the same game */
            int seq2[24]; float j2, y2, a2;
            const int n2 = play_chain(again, 1000u + 77u * (uint32_t)s, G[gi], seq2, 24, &j2, &y2, &a2);
            CHECK(n2 == ns[s] && !memcmp(seq2, seqs[s], sizeof(int) * (size_t)n2) && y2 == ye, "chain %s: seed %d plays another game the second time", G[gi], s);
            cc_destroy(c); cc_destroy(again);
        }
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(h, "rounds");
        const int rmax = cJSON_GetArrayItem(r, 1)->valueint;
        printf("  chain %s: 8 seeds, %d different games, links %d..%d, worst jump where links join %.3f\n", G[gi], distinct, ns[0], ns[7], worst);
        for (int s = 0; s < 8; s++) CHECK(ns[s] >= 1 && ns[s] <= rmax + 2, "chain %s: seed %d played %d links", G[gi], s, ns[s]);
        CHECK(!repeats, "chain %s: the same link twice running (%d times)", G[gi], repeats);
        CHECK(worst < 0.03f, "chain %s: links do not join (a node jumps %.3f)", G[gi], worst);
        CHECK(!bad_end, "chain %s: the heading does not carry into its mood (%d of 8)", G[gi], bad_end);
        if (rmax > 1) CHECK(distinct >= 3, "chain %s: only %d different games in 8 seeds", G[gi], distinct);
    }
    /* a chain naming an action the rig lacks is refused */
    cJSON *y = cJSON_GetObjectItemCaseSensitive((cJSON *)chains, "yarn");
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(y, "then"), cJSON_CreateString("no_such_clip"));
    const char *err = NULL;
    cc_t *c = cc_create(cJSON_GetObjectItemCaseSensitive(j, "creature"), &err);
    CHECK(!c && err && strstr(err, "chain"), "a chain with a missing clip: %s", err ? err : "accepted");
    cc_destroy(c);
    cJSON_Delete(j);
}

/* a prop (a root-level node but the first: the bowl, the ball) is never dragged by a crossfade: while it shows, from one
 * frame to the next it moves no more than its own path does (a ball shot away: under 0.25), and once the creature is
 * back in its mood it is gone (scaled to nothing) */
static void test_props(const char *pkgdir) {
    cJSON *j = NULL;
    cc_t *probe = load(pkgdir, "cat", &j);
    if (!probe) { cJSON_Delete(j); return; }
    const cJSON *nodes = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "creature"), "rig"), "nodes");
    int props[8], np = 0, i = 0;
    for (const cJSON *n = nodes->child; n; n = n->next, i++)
        if (i > 0 && cJSON_GetObjectItemCaseSensitive(n, "parent")->valueint < 0 && np < 8) props[np++] = i;
    cc_destroy(probe); cJSON_Delete(j);
    CHECK(np > 0, "props: the cat has its bowl and its ball");
    const char *const G[] = { "eat", "yarn" };
    float worst = 0.f; int left_over = 0;
    for (int gi = 0; gi < 2; gi++)
        for (int seed = 0; seed < 6; seed++) {
            cc_t *c = load(pkgdir, "cat", NULL);
            cc_seed(c, 31u + 97u * (uint32_t)seed);
            for (int f = 0; f < 30; f++) cc_update(c, 1.f / 30.f);
            cc_act(c, G[gi]);
            float prev[8][4]; int back = -1;
            for (int f = 0; f < 30 * 40; f++) {
                cc_update(c, 1.f / 30.f);
                const float *w = cc_world(c);
                for (int q = 0; q < np; q++) {
                    const float *m = w + props[q] * 12, now[4] = { m[3], m[7], m[11], sqrtf(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]) };
                    if (f && now[3] > 0.05f && prev[q][3] > 0.05f) {
                        const float d = sqrtf((now[0] - prev[q][0]) * (now[0] - prev[q][0]) + (now[1] - prev[q][1]) * (now[1] - prev[q][1]) + (now[2] - prev[q][2]) * (now[2] - prev[q][2]));
                        if (d > worst) worst = d;
                    }
                    if (back >= 0 && f > back + 15 && now[3] > 1e-4f) left_over++;
                    memcpy(prev[q], now, sizeof(now));
                }
                const char *name = cc_clip_name(c, cc_clip(c, NULL));
                if (back < 0 && cc_free(c) && strncmp(name, G[gi], strlen(G[gi]))) back = f;
                if (back >= 0 && f > back + 45) break;
            }
            CHECK(back >= 0, "props: %s seed %d never went back to its mood", G[gi], seed);
            cc_destroy(c);
        }
    printf("  props: eat and yarn, 6 seeds each: a showing prop moves at most %.3f a frame; %d frames shown after\n", worst, left_over);
    CHECK(worst < 0.25f, "props: a showing prop jumps %.3f in a frame (dragged by a crossfade)", worst);
    CHECK(!left_over, "props: a prop still shows %d frames after the creature is back in its mood", left_over);
}

int main(int argc, char **argv) {
    if (argc < 3) { printf("usage: clip_test <packages dir> <golden dir>\n"); return 2; }
    printf("rigged creature host test\n");
    test_golden(argv[1], argv[2], "cat");
    test_strict(argv[1]);
    test_chains(argv[1]);
    test_props(argv[1]);
    printf("%d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
