/*
 * SPDX-License-Identifier: MIT
 *
 * ask_flow_test.c: a question coming and going through mole_fluid.c itself (the web module's native build, web_molty.c,
 * included whole), at device speed (wm_device: only what the device sends reaches the panel, each frame as long as the
 * panel model says). Each of cat to question, question to question, and question to cat (cancelled, and answered with a
 * tap) is one change of picture: the old picture dissolves, its dust gathers straight into the new one and is at rest
 * before 1.5 s. Every frame: no pop (at most FRAME_PX_MAX pixels change), at most SLOW_MAX frames over the frame's
 * 33.3 ms in the model; between two questions the screen never shows the bare cat (the drawer is never given its look);
 * the cat comes back by one change, not two. The answers' arcs grow outward over several frames, never lighting whole,
 * and a tap answers only once they are on the glass. Off device speed (the page's own, the whole panel each frame) the
 * question still forms where the ring of water passes.
 *
 *   ask_flow_test <repo root>
 */
#include "../../web/web_molty.c"

static int s_pass, s_fail;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define PX 466
#define LIT 24
/* a frame's pixels changed at most: a change of picture moves at most FLUID_MORPH_FLY_PX px of grains, each cleared and
 * drawn again (4000 px), and turns its still layers a few hundred more (measured 4.4 to 4.9 kpx at the dissolve's
 * height); the old question's forming showed half its picture in one frame (10 to 11.7 kpx) */
#define FRAME_PX_MAX 6000
#define SLOW_MS      33.4f   /* a frame the model says takes longer than the frame clock (30 ms) and a little */
#define SLOW_MAX     2
#define REST_S       1.5f
#define SPAN_BINS    72

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "ask_flow_test: cannot read %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    b[n] = 0; *len = (size_t)n;
    fclose(f);
    return b;
}
static bool lit(const uint8_t *p, int i) { const uint8_t *q = p + (size_t)i * 4; return q[0] >= LIT || q[1] >= LIT || q[2] >= LIT; }

/* one frame at the device's pace: as long as the model said the last one took */
static float s_next_ms = 30.f;
static void frame(void) { wm_frame((uint32_t)(s_next_ms * 1000.f + 0.5f)); s_next_ms = wm_frame_info()[FI_FRAME_MS]; char *e = wm_events(); free(e); }
static void run(float s) { for (float t = 0.f; t < s * 1000.f; t += s_next_ms) frame(); }

/* a transition, frame by frame from the command */
#define MAXF 110
typedef struct {
    int n, px[MAXF], slow, ends, starts, hmax;
    float t[MAXF], ms[MAXF];
    uint32_t look[MAXF];
    unsigned shown[MAXF];
    int morph[MAXF];
    uint8_t *img[MAXF];
} rec_t;
static void record(rec_t *r, float seconds, void (*cmd)(void)) {
    static uint8_t prev[PX * PX * 4];
    memcpy(prev, wm_pixels(), sizeof(prev));
    const int ends0 = s_draw->m.ends;
    cmd();
    float t = 0.f;
    r->n = 0; r->slow = 0; r->starts = 0; r->hmax = 0;
    bool was = fluid_draw_morphing(s_draw);
    while (t < seconds * 1000.f && r->n < MAXF) {
        frame();
        t += r->ms[r->n] = s_next_ms;
        const uint8_t *p = wm_pixels();
        int ch = 0;
        for (int i = 0; i < PX * PX * 4; i++) if ((i & 3) != 3 && p[i] != prev[i]) { ch++; i |= 3; }
        memcpy(prev, p, sizeof(prev));
        const int k = r->n++;
        r->t[k] = t; r->px[k] = ch; r->slow += s_next_ms > SLOW_MS;
        r->look[k] = s_draw->m.look & ~FLUID_LOOK_DUST; r->shown[k] = mole_fluid_ask_shown();
        r->morph[k] = fluid_draw_morphing(s_draw);
        r->starts += r->morph[k] && !was; was = r->morph[k];
        for (int q = 0; q < s_sim->N; q++) if (s_sim->mask[q] && abs(s_sim->h[q]) > r->hmax) r->hmax = abs(s_sim->h[q]);
        free(r->img[k]); r->img[k] = malloc(PX * PX * 4); memcpy(r->img[k], p, PX * PX * 4);
    }
    r->ends = s_draw->m.ends - ends0;
}
/* when the screen is at rest: past the last frame that changed more than 3 x the mean of the last 15 (and 600 px) */
static float rest_at(const rec_t *r) {
    double q = 0.;
    for (int k = r->n - 15; k < r->n; k++) q += r->px[k];
    const double bar = fmax(3. * q / 15., 600.);
    for (int k = r->n - 1; k >= 0; k--) if (r->px[k] > bar) return r->t[k] / 1000.f;
    return 0.f;
}
/* the cat leaving and coming back: the share of its pixels at rest (the last frame) lit, frame by frame; once back (80%)
 * it never goes again (under 50%): it is not dissolved a second time */
