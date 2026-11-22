/*
 * SPDX-License-Identifier: MIT
 *
 * weather_flow_test.c: the weather on the glass through mole_fluid.c itself (the web module's native build, web_molty.c,
 * included whole), with the cat. Told through POST /api/creature/state's body as an agent's helper would. Each kind: its
 * streaks, rings, drops on the glass, the wet floor's mirror and its bumps in the water show, bounded a frame; never a
 * rain dot near the cat's eyes and its eyes the brightest thing on the glass. The lifetime runs out: back to clear and,
 * dried, nothing drawn. The moonlit rule against the sky's moon. Nothing made nor bumped while a scene, a question,
 * words or the screen off own the glass. The setting off: nothing. A downpour: the cat shakes itself off, never over a
 * gesture or a mood.
 *
 *   weather_flow_test <repo root>
 */
#include "../../web/web_molty.c"

#include <sys/wait.h>
#include <unistd.h>

static int s_pass, s_fail;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define PX 466
#define DT 33333u
#define SUPERMOON 1479131520.0                     /* 2016-11-14 13:52 UTC: full */
#define THIN      1713208380.0 - 6.0 * 86400.0     /* 2024-04-09: a day after the new moon, lit ~0.02 */

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "weather_flow_test: cannot read %s\n", path); exit(2); }
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
static void frames(int n) { for (int i = 0; i < n; i++) wm_frame(DT); }

/* one frame's look: the eyes' box (the cat's iris and pupil pixels), any pixel of the world (not the cat's) lit inside
 * it grown by 6 px, and the brightest pixel of the world against the eyes' brightest */
typedef struct { int near_eyes, world_max, eye_max; } look_t;
static look_t look(void) {
    const uint8_t *p = wm_pixels(), *c = wm_cat_pixels();
    look_t L = { 0 };
    int x0 = PX, x1 = -1, y0 = PX, y1 = -1;
    for (int q = 0; c && q < PX * PX; q++) {
        const uint8_t *e = c + (size_t)q * 4;
        if (!(e[1] > e[0] + 40 && e[1] > 120)) continue;
        const int x = q % PX, y = q / PX;
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
        if (luma(e) > L.eye_max) L.eye_max = luma(e);
    }
    for (int q = 0; c && q < PX * PX; q++) {
        const uint8_t *a = p + (size_t)q * 4, *e = c + (size_t)q * 4;
        if (e[0] | e[1] | e[2]) continue;                    /* the cat's own */
        const int l = luma(a), x = q % PX, y = q / PX;
        if (l > L.world_max) L.world_max = l;
        if (l > 8 && x >= x0 - 6 && x <= x1 + 6 && y >= y0 - 6 && y <= y1 + 6) L.near_eyes++;
    }
    return L;
}

