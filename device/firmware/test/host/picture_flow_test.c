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
 *     word never melts and the change is never cut short; the rebuild still happens once the picture is still.
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
static void frame(void) { bg_tick(); wm_frame(DT); s_fno++; char *e = wm_events(); free(e); }
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
typedef struct { int start, stood, changes, odd; float least_kept; } boot_t;
static boot_t boot_run(int until, int welcome_at) {
    boot_t b = { -1, -1, 0, 0, 1.f };
    bool was = false;
    const int ends0 = s_draw ? s_draw->m.ends : 0;
    while (s_fno < until && b.stood < 0) {
        if (s_fno == welcome_at) mole_fluid_allow_internal();
        frame();
        const bool on = fluid_draw_morphing(s_draw);
        if (on && !was) { b.changes++; if (b.start < 0) b.start = s_fno; }
        if (!on && was && (s_draw->m.look & ~FLUID_LOOK_DUST) == (CAT & ~FLUID_LOOK_DUST)) b.stood = s_fno;
        was = on;
        b.odd += !(word() || s_cur == ST_CREATURE) || s_cur == ST_BLOB || s_dir.cur == FD_ST_BLOB || (s_dir.pending && s_dir.next == FD_ST_BLOB);
        if (s_fno == 70) bright_of(s_ref);
        if (s_fno > 70 && b.start < 0) { const float k = kept(); if (k < b.least_kept) b.least_kept = k; }
    }
    b.changes = s_draw->m.ends - ends0 > b.changes ? s_draw->m.ends - ends0 : b.changes;
    return b;
}

/* each case is a power on of its own (the module starts once): picture_flow_test <repo root> <case> */
static void case_early(void) {   /* the cat made in the first frame (the module's way); it stands 1.2 s after the word's 2.8 s */
    power_on(0, false);
    boot_t b = boot_run(300, -1);
    printf("early cat: change at %.2f s, the cat stands at %.2f s\n", b.start / 30.f, b.stood / 30.f);
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
    for (int i = 0; i < 150; i++) { frame(); said += s_cur == ST_TEXT && !fd_boot_word(&s_dir); odd += s_cur == ST_BLOB || s_dir.cur == FD_ST_BLOB; }
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
    else { fprintf(stderr, "picture_flow_test: no case %s\n", c); return 2; }
    printf("picture_flow_test %s: %d passed, %d failed\n", c, s_pass, s_fail);
    return s_fail ? 1 : 0;
}