static bool cat_once(const rec_t *r) {
    const uint8_t *end = r->img[r->n - 1];
    int all = 0;
    for (int i = 0; i < PX * PX; i++) all += lit(end, i);
    bool back = false;
    for (int k = 0; k < r->n; k++) {
        int on = 0;
        for (int i = 0; i < PX * PX; i++) on += lit(end, i) && lit(r->img[k], i);
        if (on * 100 >= all * 80) back = true;
        else if (back && on * 100 < all * 50) return false;
    }
    return back;
}
static int worst_px(const rec_t *r) { int m = 0; for (int k = 0; k < r->n; k++) if (r->px[k] > m) m = r->px[k]; return m; }

/* the answers' arcs: the rim's pixels lit at rest that were dark before; per frame, the share of their angle lit */
static uint8_t s_arc[PX * PX];
static int s_bins_all;
static void arcs_of(const uint8_t *before, const uint8_t *after) {
    uint8_t all[SPAN_BINS] = { 0 };
    for (int i = 0; i < PX * PX; i++) {
        const float x = (i % PX + 0.5f) / (PX * 0.5f) - 1.f, y = (i / PX + 0.5f) / (PX * 0.5f) - 1.f;
        s_arc[i] = 0;
        if (hypotf(x, y) <= 0.93f || !lit(after, i) || lit(before, i)) continue;
        const int b = (int)((atan2f(x, -y) + 3.14159265f) / 6.2831853f * SPAN_BINS) % SPAN_BINS;
        s_arc[i] = (uint8_t)(b + 1); all[b] = 1;
    }
    s_bins_all = 0;
    for (int b = 0; b < SPAN_BINS; b++) s_bins_all += all[b];
}
static int span(const uint8_t *p) {
    uint8_t on[SPAN_BINS] = { 0 };
    int n = 0;
    for (int i = 0; i < PX * PX; i++) if (s_arc[i] && lit(p, i)) on[s_arc[i] - 1] = 1;
    for (int b = 0; b < SPAN_BINS; b++) n += on[b];
    return s_bins_all ? 100 * n / s_bins_all : 0;
}

static void ask(const char *json) { CHECK(wm_question(json) == 0, "question refused: %s", json); }
static void ask_yesno(void) { ask("{\"kind\":\"ask\",\"job\":\"j1\",\"title\":\"feed the cat?\",\"options\":[\"yes\",\"no\"]}"); }
static void ask_pair(void) { ask("{\"kind\":\"pair\",\"job\":\"j2\",\"title\":\"pair with claude?\",\"detail\":\"check the code matches\",\"code\":\"4821\",\"options\":[\"accept\",\"reject\"]}"); }
static void ask_five(void) { ask("{\"kind\":\"ask\",\"job\":\"j3\",\"title\":\"how was today?\",\"options\":[\"great\",\"good\",\"ok\",\"meh\",\"bad\"]}"); }
static void cancel(void) { wm_question_end("cancelled"); }
static void tap_accept(void) { wm_touch(1, 60, 300); }

