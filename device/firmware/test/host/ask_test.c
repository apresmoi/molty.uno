/*
 * ask_test.c — host test for a question's layout, answers and phases (main/fluid/fluid_ask.c), against
 * device/creatures/js/ask-options.js ("Rim arcs" for 1..3 answers, "Underlined rows" for 4..5): the whole title
 * and every label at its limit (14 / 10 / 8 letters) shown in full, readable, inside the glass and apart; a tap on
 * a label picks it, one on the question picks nothing; the screen's phases.
 *
 * Build + run: test/host/ask_build.sh
 */
#include "fluid_ask.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int s_fail = 0, s_pass = 0;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static fluid_ask_q_t question(const char *title, const char *detail, const char *code, int n, const char *const *labels) {
    fluid_ask_q_t q;
    memset(&q, 0, sizeof(q));
    snprintf(q.title, sizeof(q.title), "%s", title);
    snprintf(q.detail, sizeof(q.detail), "%s", detail);
    snprintf(q.code, sizeof(q.code), "%s", code);
    q.n = n;
    for (int i = 0; i < n; i++) { snprintf(q.label[i], sizeof(q.label[i]), "%s", labels[i]); q.rgb[i] = fluid_ask_color(-1, i); }
    return q;
}
/* an item's box in glass units */
static void box(const fluid_ask_item_t *it, float b[4]) {
    const float hw = (it->len * 6 - 1) * it->px / 206.f / 2.f, hh = 3.5f * it->px / 206.f;
    b[0] = it->x - hw; b[1] = it->x + hw; b[2] = it->y - hh; b[3] = it->y + hh;
}
static bool inside(const float b[4], float r) {
    for (int c = 0; c < 4; c++) { const float x = b[c & 1], y = b[2 + (c >> 1)]; if (x * x + y * y > r * r) return false; }
    return true;
}
static bool apart(const float a[4], const float b[4]) { return a[1] <= b[0] || b[1] <= a[0] || a[3] <= b[2] || b[3] <= a[2]; }

static void test_limits(void) {
    CHECK(fluid_ask_label_max(1) == 14 && fluid_ask_label_max(2) == 14 && fluid_ask_label_max(3) == 10 &&
          fluid_ask_label_max(4) == 8 && fluid_ask_label_max(5) == 8, "14 for 1..2 answers, 10 for 3, 8 for 4..5");
}

/* the install question: its name in full ("ASTROLOG" was the old 8-letter cut), a detail line, two arcs */
static void test_install(void) {
    static const char *const L[2] = { "install", "not now" };
    const fluid_ask_q_t q = question("install astrology?", "a new package for molty", "", 2, L);
    fluid_ask_layout_t lay;
    fluid_ask_lay(&q, &lay);
    CHECK(!lay.hit.rows && lay.ntargets == 2 && lay.tg[0].kind == FLUID_ASK_ARC, "1..3 answers: rim arcs");
    CHECK(lay.nitems == 4 && !strcmp(lay.item[0].str, "INSTALL ASTROLOGY?") && lay.item[0].role == FLUID_ASK_TITLE,
          "the whole name, one line, capitals (%s)", lay.item[0].str);
    CHECK(lay.item[0].px > 2.4f && lay.item[1].role == FLUID_ASK_DETAIL && lay.item[1].px < lay.item[0].px, "title big, the detail smaller (%.2f, %.2f)", lay.item[0].px, lay.item[1].px);
    CHECK(!strcmp(lay.item[2].str, "INSTALL") && lay.item[2].opt == 0 && !strcmp(lay.item[3].str, "NOT NOW") && lay.item[3].opt == 1, "the answers");
    CHECK(lay.item[2].x < 0.f && lay.item[3].x > 0.f && lay.item[2].y > lay.item[1].y, "install on the left, not now on the right, under the question");
}

