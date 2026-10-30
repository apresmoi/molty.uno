/*
 * ask_test.c — host test for the question screen's geometry (main/fluid/fluid_ask.c):
 * the sector a tap lands in for n = 1..5 (dividers included), the reading order of the
 * options, and the label layout (every 8-letter label fits its sector at a readable size,
 * and a tap on a label picks that label).
 *
 * Build + run: test/host/ask_build.sh
 */
#include "fluid_ask.h"

#include <math.h>
#include <stdio.h>

static int s_fail = 0, s_pass = 0;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define DEG (3.14159265f / 180.f)

/* a point at `deg` clockwise from 12 o'clock, r px from the centre (y down) */
static float px(float deg, float r) { return sinf(deg * DEG) * r; }
static float py(float deg, float r) { return -cosf(deg * DEG) * r; }

static void test_order(void) {
    /* option i → sector (clockwise from 12): the reading order of the sector centres */
    static const int want[6][FLUID_ASK_MAX] = {
        { 0 }, { 0 },
        { 1, 0 },                 /* left, right                                       */
        { 2, 0, 1 },              /* upper left, upper right, bottom (the website's)   */
        { 3, 0, 2, 1 },           /* TL TR BL BR                                       */
        { 4, 0, 3, 1, 2 },        /* top left, top right, lower left, lower right, bottom */
    };
    for (int n = 1; n <= FLUID_ASK_MAX; n++) {
        int got[FLUID_ASK_MAX];
        fluid_ask_order(n, got);
        for (int i = 0; i < n; i++) CHECK(got[i] == want[n][i], "n=%d option %d in sector %d, want %d", n, i, got[i], want[n][i]);
    }
}

static void test_pick(void) {
    /* n = 1: anywhere, even the panel's corners */
    CHECK(fluid_ask_pick(1, 0.f, 0.f) == 0 && fluid_ask_pick(1, -205.f, -205.f) == 0 && fluid_ask_pick(1, 150.f, 90.f) == 0, "n=1");
    /* n = 2: left / right halves */
    CHECK(fluid_ask_pick(2, -60.f, 10.f) == 0 && fluid_ask_pick(2, -1.f, -150.f) == 0 && fluid_ask_pick(2, -1.f, 150.f) == 0, "n=2 left");
    CHECK(fluid_ask_pick(2, 60.f, -10.f) == 1 && fluid_ask_pick(2, 1.f, -150.f) == 1 && fluid_ask_pick(2, 1.f, 150.f) == 1, "n=2 right");
    /* on the divider: the sector clockwise of it (straight up → right half, straight
     * down → left half); the centre is sector 0 (the one clockwise of 12 o'clock) */
    CHECK(fluid_ask_pick(2, 0.f, -100.f) == 1 && fluid_ask_pick(2, 0.f, 100.f) == 0 && fluid_ask_pick(2, 0.f, 0.f) == 1, "n=2 divider");
    CHECK(fluid_ask_pick(2, -0.f, -100.f) == 1, "n=2: −0 is straight up");
    /* n = 4: the quadrants, and the axis-aligned dividers exactly */
    CHECK(fluid_ask_pick(4, -80.f, -80.f) == 0 && fluid_ask_pick(4, 80.f, -80.f) == 1 &&
          fluid_ask_pick(4, -80.f, 80.f) == 2 && fluid_ask_pick(4, 80.f, 80.f) == 3, "n=4 quadrants");
    CHECK(fluid_ask_pick(4, 0.f, -50.f) == 1 && fluid_ask_pick(4, 50.f, 0.f) == 3 &&
          fluid_ask_pick(4, 0.f, 50.f) == 2 && fluid_ask_pick(4, -50.f, 0.f) == 0, "n=4 dividers");
    /* n = 3: the website's yes (upper left) / no (upper right) / idk (bottom) */
    CHECK(fluid_ask_pick(3, -90.f, -60.f) == 0 && fluid_ask_pick(3, 90.f, -60.f) == 1 && fluid_ask_pick(3, 0.f, 120.f) == 2, "n=3");
    /* n = 5 */
    CHECK(fluid_ask_pick(5, -60.f, -120.f) == 0 && fluid_ask_pick(5, 60.f, -120.f) == 1 && fluid_ask_pick(5, -150.f, 40.f) == 2 &&
          fluid_ask_pick(5, 150.f, 40.f) == 3 && fluid_ask_pick(5, 0.f, 150.f) == 4, "n=5");
    /* every divider, from every n: half a degree either side lands in the two sectors it
     * separates, near the centre and out at the rim */
    for (int n = 2; n <= FLUID_ASK_MAX; n++) for (int k = 0; k < n; k++) for (int ri = 0; ri < 2; ri++) {
        float a = 360.f * k / n, r = ri ? 200.f : 12.f;
        int cw = fluid_ask_sector(n, px(a + 0.5f, r), py(a + 0.5f, r));
        int ccw = fluid_ask_sector(n, px(a - 0.5f, r), py(a - 0.5f, r));
        CHECK(cw == k && ccw == (k + n - 1) % n, "n=%d divider %d r=%.0f: sectors %d|%d", n, k, r, ccw, cw);
    }
}