static void kinds(void) {
    static const char *const TELL[] = { NULL, "{\"weather\":{\"kind\":\"drizzle\",\"s\":600}}", "{\"weather\":\"rain\"}", "{\"weather\":{\"kind\":\"downpour\",\"s\":600}}" };
    static const int CAP[] = { 0, 650, 800, 900 };       /* the weather's dots a frame (streaks, rings, front, glass, mirror) */
    for (int kind = WX_DRIZZLE; kind <= WX_DOWNPOUR; kind++) {
        CHECK(wm_state(TELL[kind]) == 0, "%s told", WX_NAMES[kind]);
        int shaken = 0;                                    /* (eased in, the floor wet through; a downpour: the cat shakes) */
        for (int i = 0; i < 30 * 100; i++) {
            if (i % 600 == 0) wm_state("{\"mood\":\"idle\"}");   /* (someone about: it does not doze off) */
            wm_frame(DT);
            float t;
            const int c = cc_clip(cpk_clip(s_cpk), &t);
            shaken += c >= 0 && !strcmp(cc_clip_name(cpk_clip(s_cpk), c), "shaken");
        }
        CHECK(kind == WX_DOWNPOUR ? shaken > 0 : shaken == 0, "%s: the cat shook itself %d frames", WX_NAMES[kind], shaken);
        int most = 0, refl = 0, back = 0, front = 0, glass = 0, bumps = 0, mb = 0, near = 0, rings = 0;
        look_t worst = { 0, 0, 255 };
        for (int i = 0; i < 300; i++) {
            wm_frame(DT);
            int d[4]; cpk_weather_dots(s_cpk, d);
            const int all = d[0] + d[1] + d[2] + d[3];
            most = all > most ? all : most;
            refl += d[0]; back += d[1]; front += d[2]; glass += d[3];
            bumps += s_wx->nbump; mb = s_wx->nbump > mb ? s_wx->nbump : mb; rings += s_wx->nring;
            if (i % 10 == 0) {
                const look_t L = look();
                near += L.near_eyes;
                if (L.world_max > worst.world_max) worst.world_max = L.world_max;
                if (L.eye_max < worst.eye_max) worst.eye_max = L.eye_max;
            }
        }
        printf("  %-9s a frame: %d mirrored, %d behind, %d in front, %d on the glass (most %d); %.1f rings, %.2f bumps (most %d); world's brightest %d, eyes %d\n",
               WX_NAMES[kind], refl / 300, back / 300, front / 300, glass / 300, most, rings / 300.f, bumps / 300.f, mb, worst.world_max, worst.eye_max);
        CHECK(s_wx->level == (float)kind && s_wx->wet == 1.f, "%s: level %.2f, wet %.2f", WX_NAMES[kind], s_wx->level, s_wx->wet);
        CHECK(back / 300 > 10 && rings > 0 && glass > 0 && bumps > 0, "%s: rain missing (%d behind, %d rings, %d glass, %d bumps)", WX_NAMES[kind], back, rings, glass, bumps);
        CHECK(refl / 300 > 40, "%s: the wet floor mirrors %d dots", WX_NAMES[kind], refl / 300);
        CHECK(kind == WX_DOWNPOUR ? front > 0 : front == 0, "%s: %d in front", WX_NAMES[kind], front);
        CHECK(most <= CAP[kind] && mb <= WX_BUMPS, "%s: %d dots, %d bumps a frame", WX_NAMES[kind], most, mb);
        CHECK(near == 0, "%s: %d world pixels by the eyes", WX_NAMES[kind], near);
        CHECK(worst.world_max < worst.eye_max, "%s: the world's brightest (%d) over the eyes' (%d)", WX_NAMES[kind], worst.world_max, worst.eye_max);
        char *js = wm_state_json();
        char want[64];
        snprintf(want, sizeof(want), "\"kind\":\"%s\"", WX_NAMES[kind]);
        CHECK(js && strstr(js, want) && strstr(js, "\"s_left\":") && strstr(js, "\"weather_on\":true"), "GET reports %s", WX_NAMES[kind]);
        free(js);
    }
}

/* the drizzle's end (weather-drizzle.script): told for 40 s, it runs out, a quarter of an hour on it is dry; the panel's
 * hash of each of the last frames */
