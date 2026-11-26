/*
 * SPDX-License-Identifier: MIT
 *
 * picture_flow_test.c: changes of picture through mole_fluid.c itself (the web module's native build, web_molty.c,
 * included whole), at device speed (wm_device). The creature is made on a task of its own on the device (cr_load); here
 * that task is run a set number of frames after it is asked for, to stand for a creature made early, late, never, or
 * one that fails to build.
 *   start-up: only the boot word, dust and the creature are ever shown (never the blob); the word stays whole until its
 *     one change into the creature starts, as soon as the creature is made (at 2.8 s if it was made before); with no
 *     creature the word stays for good, lit, and a say over it comes back to it;
 *   the relay's first welcome (the grid rebuilt in internal RAM): never while the word is up or a change runs, so the
 *     word never melts and the change is never cut short; the rebuild still happens once the picture is still;
 *   a say's words going back to the cat: the last word never comes back over the cat as its dust gathers;
 *   a question over a scene: the scene stays whole until the question's one change takes it (no coarse dots between);
 *   screen off (by hand, the nap), power off, a restart: the picture dissolves to an empty glass first, then the panel
 *     sleeps (and nothing is drawn) or the power goes; on again, it gathers from the empty glass;
 *   creatures: a creature change and the way home with one drawn in the fluid are each one change of picture.
 *
 *   picture_flow_test <repo root> <case>   (picture_flow_build.sh runs every case)
 */
#include "web_sdk.h"
static int bg_create(void (*fn)(void *), void *arg, void **out);
#define xTaskCreatePinnedToCoreWithCaps(fn, name, stack, arg, prio, out, core, caps) bg_create(fn, arg, (void **)(out))
#include "../../web/web_molty.c"

static int s_pass, s_fail;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define PX     466
#define DT     33333u
#define BRIGHT 120     /* a pixel of the word's dots (or the creature's lines), not their glow */
#define LIT    24

/* the creature's task: run `late` frames after it was made (0: in place, as the module), never (< 0), or on a file that
 * does not build (s_bad) */
static int s_late;
static bool s_bad;
static struct { void (*fn)(void *); void *arg; int left; bool on; } s_bg;
static int bg_create(void (*fn)(void *), void *arg, void **out) {
    if (!s_late && !s_bad) return pdFALSE;          /* made where it is asked for */
    s_bg.fn = fn; s_bg.arg = arg; s_bg.left = s_late; s_bg.on = true;
    *out = (void *)&s_bg;
    return pdPASS;
}
static void bg_tick(void) {
    if (!s_bg.on || s_bg.left < 0 || s_bg.left-- > 0) return;
    s_bg.on = false;
    if (s_bad) {                                    /* what a broken package does: cpk_create refuses it */
        cr_job_t *j = s_bg.arg;
        static const char broken[] = "{\"not\":\"a creature\"}";
        j->blob = realloc(j->blob, sizeof(broken)); memcpy(j->blob, broken, sizeof(broken)); j->len = sizeof(broken) - 1;
    }
    s_bg.fn(s_bg.arg);
}

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "picture_flow_test: cannot read %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    b[n] = 0; *len = (size_t)n;
    fclose(f);
    return b;
}

static int s_fno;
/* every frame: a new look with no change of picture running (it came at once: a cut), changes started */
static uint32_t s_look_was;
static bool s_on_was;
static int s_cuts, s_starts, s_slow, s_slow_n;   /* (s_slow: frames of a change over 33.4 ms in the panel model) */
static void frame(void) {
    bg_tick(); wm_frame(DT); s_fno++; char *e = wm_events(); free(e);
    const uint32_t look = s_draw ? s_draw->m.look & ~FLUID_LOOK_DUST : 0;
    const bool on = s_draw && fluid_draw_morphing(s_draw);
    s_cuts += look != s_look_was && !on && !s_on_was && s_look_was;
    s_starts += on && !s_on_was;
    if (on) { s_slow += wm_frame_info()[FI_FRAME_MS] > 33.4f; s_slow_n++; }
    s_look_was = look; s_on_was = on;
}
static void frames(int n) { while (n-- > 0) frame(); }

