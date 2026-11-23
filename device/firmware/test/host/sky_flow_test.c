/*
 * SPDX-License-Identifier: MIT
 *
 * sky_flow_test.c: the sky behind the cat through mole_fluid.c itself (the web module's native build, web_molty.c,
 * included whole). The same cat, the same seed, the same gestures twice: once with no time (a child process: the frames
 * as they always were) and once on the night of the 2016 supermoon (the largest moon). Every frame: outside the moon's
 * corner of the glass the two are the same picture; inside it every pixel of the cat is the same and every pixel the sky
 * adds is in the moon's disc, off the cat, and under the cat's eyes. Then the falling star: it comes when its wait is
 * over, runs its time with a bounded number of dots, and never while a question, a listen, a say, a speaking mood, a
 * scene, the screen off, or sleep with do not disturb owns the glass; the setting off draws nothing of the sky.
 *
 *   sky_flow_test <repo root>
 */
#include "../../web/web_molty.c"

#include <sys/wait.h>
#include <unistd.h>

static int s_pass, s_fail;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define PX 466
#define DT 33333u
#define SUPERMOON 1479131520.0                     /* 2016-11-14 13:52 UTC: full, 356,5xx km (sky_test.c) */
/* the moon's corner: its disc (option A's place, the largest radius) with room */
#define BX0 300
#define BX1 412
#define BY0 40
#define BY1 156
#define BW (BX1 - BX0)
#define BN (BW * (BY1 - BY0))
typedef struct { uint64_t outside; uint8_t luma[BN], cat[BN]; } rec_t;

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "sky_flow_test: cannot read %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    b[n] = 0; *len = (size_t)n;
    fclose(f);
    return b;
}
static int luma(const uint8_t *p) { return (int)(0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2] + 0.5f); }

/* one frame's record: the whole glass outside the corner as a hash, the corner as luma and the cat's own pixels */
static void record(rec_t *r) {
    const uint8_t *p = wm_pixels(), *c = wm_cat_pixels();
    uint64_t h = 0xcbf29ce484222325ull;
    for (int y = 0; y < PX; y++) for (int x = 0; x < PX; x++) {
        const uint8_t *q = p + ((size_t)y * PX + x) * 4;
        if (x >= BX0 && x < BX1 && y >= BY0 && y < BY1) {
            const int k = (y - BY0) * BW + x - BX0;
            r->luma[k] = (uint8_t)luma(q);
            r->cat[k] = c && (c[((size_t)y * PX + x) * 4] | c[((size_t)y * PX + x) * 4 + 1] | c[((size_t)y * PX + x) * 4 + 2]) ? 1 : 0;
            continue;
        }
        for (int b = 0; b < 3; b++) { h ^= q[b]; h *= 0x100000001b3ull; }
    }
    r->outside = h;
}

/* the cat goes through all it does: every gesture of its package, the moods, a walk */
static const char *const STEPS[] = {
    "{\"act\":\"look_up\"}", "{\"act\":\"stretch\"}", "{\"act\":\"walk\"}", "{\"act\":\"yarn\"}", "{\"act\":\"eat\"}",
    "{\"act\":\"peek\"}", "{\"act\":\"inspect\"}", "{\"act\":\"glance_right\"}", "{\"act\":\"bounce\"}", "{\"act\":\"wiggle\"}",
    "{\"act\":\"paddle\"}", "{\"act\":\"yawn\"}", "{\"act\":\"shake_head\"}", "{\"act\":\"wobble\"}", "{\"mood\":\"surprised\"}",
    "{\"mood\":\"happy\"}", "{\"mood\":\"sleepy\"}", "{\"mood\":\"idle\"}",
};
#define STEP_FRAMES 150
#define WARM 90
static int frames_total(void) { return WARM + (int)(sizeof(STEPS) / sizeof(STEPS[0])) * STEP_FRAMES; }

static void scenario(void (*each)(int f, void *), void *ctx) {
    int f = 0;
    for (int i = 0; i < WARM; i++, f++) { wm_frame(DT); each(f, ctx); }
    for (size_t s = 0; s < sizeof(STEPS) / sizeof(STEPS[0]); s++) {
        if (wm_state(STEPS[s]) != 0) { printf("FAIL: step %s refused\n", STEPS[s]); s_fail++; }
        for (int i = 0; i < STEP_FRAMES; i++, f++) { wm_frame(DT); each(f, ctx); }
    }
}

static void child_each(int f, void *ctx) {
    (void)f;
    static rec_t r;
    record(&r);
    fwrite(&r, sizeof(r), 1, (FILE *)ctx);
}