/* the transition's common rules: no pop, the frame budget, at rest in time, one change */
static void common(const char *name, const rec_t *r, int changes) {
    const float rest = rest_at(r);
    printf("%s: rest %.2f s, worst frame %d px, slow frames %d, changes %d\n", name, rest, worst_px(r), r->slow, r->ends);
    CHECK(worst_px(r) <= FRAME_PX_MAX, "%s: a frame changed %d px (a pop; at most %d)", name, worst_px(r), FRAME_PX_MAX);
    CHECK(r->slow <= SLOW_MAX, "%s: %d frames over %.1f ms in the panel model (at most %d)", name, r->slow, SLOW_MS, SLOW_MAX);
    CHECK(rest > 0.3f && rest < REST_S, "%s: at rest at %.2f s (under %.1f s)", name, rest, REST_S);
    CHECK(r->ends == changes && r->starts == changes, "%s: %d changes of picture started, %d ended (want %d)", name, r->starts, r->ends, changes);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ask_flow_test <repo root>\n"); return 2; }
    char path[1024];
    size_t len;
    snprintf(path, sizeof(path), "%s/packages/cat/cat.json", argv[1]);
    char *cat = slurp(path, &len);
    if (wm_creature("cat", cat, len) || wm_start(PX, 20260516, 0)) { printf("FAIL: start\n"); return 1; }
    wm_sense(1);
    wm_device(1);
    run(8.f);
    const uint32_t creature = fluid_look_of(FLUID_LOOK_CREATURE, 1u, true) & ~FLUID_LOOK_DUST;
    CHECK((s_draw->m.look & ~FLUID_LOOK_DUST) == creature, "the cat is not on the glass after 8 s");
    static rec_t r;

    /* cat to "yes or no": one change; the arcs grow; a tap answers only once they are on the glass */
    static uint8_t cat_px[PX * PX * 4];
    memcpy(cat_px, wm_pixels(), sizeof(cat_px));
    record(&r, 3.f, ask_yesno);
    common("cat to yes/no", &r, 1);
    arcs_of(cat_px, r.img[r.n - 1]);
    {
        int prev = 0, growing = 0, first = -1, full = -1, drops = 0;
        for (int k = 0; k < r.n; k++) {                /* (from its first growth on: a grain flying by lights a bin a frame) */
            const int s = span(r.img[k]);
            if (s > 5 && first < 0) first = k;
            if (first >= 0 && s < prev && prev < 95) drops++;
            if (s > 5 && s < 90) growing++;
            if (s >= 90 && full < 0) full = k;
            prev = s;
        }
        printf("  arcs: %d%% of their angle lit at rest, growing over %d frames (from frame %d to %d)\n", span(r.img[r.n - 1]), growing, first, full);
        CHECK(s_bins_all >= SPAN_BINS / 3, "the arcs were not found (%d bins)", s_bins_all);
        CHECK(drops == 0, "the arcs' lit length went back %d times while they grew", drops);
        CHECK(growing >= 6, "the arcs lit over %d frames (they grow over 6 or more, not whole at once)", growing);
        CHECK(full > 0 && r.t[full] < REST_S * 1000.f, "the arcs were whole at frame %d", full);
        int early = 0, late = -1;
        for (int k = 0; k < r.n; k++) {
            if (r.shown[k] && span(r.img[k]) < 90) early++;
            if (!r.morph[k] && k > 0 && r.morph[k - 1]) late = k;
        }
        CHECK(early == 0, "a tap could answer on %d frames before the answers were on the glass", early);
        CHECK(late >= 0 && r.shown[r.n - 1] && (late + 2 >= r.n || r.shown[late + 2]), "a tap cannot answer once the change has brought the question");
    }

    /* "yes or no" to "pairing code": one change, never through the bare cat */
    record(&r, 3.f, ask_pair);
    common("yes/no to pair", &r, 1);
    {
        int bare = 0;
        for (int k = 0; k < r.n; k++) bare += r.look[k] == creature;
        CHECK(bare == 0, "between two questions the drawer was given the bare cat's look on %d frames", bare);
    }

    /* "pairing code" to the cat, cancelled: one change, the cat not dissolved twice */
    record(&r, 3.f, cancel);
    common("pair to cancel", &r, 1);
    CHECK(cat_once(&r), "pair to cancel: the cat came back and was dissolved again");
    CHECK(r.look[r.n - 1] == creature, "the cat is not back after a cancel");
    run(2.f);

    /* "how was today?" (rows) to the cat, answered with a tap: one change, the press taken as the answer (no ripple) */
    record(&r, 3.f, ask_five);
    common("cat to five", &r, 1);
    ask_pair(); run(3.f);
    record(&r, 3.f, tap_accept);
    wm_touch(0, 0, 0);
    common("pair to accept", &r, 1);
    CHECK(cat_once(&r), "pair to accept: the cat came back and was dissolved again");
    printf("  water after the answer: %d (of %d)\n", r.hmax, (int)FQ_H);
    CHECK(r.hmax < 0.03f * FQ_H, "the water ran high after a tap answered (%d): a ripple or a wave of the answer under the change", r.hmax);

    /* off device speed: the whole panel each frame, no change of picture runs; the question forms by the ring of water */
    wm_device(0);
    run(2.f);
    ask_yesno();
    for (int i = 0; i < 12; i++) frame();
    CHECK(!s_ask.scr.whole && s_ask.v, "off device speed the question came whole (no change of picture carries it there)");

    printf("ask_flow_test: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