static void test_layout(int stage) {
    const float r = stage * 0.5f, sc = stage / 412.f;
    for (int n = 1; n <= FLUID_ASK_MAX; n++) {
        int lens[FLUID_ASK_MAX], sector[FLUID_ASK_MAX];
        for (int i = 0; i < n; i++) lens[i] = FLUID_ASK_LABEL_MAX;
        float cx[FLUID_ASK_MAX], cy[FLUID_ASK_MAX];
        float fs = fluid_ask_layout(n, lens, stage, cx, cy);
        fluid_ask_order(n, sector);
        printf("  stage %d n=%d: 8 letters at %.1f px (%.1f px wide)\n", stage, n, fs, 8 * 0.6f * fs);
        if (stage == 412) CHECK(fs >= FLUID_ASK_FS_MIN, "n=%d: 8 letters only fit at %.1f px", n, fs);
        CHECK(fs <= FLUID_ASK_FS_MAX * sc + 1e-3f, "n=%d: %.1f px above the cap", n, fs);
        for (int i = 0; i < n; i++) {
            float x = cx[i] - r, y = cy[i] - r;
            CHECK(fluid_ask_fits(n, sector[i], lens[i], fs, x, y, r, sc), "n=%d option %d does not fit its sector", n, i);
            CHECK(fluid_ask_pick(n, x, y) == i, "n=%d: a tap on label %d picks %d", n, i, fluid_ask_pick(n, x, y));
            /* every corner of the label's box picks it too */
            float hw = lens[i] * 0.6f * fs * 0.5f, hh = 1.05f * fs * 0.5f;
            for (int c = 0; c < 4; c++)
                CHECK(fluid_ask_pick(n, x + (c & 1 ? hw : -hw), y + (c & 2 ? hh : -hh)) == i, "n=%d label %d corner %d", n, i, c);
        }
        /* shorter labels never get a smaller size */
        for (int i = 0; i < n; i++) lens[i] = 3;
        float fs3 = fluid_ask_layout(n, lens, stage, cx, cy);
        CHECK(fs3 >= fs, "n=%d: 3 letters %.1f px < 8 letters %.1f px", n, fs3, fs);
    }
    /* one long label sets the size for all (one type size per question) */
    int mixed[3] = { 2, 8, 3 };
    float cx[3], cy[3], fs = fluid_ask_layout(3, mixed, stage, cx, cy);
    int eight[3] = { 8, 8, 8 };
    float fs8 = fluid_ask_layout(3, eight, stage, cx, cy);
    CHECK(fabsf(fs - fs8) < 0.6f * sc + 0.5f, "stage %d: mixed lengths %.1f px vs all-8 %.1f px", stage, fs, fs8);
}

/* the fit test itself: a box over a divider or past the rim does not fit */
static void test_fits(void) {
    CHECK(fluid_ask_fits(1, 0, 8, 30.f, 0.f, 0.f, 206.f, 1.f), "n=1 centred");
    CHECK(!fluid_ask_fits(1, 0, 8, 30.f, 0.f, 190.f, 206.f, 1.f), "n=1 past the rim");
    CHECK(!fluid_ask_fits(2, 0, 4, 30.f, 0.f, 0.f, 206.f, 1.f), "n=2 straddling the divider");
    CHECK(fluid_ask_fits(2, 0, 4, 30.f, 60.f, 0.f, 206.f, 1.f) && !fluid_ask_fits(2, 1, 4, 30.f, 60.f, 0.f, 206.f, 1.f), "n=2 right half");
    CHECK(!fluid_ask_fits(4, 0, 2, 20.f, 3.f, -100.f, 206.f, 1.f), "n=4 within the gap of 12 o'clock");
}

int main(void) {
    printf("ask host test\n");
    test_order();
    test_pick();
    test_fits();
    test_layout(412);
    test_layout(240);
    printf("%d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