/* every count at its longest labels and a 40-letter title: all inside the glass, nothing on top of anything */
static void test_fit(void) {
    static const char *const LONG[5] = { "ABCDEFGHIJKLMN", "NOPQRSTUVWXYZA", "ABCDEFGHIJ", "ABCDEFGH", "ABCDEFGH" };
    for (int n = 1; n <= FLUID_ASK_MAX; n++) {
        const char *labels[5];
        char buf[5][16];
        for (int i = 0; i < n; i++) { snprintf(buf[i], sizeof(buf[i]), "%.*s", fluid_ask_label_max(n), LONG[i] ); buf[i][0] = (char)('A' + i); labels[i] = buf[i]; }
        for (int hdr = 0; hdr < 3; hdr++) {
            const fluid_ask_q_t q = question(hdr ? "send the whole summary to ana tomorrow?" : "", hdr ? "your agent asks" : "", hdr == 2 ? "4719" : "", n, labels);
            fluid_ask_layout_t lay;
            fluid_ask_lay(&q, &lay);
            CHECK(lay.hit.rows == (n >= 4), "n=%d: %s", n, n >= 4 ? "rows" : "arcs");
            float b[FLUID_ASK_ITEMS][4];
            int titles = 0;
            for (int i = 0; i < lay.nitems; i++) {
                const fluid_ask_item_t *it = &lay.item[i];
                box(it, b[i]);
                CHECK(inside(b[i], 0.95f), "n=%d hdr %d: \"%s\" inside the glass", n, hdr, it->str);
                if (it->role == FLUID_ASK_LABEL) {
                    CHECK(it->len == fluid_ask_label_max(n) && it->px >= FLUID_ASK_PX_MIN - 1e-4f, "n=%d: label %s whole (%d letters) at %.2f px", n, it->str, it->len, it->px);
                }
                if (it->role == FLUID_ASK_TITLE) { titles++; CHECK(it->px >= 2.f, "the title readable (%.2f px)", it->px); }
                for (int j = 0; j < i; j++) CHECK(apart(b[i], b[j]), "n=%d hdr %d: \"%s\" and \"%s\" apart", n, hdr, it->str, lay.item[j].str);
            }
            if (hdr) CHECK(titles == 2, "a 40-letter title on two lines (%d)", titles);
            /* a tap on each label picks it; on the title, nothing */
            for (int i = 0; i < lay.nitems; i++) {
                const fluid_ask_item_t *it = &lay.item[i];
                const int p = fluid_ask_pick(&lay.hit, it->x, it->y);
                if (it->role == FLUID_ASK_LABEL) CHECK(p == it->opt, "n=%d hdr %d: a tap on %s picks %d (got %d)", n, hdr, it->str, it->opt, p);
                else CHECK(p == -1, "n=%d hdr %d: a tap on the question (%s) picks nothing (%d)", n, hdr, it->str, p);
            }
            /* the arcs: each one's middle picks its own option */
            for (int t = 0; !lay.hit.rows && t < lay.ntargets; t++) {
                const float m = 0.5f * (lay.tg[t].b + lay.tg[t].c);
                CHECK(fluid_ask_pick(&lay.hit, 0.95f * sinf(m), -0.95f * cosf(m)) == lay.tg[t].opt, "n=%d: arc %d picks its option", n, t);
            }
        }
    }
    /* the centre and the upper half answer nothing (rim arcs) */
    static const char *const YN[2] = { "yes", "no" };
    const fluid_ask_q_t q = question("ok?", "", "", 2, YN);
    fluid_ask_layout_t lay; fluid_ask_lay(&q, &lay);
    CHECK(fluid_ask_pick(&lay.hit, 0.f, 0.f) == -1 && fluid_ask_pick(&lay.hit, 0.5f, -0.5f) == -1 && fluid_ask_pick(&lay.hit, -0.5f, 0.5f) == 0 && fluid_ask_pick(&lay.hit, 0.5f, 0.5f) == 1,
          "centre and top: none; lower left: 0; lower right: 1");
    /* a label over its limit is cut to it */
    static const char *const OVER[3] = { "ABCDEFGHIJKLMN", "B", "C" };
    const fluid_ask_q_t q3 = question("", "", "", 3, OVER);
    fluid_ask_lay(&q3, &lay);
    CHECK(lay.item[0].len == 10, "3 answers: 10 letters at most (%d)", lay.item[0].len);
}

/* the pairing code: the digits spaced, big, between the detail and the answers */
static void test_code(void) {
    static const char *const AR[2] = { "accept", "reject" };
    const fluid_ask_q_t q = question("pair with claude?", "check the code matches", "4719", 2, AR);
    fluid_ask_layout_t lay; fluid_ask_lay(&q, &lay);
    CHECK(lay.nitems == 5 && lay.item[2].role == FLUID_ASK_CODE && !strcmp(lay.item[2].str, "4 7 1 9") && lay.item[2].px > 5.f, "the code (%s)", lay.item[2].str);
    CHECK(lay.item[2].y < lay.item[3].y && lay.item[2].y > lay.item[1].y, "under the detail, over the answers");
}