#define END_N 60
static void drizzle_end(bool told, uint64_t *out) {
    frames(60);
    if (told) wm_state("{\"weather\":{\"kind\":\"drizzle\",\"s\":40}}");
    frames(900);
    wm_touch(1, 233, 415); frames(2); wm_touch(0, 0, 0);
    frames(360);
    wm_frame(900000000u);
    frames(120 - END_N);
    for (int i = 0; i < END_N; i++) {
        wm_frame(DT);
        const uint8_t *p = wm_pixels();
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t q = 0; q < (size_t)PX * PX * 4; q++) { h ^= p[q]; h *= 0x100000001b3ull; }
        out[i] = h;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: weather_flow_test <repo root>\n"); return 2; }
    char path[1024];
    size_t len;
    snprintf(path, sizeof(path), "%s/packages/cat/cat.json", argv[1]);
    char *cat = slurp(path, &len);
    {   /* told, run out and dried: then exactly the frames of a Molty never told (a child: the same seed, no weather) */
        int pipefd[2];
        if (pipe(pipefd)) return 2;
        fflush(stdout);
        const pid_t pid = fork();
        if (pid == 0) {
            uint64_t h[END_N];
            if (wm_creature("cat", cat, len) || wm_start(PX, 20261122, 0)) _exit(3);
            drizzle_end(false, h);
            _exit(write(pipefd[1], h, sizeof(h)) == (ssize_t)sizeof(h) ? 0 : 4);
        }
        uint64_t a[END_N], b[END_N];
        CHECK(wm_creature("cat", cat, len) == 0 && wm_start(PX, 20261122, 0) == 0, "the module starts");
        drizzle_end(true, a);
        int st = 0;
        const bool got = read(pipefd[0], b, sizeof(b)) == (ssize_t)sizeof(b);
        waitpid(pid, &st, 0);
        int same = 0;
        for (int i = 0; got && i < END_N; i++) same += a[i] == b[i];
        CHECK(got && same == END_N, "dried: %d of the last %d frames as with no weather told", same, END_N);
        CHECK(!weather_drawn(s_wx) && s_wx->kind == WX_CLEAR, "dried: still drawn");
    }
    frames(90);
    CHECK(!s_wx->show && s_wx->level == 0.f && !weather_drawn(s_wx), "clear and dry: nothing");
    CHECK(wm_state("{\"weather\":\"snow\"}") != 0 && wm_state("{\"weather\":{\"kind\":\"rain\",\"s\":5}}") != 0 && wm_state("{\"weather_on\":1}") != 0,
          "a wrong kind, lifetime or setting refused");
    frames(5);
    CHECK(s_wx->kind == WX_CLEAR, "a refused telling taken");

    kinds();

    /* the downpour's shake (it shook above): never while a mood holds */
    {
        s_wx->shake = true;                                /* (due again at once) */
        wm_state("{\"mood\":\"happy\"}");
        int during = 0;
        for (int i = 0; i < 60; i++) {
            wm_frame(DT);
            float t;
            const int c = cc_clip(cpk_clip(s_cpk), &t);
            during += c >= 0 && !strcmp(cc_clip_name(cpk_clip(s_cpk), c), "shaken");
        }
        CHECK(during == 0 && s_wx->shake, "a mood held: the shake waits (%d frames of it)", during);
        wm_state("{\"mood\":\"idle\"}");
        s_wx->shake = false;
    }

    /* nothing made nor bumped while something else owns the glass; the clock runs */
    {
        snprintf(path, sizeof(path), "%s/packages/astrology/astrology.json", argv[1]);
        char *astro = slurp(path, &len);
        CHECK(wm_package(astro, len) == 0 && wm_scene("globe", "{\"img\":\"moon\",\"lit\":0.5,\"side\":1,\"r\":0.3,\"spin\":0}", 20.f) == 0, "a scene");
        int made = 0, up = 0;
        for (int i = 0; i < 60; i++) { wm_frame(DT); up += s_cur == ST_SCENE; made += s_cur == ST_SCENE && (s_wx->show || s_wx->nbump); }   /* (it leaves with the cat's picture) */
        CHECK(made == 0 && up > 30, "a scene (%d frames up): the weather made %d frames", up, made);
        wm_scene_clear(); frames(90);
        CHECK(s_wx->show, "the cat back: the weather back");
        CHECK(wm_ask("{\"title\":\"umbrella?\",\"options\":[\"yes\",\"no\"]}") == 0, "a question");
        made = 0;
        for (int i = 0; i < 90; i++) { wm_frame(DT); made += s_wx->show || s_wx->nbump; }
        CHECK(made == 0, "a question: the weather made %d frames", made);
        wm_ask_end(); frames(60);
        wm_state("{\"state\":\"text\",\"text\":\"it is pouring outside\"}");
        made = 0;
        for (int i = 0; i < 30; i++) { wm_frame(DT); made += s_cur == ST_TEXT && (s_wx->show || s_wx->nbump); }
        CHECK(made == 0, "words on the glass: the weather made %d frames", made);
        frames(240);
        mole_fluid_screen(false); frames(5);
        const int64_t t = s_wx_t;
        frames(60);
        CHECK(!mole_fluid_screen_on() && s_wx_t == t, "the screen off: the weather ran");
        mole_fluid_screen(true); frames(5);
    }

    /* moonlit: rain under the supermoon; plain at a thin moon and with no date */
    {
        wm_state("{\"weather\":\"rain\"}");
        frames(30 * 15);
        CHECK(s_wx->moonlit == 0.f, "no date: moonlit %.2f", s_wx->moonlit);
        wm_time(THIN); frames(30);
        CHECK(s_sky->moon.lit < 0.1f && s_wx->moonlit == 0.f, "a thin moon (lit %.2f): moonlit %.2f", s_sky->moon.lit, s_wx->moonlit);
        wm_time(SUPERMOON); frames(30);
        CHECK(s_sky->moon.lit > 0.99f && s_wx->moonlit > 0.99f, "the full moon (lit %.2f): moonlit %.2f", s_sky->moon.lit, s_wx->moonlit);
        wm_state("{\"sky\":false}"); frames(5);
        CHECK(s_wx->moonlit == 0.f, "the sky off: moonlit %.2f", s_wx->moonlit);
        wm_state("{\"sky\":true}");
    }

    /* the lifetime: told for 20 s, it eases back to clear; dried, nothing of it is drawn */
    {
        wm_state("{\"weather\":{\"kind\":\"rain\",\"s\":20}}");
        frames(30 * 21);
        CHECK(s_wx->kind == WX_CLEAR, "20 s on: still %s", WX_NAMES[s_wx->kind]);
        frames(30 * 15);
        CHECK(s_wx->level == 0.f && s_wx->wet > 0.f && weather_drawn(s_wx), "the rain over: level %.2f, wet %.2f", s_wx->level, s_wx->wet);
        int d[4]; cpk_weather_dots(s_cpk, d);
        CHECK(d[0] > 0, "after the rain: the floor still mirrors");
        s_wx_t -= (int64_t)(WX_DRY_S * 1e6f);              /* (a quarter of an hour on, the screen off) */
        frames(30 * 75);                                   /* (the drops on the glass run their time out) */
        cpk_weather_dots(s_cpk, d);
        CHECK(s_wx->wet == 0.f && !weather_drawn(s_wx) && !d[0] && !s_wx->nbead, "dried: wet %.2f, drawn %d, %d beads", s_wx->wet, weather_drawn(s_wx), s_wx->nbead);
        char *js = wm_state_json();
        CHECK(js && strstr(js, "\"kind\":\"clear\"") && strstr(js, "\"s_left\":0"), "GET reports clear");
        free(js);
    }

    /* the setting off: nothing, a telling ignored; GET reports it */
    {
        wm_state("{\"weather\":\"downpour\"}");
        frames(30 * 10);
        CHECK(s_wx->show, "a downpour");
        CHECK(wm_state("{\"weather_on\":false}") == 0, "the setting taken");
        int made = 0;
        for (int i = 0; i < 60; i++) { wm_frame(DT); made += s_wx->show || s_wx->nbump; }
        int d[4]; cpk_weather_dots(s_cpk, d);
        CHECK(made == 0 && !d[0] && !d[1] && !d[2] && !d[3], "the setting off: %d frames made", made);
        wm_state("{\"weather\":\"rain\"}"); frames(60);
        CHECK(!s_wx->show && s_wx->kind == WX_CLEAR, "the setting off: rain told and shown");
        char *js = wm_state_json();
        CHECK(js && strstr(js, "\"weather_on\":false"), "GET reports the setting off");
        free(js);
        wm_state("{\"weather_on\":true}");
    }

    printf("weather_flow_test: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