typedef struct { FILE *in; int frames, through, added_max, sky_px, eye_max, outside_bad, cat_bad, place_bad; float r; } cmp_t;
static void parent_each(int f, void *ctx) {
    cmp_t *c = ctx;
    static rec_t a, b;
    if (fread(&a, sizeof(a), 1, c->in) != 1) { c->outside_bad++; return; }
    record(&b);
    c->frames++;
    if (a.outside != b.outside) { c->outside_bad++; if (c->outside_bad < 4) printf("frame %d: outside the moon's corner the glass differs\n", f); }
    const float cx = SKY_MOON_X * PX, cy = SKY_MOON_Y * PX;
    bool through = false;
    for (int k = 0; k < BN; k++) {
        const float x = BX0 + k % BW + 0.5f, y = BY0 + k / BW + 0.5f, d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
        if (a.cat[k] && d < c->r) through = true;
        if (a.cat[k] && (!b.cat[k] || a.luma[k] != b.luma[k])) c->cat_bad++;
        if (a.luma[k] == b.luma[k]) continue;
        c->sky_px++;
        if (b.luma[k] > c->added_max) c->added_max = b.luma[k];
        if (d > c->r + 6.f || b.cat[k]) c->place_bad++;
    }
    c->through += through;
    /* the eyes: the cat's brightest green (its iris), wherever they are */
    const uint8_t *cat = wm_cat_pixels();
    for (int q = 0; cat && q < PX * PX; q++) {
        const uint8_t *p = cat + (size_t)q * 4;
        if (p[1] > p[0] + 40 && p[1] > 120 && luma(p) > c->eye_max) c->eye_max = luma(p);
    }
}