static uint8_t s_ref[PX * PX];
static int bright_of(uint8_t *out) {                /* the bright pixels now; with out: kept there */
    const uint8_t *p = wm_pixels();
    int n = 0;
    for (int i = 0; i < PX * PX; i++) {
        const uint8_t *q = p + (size_t)i * 4, b = q[0] > BRIGHT || q[1] > BRIGHT || q[2] > BRIGHT;
        if (out) out[i] = b;
        n += b;
    }
    return n;
}
static float kept(void) {                           /* the share of s_ref's bright pixels still bright */
    const uint8_t *p = wm_pixels();
    int all = 0, on = 0;
    for (int i = 0; i < PX * PX; i++) {
        if (!s_ref[i]) continue;
        const uint8_t *q = p + (size_t)i * 4;
        all++; on += q[0] > BRIGHT || q[1] > BRIGHT || q[2] > BRIGHT;
    }
    return all ? (float)on / (float)all : 0.f;
}
static float lit_share(void) {
    const uint8_t *p = wm_pixels();
    int disc = 0, on = 0;
    const float r = PX * 0.5f;
    for (int y = 0; y < PX; y++) for (int x = 0; x < PX; x++) {
        const float dx = x + 0.5f - r, dy = y + 0.5f - r;
        if (dx * dx + dy * dy > r * r) continue;
        const uint8_t *q = p + ((size_t)y * PX + x) * 4;
        disc++; on += q[0] > LIT || q[1] > LIT || q[2] > LIT;
    }
    return (float)on / (float)disc;
}

static char *s_cat; static size_t s_cat_len;
static void power_on(int late, bool bad) {
    memset(&s_bg, 0, sizeof(s_bg));
    s_late = late; s_bad = bad; s_fno = 0;
    CHECK(wm_creature("cat", s_cat, s_cat_len) == 0 && wm_start(PX, 20260516, 1) == 0, "power on");
    wm_device(1);
}
static bool word(void) { return s_cur == ST_TEXT && fd_boot_word(&s_dir); }
static const uint32_t CAT = 0x94000001u;            /* fluid_look_of(FLUID_LOOK_CREATURE, 1, dust) */

/* a start-up: until the cat stands (or `until` frames). Only the word, its change and the cat; the word whole from
 * frame 70 (formed) until its change starts. The frame the change started and the one the cat stood, or -1. */
typedef struct { int start, stood, changes, odd, gather, gather_end; float least_kept; } boot_t;
static boot_t boot_run(int until, int welcome_at) {
    boot_t b = { -1, -1, 0, 0, 0, -1, 1.f };
    bool was = s_draw && fluid_draw_morphing(s_draw);
    while (s_fno < until && b.stood < 0) {
        if (s_fno == welcome_at) mole_fluid_allow_internal();
        frame();
        const bool on = fluid_draw_morphing(s_draw);
        if (on && !was) { if (s_fno <= 3) b.gather++; else { b.changes++; if (b.start < 0) b.start = s_fno; } }
        if (!on && was && b.gather_end < 0) b.gather_end = s_fno;
        if (!on && was && (s_draw->m.look & ~FLUID_LOOK_DUST) == (CAT & ~FLUID_LOOK_DUST)) b.stood = s_fno;
        was = on;
        b.odd += !(word() || s_cur == ST_CREATURE) || (s_dir.pending && s_dir.next != FD_ST_TEXT && s_dir.next != FD_ST_CREATURE);
        if (s_fno == 70) bright_of(s_ref);
        if (s_fno > 70 && b.start < 0) { const float k = kept(); if (k < b.least_kept) b.least_kept = k; }
    }
    return b;
}