static void test_follow(void) {
    fluid_ask_screen_t q = { 0 };
    int did = fluid_ask_follow(&q, true, 1, -1, 2);
    CHECK(did == FLUID_ASK_DID_SHOW && q.phase == FLUID_ASK_SHOW && q.seq == 1, "show");
    /* tappable only once formed enough (t·speed ≥ FLUID_ASK_READY), at any speed */
    CHECK(fluid_ask_tappable(&q, 1.f) == 0, "not formed at t = 0");
    q.t = 0.59f; CHECK(fluid_ask_tappable(&q, 1.f) == 0, "t 0.59 at speed 1");
    q.t = 0.6f;  CHECK(fluid_ask_tappable(&q, 1.f) == 1, "t 0.6 at speed 1");
    CHECK(fluid_ask_tappable(&q, 0.4f) == 0, "slow speed: still forming");
    /* a new question replaces it */
    did = fluid_ask_follow(&q, true, 2, -1, 3);
    CHECK(did == FLUID_ASK_DID_SHOW && q.seq == 2 && q.t == 0.f && fluid_ask_tappable(&q, 2.5f) == 0, "replace");
    /* answered: blows away over FLUID_ASK_GONE_S, never tappable meanwhile */
    q.t = 1.f;
    did = fluid_ask_follow(&q, true, 2, 1, 3);
    CHECK(did == FLUID_ASK_DID_PICK && q.phase == FLUID_ASK_PICK && q.pick == 1 && q.out_t == 0.f && fluid_ask_tappable(&q, 1.f) == 0, "pick");
    CHECK(fluid_ask_follow(&q, false, 2, 1, 3) == 0 && q.phase == FLUID_ASK_PICK, "the link ending it changes nothing: its dust goes on");
    q.out_t = FLUID_ASK_GONE_S - 0.01f; CHECK(fluid_ask_follow(&q, true, 2, 1, 3) == 0, "still blowing away");
    q.out_t = FLUID_ASK_GONE_S;
    did = fluid_ask_follow(&q, true, 2, 1, 3);
    CHECK(did == FLUID_ASK_DID_GONE && q.phase == FLUID_ASK_NONE, "gone");
    /* ended unanswered: OUT, then gone */
    fluid_ask_follow(&q, true, 3, -1, 2);
    did = fluid_ask_follow(&q, false, 3, -1, 2);
    CHECK(did == FLUID_ASK_DID_LEAVE && q.phase == FLUID_ASK_OUT && q.out_t == 0.f, "end: the rim lets go");
    q.out_t = FLUID_ASK_GONE_S;
    CHECK(fluid_ask_follow(&q, false, 3, -1, 2) == FLUID_ASK_DID_GONE && q.phase == FLUID_ASK_NONE, "then gone");
    /* a new question over one blowing away replaces it at once */
    fluid_ask_follow(&q, true, 4, -1, 2); fluid_ask_follow(&q, false, 4, -1, 2);
    did = fluid_ask_follow(&q, true, 5, -1, 2);
    CHECK(did == FLUID_ASK_DID_SHOW && q.phase == FLUID_ASK_SHOW && q.seq == 5, "a new one over the dust");
    /* answered before it was ever drawn: show + pick in one frame; out-of-range pick → 0 */
    did = fluid_ask_follow(&q, true, 9, 7, 2);
    CHECK(did == (FLUID_ASK_DID_SHOW | FLUID_ASK_DID_PICK) && q.pick == 0, "show + pick");
}

/* colours: the agent's, else green, red, amber, blue, violet by index */
static void test_colors(void) {
    static const uint32_t want[5] = { 0x3ccf96, 0xff6b6b, 0xffa94d, 0x4dabf7, 0xb197fc };
    for (int i = 0; i < FLUID_ASK_MAX; i++) {
        CHECK(fluid_ask_color(-1, i) == want[i], "default %d = %06x", i, (unsigned)fluid_ask_color(-1, i));
        CHECK(fluid_ask_color(0x123456, i) == 0x123456 && fluid_ask_color(0, i) == 0, "given %d", i);
    }
}

int main(void) {
    printf("ask host test\n");
    test_limits();
    test_colors();
    test_install();
    test_fit();
    test_code();
    test_follow();
    printf("%d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