static int star_dots(void) { int n = 0; for (int q = 0; q < s_sky->nstar; q++) n += __builtin_popcount(s_sky->star_c[q].bits); return n; }
/* frames with a falling star on the glass while something else owns it (none allowed) */
static int stars_during(int n) { int bad = 0; for (int i = 0; i < n; i++) { wm_frame(DT); bad += s_sky->nstar > 0; } return bad; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: sky_flow_test <repo root>\n"); return 2; }
    char path[1024], tmp[] = "/tmp/molty-sky-flow.XXXXXX";
    size_t len;
    snprintf(path, sizeof(path), "%s/packages/cat/cat.json", argv[1]);
    char *cat = slurp(path, &len);
    const int fd = mkstemp(tmp);
    if (fd < 0) return 2;
    close(fd);
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {                                  /* no time: the frames as they always were */
        FILE *out = fopen(tmp, "wb");
        if (!out || wm_creature("cat", cat, len) || wm_start(PX, 20261122, 0)) _exit(3);
        scenario(child_each, out);
        fclose(out);
        _exit(s_sky->show || s_sky->nmoon || s_sky->day != -1 ? 4 : 0);   /* (no time: nothing of the sky made) */
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "the run without a time made no sky (%d)", WEXITSTATUS(st));

    CHECK(wm_creature("cat", cat, len) == 0 && wm_start(PX, 20261122, 0) == 0, "the module starts");
    wm_time(SUPERMOON);
    s_sky->star.wait = 1e9f;                         /* (the moon alone first) */
    cmp_t c = { .in = fopen(tmp, "rb") };
    if (!c.in) return 2;
    c.r = 10.f / (2 * 59) * PX;                      /* the largest moon's radius, px */
    scenario(parent_each, &c);
    fclose(c.in);
    remove(tmp);
    CHECK(s_sky->rdots == 10 && s_sky->moon.lit > 0.99f, "the supermoon: %d dots across, lit %.3f", s_sky->rdots, s_sky->moon.lit);
    CHECK(c.frames == frames_total(), "%d frames compared", c.frames);
    CHECK(c.outside_bad == 0, "outside the moon's corner the glass is the same as with no time (%d frames differ)", c.outside_bad);
    CHECK(c.cat_bad == 0, "every pixel of the cat the same with the moon (%d differ)", c.cat_bad);
    CHECK(c.place_bad == 0, "every pixel the sky adds inside the moon's disc and off the cat (%d not)", c.place_bad);
    CHECK(c.sky_px > 300 * c.frames / 2, "the moon shows (%d px over %d frames)", c.sky_px, c.frames);
    CHECK(c.through > 0, "the cat passed through the moon's disc in %d frames (and stayed in front)", c.through);
    CHECK(c.eye_max > 0 && c.added_max < c.eye_max, "the moon's brightest (luma %d) under the cat's eyes (%d)", c.added_max, c.eye_max);

    /* the falling star: its wait over, it comes, runs its time, bounded */
    s_sky->star.wait = 0.2f;
    int on = 0, most = 0, first = -1, brightest = 0;
    for (int i = 0; i < 90; i++) {
        wm_frame(DT);
        if (s_sky->nstar > 0) { on++; if (first < 0) first = i; }
        most = star_dots() > most ? star_dots() : most;
        for (int q = 0; q < s_sky->nstar; q++) { uint8_t p[3] = { s_sky->star_c[q].rgb >> 16, s_sky->star_c[q].rgb >> 8, s_sky->star_c[q].rgb }; brightest = luma(p) > brightest ? luma(p) : brightest; }
    }
    const float life = (float)on * DT / 1e6f;
    CHECK(first >= 0 && first <= 12, "it came %d frames after its wait", first);
    CHECK(life > 0.4f && life <= SKY_STAR_DUR_MAX_S + SKY_STAR_FADE_S + 0.1f, "it showed %.2f s", life);
    CHECK(most > 0 && most <= SKY_STAR_DOTS, "%d dots at most", most);
    CHECK(brightest < c.eye_max, "its head (luma %d) under the cat's eyes (%d)", brightest, c.eye_max);

    /* never while something else owns the glass: each time its wait is over first */
    s_sky->star.wait = 0.f;
    CHECK(wm_ask("{\"title\":\"feed the cat?\",\"options\":[\"yes\",\"no\"]}") == 0, "a question");
    CHECK(stars_during(120) == 0 && s_sky->nmoon_shown == 0, "a question: no star, no moon");
    wm_ask_end(); stars_during(60);
    s_sky->star.wait = 0.f; s_sky->star.t = -1.f;
    wm_listen(1);
    CHECK(stars_during(90) == 0 && s_sky->nmoon_shown > 0, "a listen: no star, the moon stays");
    wm_listen(0); stars_during(60);
    s_sky->star.wait = 0.f; s_sky->star.t = -1.f;
    wm_state("{\"mood\":\"speaking\"}");
    CHECK(stars_during(90) == 0, "speaking: no star");
    wm_state("{\"mood\":\"idle\"}"); stars_during(30);
    s_sky->star.wait = 0.f; s_sky->star.t = -1.f;
    wm_state("{\"state\":\"text\",\"text\":\"the build passed\"}");
    CHECK(stars_during(60) == 0, "a say: no star");
    stars_during(150);
    snprintf(path, sizeof(path), "%s/packages/astrology/astrology.json", argv[1]);
    char *astro = slurp(path, &len);
    CHECK(wm_package(astro, len) == 0 && wm_scene("globe", "{\"img\":\"moon\",\"lit\":0.5,\"side\":1,\"r\":0.3,\"spin\":0}", 20.f) == 0, "a scene");
    s_sky->star.wait = 0.f; s_sky->star.t = -1.f;
    CHECK(stars_during(120) == 0, "a scene: no star (state %d)", s_cur);
    wm_scene_clear(); stars_during(90);
    /* the screen off: no frame runs, nothing on the glass, the wait does not count */
    s_sky->star.wait = 0.5f; s_sky->star.t = -1.f;
    mole_fluid_screen(false); stars_during(5);
    const float wait = s_sky->star.wait;
    CHECK(!mole_fluid_screen_on() && stars_during(90) == 0 && s_sky->star.wait == wait, "the screen off: no star, the wait stands");
    mole_fluid_screen(true); stars_during(5);
    /* asleep with do not disturb on: none; do not disturb off: it comes */
    mole_fluid_dnd(true);
    s_dir.asleep = true;
    s_sky->star.wait = 0.f; s_sky->star.t = -1.f;
    int bad = 0;
    for (int i = 0; i < 60; i++) { s_dir.asleep = true; wm_frame(DT); bad += s_sky->nstar > 0; }
    CHECK(bad == 0, "asleep with do not disturb: no star");
    mole_fluid_dnd(false);
    int seen = 0;
    for (int i = 0; i < 30; i++) { s_dir.asleep = true; wm_frame(DT); seen += s_sky->nstar > 0; }
    CHECK(seen > 0, "asleep without do not disturb: it comes");
    /* the setting off: nothing of the sky; GET reports it */
    CHECK(wm_state("{\"sky\":false}") == 0 && wm_state("{\"sky\":1}") != 0, "the setting taken, a wrong one refused");
    s_sky->star.wait = 0.f;
    CHECK(stars_during(60) == 0 && !s_sky->show, "the setting off: no star, no moon");
    char *js = wm_state_json();
    CHECK(js && strstr(js, "\"sky\":false"), "GET reports the setting off");
    free(js);
    int grey = 0;
    const uint8_t *p = wm_pixels();
    for (int y = BY0; y < BY1; y++) for (int x = BX0; x < BX1; x++) {
        const uint8_t *q = p + ((size_t)y * PX + x) * 4;
        grey += q[0] > 20 && q[0] == q[1] && q[2] < q[0];   /* (the moon's own grey) */
    }
    CHECK(grey == 0, "no moon on the glass (%d of its pixels)", grey);
    s_sky->star.wait = 1e9f;
    CHECK(wm_state("{\"sky\":true}") == 0 && stars_during(5) == 0 && s_sky->nmoon_shown > 0, "on again: the moon");

    printf("sky_flow_test: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