/* the lit pixels in the reader's box (where a say's word stands, over the cat's chest) */
static int box_lit(void) {
    const uint8_t *p = wm_pixels();
    int n = 0;
    for (int y = 205; y < 262; y++) for (int x = 150; x < 320; x++) { const uint8_t *q = p + ((size_t)y * PX + x) * 4; n += q[0] > LIT || q[1] > LIT || q[2] > LIT; }
    return n;
}

/* a say's words, then back to the cat by one change; the last word never comes back over the cat as the dust gathers
 * (the letters stay out of the new picture) */
static void case_say(const char *state, bool held) {
    CHECK(wm_creature("cat", s_cat, s_cat_len) == 0 && wm_start(PX, 20260516, 0) == 0, "power on");
    wm_device(1);
    frames(150);
    CHECK(!s_cuts, "power on to the cat: %d looks came at once", s_cuts);
    const int starts = s_starts;
    CHECK(wm_state(state) == 0, "the words");
    int most = 0, n = 0, start = -1, words = 0;
    bool was = false;
    for (int i = 0; i < 360 && (start < 0 || was || n < 10); i++) {
        if (held && i == 75) CHECK(wm_state("{\"state\":\"creature\"}") == 0, "home");
        frame();
        const bool on = fluid_draw_morphing(s_draw);
        if (s_cur == ST_TEXT) words = box_lit() > words ? box_lit() : words;
        if (on && !was && s_cur == ST_CREATURE) start = s_fno;
        if (start >= 0 && s_fno >= start + 20) { const int b = box_lit(); if (b > most) most = b; n += !on; }   /* (its gathering half) */
        was = on;
    }
    const int settled = box_lit();
    printf("%s: the words %d lit px in the reader's box; going: most %d, the cat settled %d\n", state, words, most, settled);
    CHECK(start > 0 && words > 2 * settled, "the words were up, then one change back to the cat (at frame %d)", start);
    CHECK(most <= settled * 115 / 100, "the word came back over the cat: %d lit px in its box, the cat alone %d", most, settled);
    CHECK(!s_cuts && s_starts - starts == 2, "the words came and went by a change each (%d at once, %d changes)", s_cuts, s_starts - starts);
    printf("  frames of a change over 33.4 ms in the panel model: %d of %d\n", s_slow, s_slow_n);
    CHECK(s_slow <= 2, "%d frames of its changes over the frame (30 a second the floor)", s_slow);
}

/* a question over a scene: the scene stays as it is until the question's change starts, which takes it from there; no
 * frame between shows a coarse stand-in (the glass's look, at once) */
static void case_ask_scene(const char *root) {
    char path[1024]; size_t len;
    snprintf(path, sizeof(path), "%s/packages/astrology/astrology.json", root);
    char *astro = slurp(path, &len);
    CHECK(wm_creature("cat", s_cat, s_cat_len) == 0 && wm_start(PX, 20260516, 0) == 0 && wm_package(astro, len) == 0, "power on");
    wm_device(1);
    frames(150);
    CHECK(wm_scene("globe", "{\"img\":\"moon\",\"lit\":0.5,\"side\":1,\"r\":0.3,\"spin\":0}", 40.f) == 0, "a scene");
    frames(90);
    const uint32_t scene = s_draw->m.look & ~FLUID_LOOK_DUST;
    CHECK((scene & 0x08000000u) && !fluid_draw_morphing(s_draw), "the scene stands (look %08x)", (unsigned)scene);
    CHECK(wm_ask("{\"title\":\"keep it?\",\"options\":[\"yes\",\"no\"]}") == 0, "a question");
    static uint8_t prev[PX * PX * 4];
    memcpy(prev, wm_pixels(), sizeof(prev));
    int odd = 0, cut = 0, worst = 0, starts = 0;
    bool was = false;
    for (int i = 0; i < 70; i++) {
        frame();
        const uint32_t look = s_draw->m.look & ~FLUID_LOOK_DUST;
        const bool on = fluid_draw_morphing(s_draw);
        odd += look != scene && !(look & 0x20000000u);   /* only the scene, then the question */
        cut += look != scene && !on && !was && starts == 0;
        starts += on && !was;
        if (!on && !was) {                               /* (outside a change, nothing may jump) */
            const uint8_t *p = wm_pixels();
            int ch = 0;
            for (int k = 0; k < PX * PX * 4; k++) if ((k & 3) != 3 && p[k] != prev[k]) { ch++; k |= 3; }
            if (ch > worst) worst = ch;
        }
        memcpy(prev, wm_pixels(), sizeof(prev));
        was = on;
    }
    printf("question over a scene: %d frames of another look, worst jump outside the change %d px\n", odd, worst);
    CHECK(odd == 0 && cut == 0 && starts == 1, "only the scene, then the question by one change (%d other, %d cut, %d changes)", odd, cut, starts);
    CHECK(worst <= 6000, "a frame outside the change changed %d px (a pop)", worst);
    free(astro);
}

/* the screen off by hand: the cat dissolves to an empty glass, then the panel sleeps and nothing is simulated or drawn;
 * on again: the panel black, the cat gathers on it by one short change starting the next frame */
static void case_screen(void) {
    CHECK(wm_creature("cat", s_cat, s_cat_len) == 0 && wm_start(PX, 20260516, 0) == 0, "power on");
    wm_device(1);
    frames(150);
    const int starts = s_starts;
    mole_fluid_screen(false);
    int off = -1;
    float lit_before = 1.f;
    for (int i = 0; i < 45 && off < 0; i++) { const float l = lit_share(); frame(); if (!mole_fluid_screen_on()) off = i + 1; else lit_before = l; }
    (void)lit_before;
    printf("screen off: dark and asleep after %.2f s\n", off / 30.f);
    CHECK(off > 20 && off <= 33 && s_starts - starts == 1 && !s_cuts, "off: one change to the empty glass, asleep within 1.1 s (%d frames)", off);
    CHECK(fluid_draw_morph_carries(s_draw) || s_draw->m.look == FLUID_LOOK_EMPTY, "the glass empty when it slept");
    const uint32_t n0 = s_frame_no;
    frames(60);
    CHECK(s_frame_no == n0 && !mole_fluid_screen_on(), "asleep: no frame simulated or drawn in 2 s (%u)", (unsigned)(s_frame_no - n0));
    mole_fluid_screen(true);
    frame();
    CHECK(mole_fluid_screen_on() && lit_share() < 0.002f && fluid_draw_morphing(s_draw), "on: the panel black, the gathering started the next frame");
    int gather = 0;
    while (fluid_draw_morphing(s_draw) && gather < 60) { frame(); gather++; }
    printf("screen on: gathered in %.2f s\n", (gather + 1) / 30.f);
    CHECK(gather >= 14 && gather <= 26 && !s_cuts && (s_draw->m.look & ~FLUID_LOOK_DUST) == (CAT & ~FLUID_LOOK_DUST), "the cat gathered in ~0.65 s (%d frames)", gather);
}

/* the nap: the cat asleep on battery for the set time: the same dissolve to dark, then asleep */
static void case_nap(void) {
    CHECK(wm_creature("cat", s_cat, s_cat_len) == 0 && wm_start(PX, 20261122, 0) == 0, "power on");
    CHECK(wm_state("{\"screen_off_battery_s\":10}") == 0, "the nap's setting");
    wm_device(1);
    frames(30);
    s_dir.cfg.sleep_after_s = 3.f; s_dir.cfg.yawn_lead_s = 1.f;
    mole_fluid_power(NAP_POWER_BATTERY);
    const int starts = s_starts;
    int i = 0;
    while (mole_fluid_screen_on() && i < 30 * 30) { frame(); i++; }
    CHECK(!mole_fluid_screen_on() && s_draw->m.look == FLUID_LOOK_EMPTY && s_starts - starts == 1 && !s_cuts, "the nap: by one change to the empty glass (%d changes)", s_starts - starts);
}

/* power off: the picture dissolves into "bye!", which holds, then to the empty glass: done in 2.5 s; no blob, no pond */
static void case_bye(void) {
    CHECK(wm_creature("cat", s_cat, s_cat_len) == 0 && wm_start(PX, 20260516, 0) == 0, "power on");
    wm_device(1);
    frames(150);
    const int starts = s_starts;
    mole_fluid_goodbye();
    int i = 0, odd = 0, words = 0;
    while (!mole_fluid_goodbye_done() && i < 150) {
        frame(); i++;
        odd += (s_cur != ST_TEXT && s_cur != ST_CREATURE) || !mole_fluid_screen_on();
        words += s_cur == ST_TEXT && !fluid_draw_morphing(s_draw);
    }
    printf("power off: done after %.2f s, the word whole %.2f s\n", i / 30.f, words / 30.f);
    CHECK(mole_fluid_goodbye_done() && i <= 78, "done within 2.6 s (%.2f s)", i / 30.f);
    CHECK(!odd && !s_cuts && s_starts - starts == 2 && words >= 8, "the word by a change, then dark by another; nothing else (%d odd, %d changes)", odd, s_starts - starts);
    CHECK(lit_share() < 0.002f && s_draw->m.look == FLUID_LOOK_EMPTY, "the glass empty when done");
    mole_fluid_screen(false); mole_fluid_screen(true);
    frames(30);
    CHECK(mole_fluid_goodbye_done() && lit_share() < 0.002f, "nothing brings a picture back after it");
}

/* a restart the firmware chose (mole_fluid_dark): the picture dissolves to the empty glass, the panel stays on */
static void case_restart(void) {
    CHECK(wm_creature("cat", s_cat, s_cat_len) == 0 && wm_start(PX, 20260516, 0) == 0, "power on");
    wm_device(1);
    frames(150);
    atomic_store(&s_dark_req, true);   /* (mole_fluid_dark's request; its wait is a task's) */
    int i = 0;
    while (!atomic_load(&s_dark_done) && i < 60) { frame(); i++; }
    CHECK(atomic_load(&s_dark_done) && i <= 33 && mole_fluid_screen_on() && lit_share() < 0.002f && !s_cuts, "dark in %.2f s, the panel on", i / 30.f);
}

/* creatures: a change between two of them is one change of picture, either drawn as lines or in the fluid; so is the
 * way home from a question with a creature drawn in the fluid */
static void case_creatures(const char *root) {
    char path[1024]; size_t len;
    snprintf(path, sizeof(path), "%s/packages/octopus/octopus.json", root);
    char *oct = slurp(path, &len);
    size_t dlen;
    snprintf(path, sizeof(path), "%s/packages/duck/duck.json", root);
    char *duck = slurp(path, &dlen);
    CHECK(wm_creature("cat", s_cat, s_cat_len) == 0 && wm_start(PX, 20260516, 0) == 0, "power on");
    wm_device(1);
    frames(150);
    const char *name[] = { "cat to octopus", "octopus to duck", "duck to cat" };
    for (int c = 0; c < 3; c++) {
        const int starts = s_starts;
        CHECK((c == 0 ? wm_creature("octopus", oct, len) : c == 1 ? wm_creature("duck", duck, dlen) : wm_creature("cat", s_cat, s_cat_len)) == 0, "%s", name[c]);
        frames(90);
        CHECK(s_starts - starts == 1 && !s_cuts, "%s: one change of picture (%d, %d at once)", name[c], s_starts - starts, s_cuts);
    }
    CHECK(wm_creature("octopus", oct, len) == 0, "the octopus"); frames(90);
    const int starts = s_starts;
    CHECK(wm_ask("{\"title\":\"keep it?\",\"options\":[\"yes\",\"no\"]}") == 0, "a question"); frames(90);
    wm_ask_end(); frames(90);
    CHECK(s_starts - starts == 2 && !s_cuts, "the octopus to a question and home: a change each (%d, %d at once)", s_starts - starts, s_cuts);
    free(oct); free(duck);
}

/* the old demo states (the blob, "thinking", the effects, the reel) are refused as any unknown value is, and change
 * nothing on the glass */
static void case_refused(void) {
    CHECK(wm_creature("cat", s_cat, s_cat_len) == 0 && wm_start(PX, 20260516, 0) == 0, "power on");
    wm_device(1);
    frames(150);
    const int starts = s_starts;
    static const char *const old[] = { "blob", "thinking", "effect:ripple", "effect:vortex", "effect:wave", "autoplay", "pond" };
    for (int i = 0; i < 7; i++) {
        char j[64];
        snprintf(j, sizeof(j), "{\"state\":\"%s\"}", old[i]);
        CHECK(wm_state(j) != 0, "state %s refused", old[i]);
        frames(10);
    }
    char *g = NULL;
    CHECK(mole_fluid_creature_state_get_json(&g) == ESP_OK && g && strstr(g, "\"state\":\"creature\"") && !strstr(g, "autoplay") && !strstr(g, "reel_step"),
          "GET: the creature, no autoplay or reel fields (%.80s)", g ? g : "");
    free(g);
    CHECK(s_starts == starts && !s_cuts && s_cur == ST_CREATURE, "nothing changed on the glass");
}

/* each case is a power on of its own (the module starts once): picture_flow_test <repo root> <case> */
static void case_early(void) {   /* the cat made in the first frame (the module's way); it stands 1.2 s after the word's 2.8 s */
    power_on(0, false);
    boot_t b = boot_run(300, -1);
    printf("early cat: the word gathered by %.2f s, its change at %.2f s, the cat stands at %.2f s\n", b.gather_end / 30.f, b.start / 30.f, b.stood / 30.f);
    CHECK(b.gather == 1 && b.gather_end >= 15 && b.gather_end <= 30, "power on: the word gathers on the empty glass in ~0.65 s (%d, done at frame %d)", b.gather, b.gather_end);
    CHECK(b.odd == 0, "early: %d frames showed something but the word, its change or the cat", b.odd);
    CHECK(b.start >= 83 && b.start <= 86 && b.changes == 1, "early: one change, at the word's 2.8 s (frame %d, %d changes)", b.start, b.changes);
    CHECK(b.least_kept >= 0.97f, "early: the word whole till its change (%.2f kept)", b.least_kept);
    CHECK(b.stood > 0 && b.stood <= 125, "early: the cat stands by 4.2 s (frame %d)", b.stood);
}

static void case_late(void) {    /* the cat made at 4.5 s: the word waits for it, one change straight into it */
    power_on(135, false);
    boot_t b = boot_run(400, -1);
    printf("late cat (made at 4.5 s): change at %.2f s, the cat stands at %.2f s\n", b.start / 30.f, b.stood / 30.f);
    CHECK(b.odd == 0, "late: %d frames showed something but the word, its change or the cat (the blob?)", b.odd);
    CHECK(b.start >= 135 && b.start <= 138 && b.changes == 1, "late: one change, as the cat is made (frame %d, %d changes)", b.start, b.changes);
    CHECK(b.least_kept >= 0.97f, "late: the word whole till its change (%.2f kept)", b.least_kept);
    CHECK(b.stood > 0 && b.stood <= b.start + 42, "late: the cat stands 1.2 s after it is made (frame %d)", b.stood);
}

/* never made, or made but failing to build: the word for good, lit; a say over it comes back to it */
static void case_none(bool bad) {
    const char *what = bad ? "fails to build" : "never made";
    power_on(bad ? 60 : -1, bad);
    boot_t b = boot_run(300, -1);
    float least = 1.f;
    for (int i = 0; i < 30; i++) { frame(); const float l = lit_share(); if (l < least) least = l; }
    CHECK(b.odd == 0 && b.changes == 0 && b.stood < 0, "%s: only the word for 11 s (%d odd frames, %d changes)", what, b.odd, b.changes);
    CHECK(b.least_kept >= 0.97f && least > 0.01f, "%s: the word whole and lit (%.2f kept, %.4f lit)", what, b.least_kept, least);
    CHECK(!cr_loaded() && (!bad || s_cute_err[0]), "%s: no creature, its error kept (\"%s\")", what, s_cute_err);
    CHECK(wm_state("{\"state\":\"text\",\"text\":\"still here\"}") == 0, "a say");
    int said = 0, odd = 0;
    for (int i = 0; i < 150; i++) { frame(); said += s_cur == ST_TEXT && !fd_boot_word(&s_dir); odd += s_cur != ST_TEXT && s_cur != ST_CREATURE; }
    CHECK(said > 30 && odd == 0 && word() && !s_dir.pending, "%s: the say's words, then the word again (%d frames of words, %d of the blob)", what, said, odd);
}

/* the relay's first welcome while the word is up (the cat early or late): the word stays whole, the rebuild comes once
 * the cat stands */
static void case_welcome_word(int late) {
    power_on(late, false);
    frames(late ? 105 : 49);
    bright_of(s_ref);
    mole_fluid_allow_internal();
    float least = 1.f;
    for (int i = 0; i < 25; i++) { frame(); const float k = kept(); if (k < least) least = k; }
    CHECK(least >= 0.97f && s_req_internal, "welcome during the word: it stays whole (%.2f kept), the rebuild waits", least);
    boot_t b = boot_run(400, -1);
    CHECK(b.odd == 0 && b.changes == 1 && b.stood > 0, "...then the word's one change into the cat");
    frames(2);
    CHECK(!s_req_internal, "the rebuild, once the cat stands");
}

static void case_welcome_change(void) {   /* during the word's change into the cat: the change runs its whole length */
    power_on(0, false);
    boot_t b = boot_run(300, 98);
    const int len = b.stood - b.start;
    CHECK(b.start > 0 && b.start < 98 && b.changes == 1 && len >= 34 && len <= 40, "welcome during the change: one change, its whole length (%d frames)", len);
    frames(2);
    CHECK(!s_req_internal, "the rebuild, after the change");
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: picture_flow_test <repo root> <case>\n"); return 2; }
    char path[1024];
    snprintf(path, sizeof(path), "%s/packages/cat/cat.json", argv[1]);
    s_cat = slurp(path, &s_cat_len);
    const char *c = argv[2];
    if (!strcmp(c, "early")) case_early();
    else if (!strcmp(c, "late")) case_late();
    else if (!strcmp(c, "never")) case_none(false);
    else if (!strcmp(c, "broken")) case_none(true);
    else if (!strcmp(c, "welcome-word")) case_welcome_word(0);
    else if (!strcmp(c, "welcome-late")) case_welcome_word(135);
    else if (!strcmp(c, "welcome-change")) case_welcome_change();
    else if (!strcmp(c, "say")) case_say("{\"state\":\"text\",\"text\":\"Build passed. Three tests were flaky.\"}", false);
    else if (!strcmp(c, "hold")) {   /* a held word, then home */
        case_say("{\"state\":\"text\",\"text\":\"update\",\"hold\":true}", true);
    }
    else if (!strcmp(c, "ask-scene")) case_ask_scene(argv[1]);
    else if (!strcmp(c, "screen")) case_screen();
    else if (!strcmp(c, "nap")) case_nap();
    else if (!strcmp(c, "bye")) case_bye();
    else if (!strcmp(c, "restart")) case_restart();
    else if (!strcmp(c, "creatures")) case_creatures(argv[1]);
    else if (!strcmp(c, "refused")) case_refused();
    else { fprintf(stderr, "picture_flow_test: no case %s\n", c); return 2; }
    printf("picture_flow_test %s: %d passed, %d failed\n", c, s_pass, s_fail);
    return s_fail ? 1 : 0;
}
