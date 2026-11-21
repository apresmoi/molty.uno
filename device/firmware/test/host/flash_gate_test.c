/* flash_gate_test: main/diag/flash_gate.c: a write that can wait holds while the screen animates, goes once it has
 * been calm, at the cap, or at once before a restart; nothing it owes is lost; the flash's ops are logged a batch a line. */
#include "flash_gate.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MS 1000LL
#define S  1000000LL

/* the screen's calm (us) at t for a timeline of animations [on, off) */
typedef struct { int64_t on, off; } span_t;
static int64_t calm_at(const span_t *a, int n, int64_t t)
{
    int64_t last_off = 0;
    for (int i = 0; i < n; i++) {
        if (t >= a[i].on && t < a[i].off) return 0;
        if (a[i].off <= t && a[i].off > last_off) last_off = a[i].off;
    }
    return t - last_off;
}

static void test_hold_and_go(void)
{
    fg_hold_t h = {0}; fg_stats_t st = {0};
    /* calm long enough: goes at once, nothing held */
    assert(fg_step(&h, &st, 10 * S, FG_CALM_US, false) && st.held == 0 && st.flushed == 0);
    /* animating: held (counted once), still held while it animates or has been calm only a little */
    assert(!fg_step(&h, &st, 20 * S, 0, false) && st.held == 1);
    assert(!fg_step(&h, &st, 21 * S, 0, false) && st.held == 1);
    assert(!fg_step(&h, &st, 22 * S, FG_CALM_US - 1, false));
    /* calm FG_CALM_US: goes; the hold counted flushed, its length kept */
    assert(fg_step(&h, &st, 23 * S, FG_CALM_US, false));
    assert(st.flushed == 1 && st.capped == 0 && st.hold_max_ms == 3000 && !h.held);
    /* a new write starts its own hold */
    assert(!fg_step(&h, &st, 30 * S, 0, false) && st.held == 2);
    assert(fg_step(&h, &st, 31 * S, 2 * S, false) && st.flushed == 2 && st.hold_max_ms == 3000);
}

static void test_cap(void)
{
    fg_hold_t h = {0}; fg_stats_t st = {0};
    assert(!fg_step(&h, &st, 5 * S, 0, false));
    assert(!fg_step(&h, &st, 5 * S + FG_HOLD_MAX_US - 1, 0, false));
    assert(fg_step(&h, &st, 5 * S + FG_HOLD_MAX_US, 0, false));        /* still animating: the cap */
    assert(st.held == 1 && st.flushed == 1 && st.capped == 1 && st.hold_max_ms == FG_HOLD_MAX_US / 1000);
}

static void test_force(void)
{
    fg_hold_t h = {0}; fg_stats_t st = {0};
    assert(!fg_step(&h, &st, 1 * S, 0, false));
    assert(fg_step(&h, &st, 2 * S, 0, true));                          /* a restart is next: at once, animating or not */
    assert(st.flushed == 1 && st.forced == 1 && st.capped == 0);
    assert(fg_step(&h, &st, 3 * S, 0, true) && st.held == 1 && st.forced == 2);   /* never held once forced */
}

/* A writer whose data lives in a ring (the log, the battery history): items arrive at random, a write takes all owed.
 * Over an hour of animations (taps, changes, a 15-minute stretch of busy screen) it never writes mid-animation except
 * at the cap, every write is after FG_CALM_US of calm (or the cap / the force), and after the restart's forced flush
 * nothing is owed. */
static void test_writer(void)
{
    enum { N = 400 };
    static span_t a[N];
    int n = 0;
    unsigned seed = 7;
    int64_t t = 2 * S;
    while (n < N - 1 && t < 3600 * S) {
        seed = seed * 1103515245u + 12345u;
        const int64_t len = (int64_t)(300 + (seed >> 8) % 3000) * MS, gap = (int64_t)(200 + (seed >> 4) % 9000) * MS;
        a[n++] = (span_t){ t, t + len };
        t += len + gap;
    }
    a[n++] = (span_t){ 1800 * S, 2700 * S };                          /* 15 min of a busy screen: the cap must act */
    fg_hold_t h = {0}; fg_stats_t st = {0};
    uint32_t owed = 0, arrived = 0, written = 0, mid = 0;
    for (int64_t now = 0; now < 3600 * S; now += 100 * MS) {           /* the writer looks every 100 ms */
        seed = seed * 1103515245u + 12345u;
        if ((seed >> 16) % 50 == 0) { owed++; arrived++; }
        if (!owed) continue;
        const int64_t calm = calm_at(a, n, now);
        if (!fg_step(&h, &st, now, calm, false)) continue;
        assert(calm >= FG_CALM_US || st.capped);
        if (calm == 0) mid++;
        written += owed; owed = 0;
    }
    assert(mid <= st.capped);                                          /* mid-animation only at the cap */
    assert(st.capped >= 1 && st.held >= 1 && st.flushed == st.held - (h.held ? 1 : 0));
    if (owed) assert(fg_step(&h, &st, 3600 * S, 0, true));             /* the restart: forced */
    written += owed;
    assert(written == arrived);
    printf("writer: %u items, %u held, %u flushed, %u at the cap, longest hold %u ms\n",
           arrived, st.held, st.flushed, st.capped, st.hold_max_ms);
}

static void test_batches(void)
{
    fg_batch_t b = {0}, d;
    /* three page writes of one task 10 ms apart, an erase: one batch */
    assert(!fg_batch_op(&b, &d, 100 * MS, "diag_persist", 256, 0, 0, 1000, false));
    assert(!fg_batch_op(&b, &d, 110 * MS, "diag_persist", 256, 0, 0, 1000, false));
    assert(!fg_batch_op(&b, &d, 170 * MS, "diag_persist", 0, 4096, 0, 45000, true));
    assert(!fg_batch_idle(&b, &d, 170 * MS + FG_BATCH_GAP_US));        /* not quiet long enough yet */
    assert(fg_batch_idle(&b, &d, 170 * MS + FG_BATCH_GAP_US + 1));
    assert(d.ops == 3 && d.wbytes == 512 && d.ebytes == 4096 && d.busy_us == 47000 && d.anim_ops == 1);
    assert(!strcmp(d.who, "diag_persist") && !d.mixed && d.first_us == 99 * MS && fg_batch_worth(&d));
    assert(!fg_batch_idle(&b, &d, 10 * S));                            /* nothing open */
    /* another task joins within the gap: one batch, mixed */
    fg_batch_op(&b, &d, 1 * S, "power", 256, 0, 0, 500, false);
    fg_batch_op(&b, &d, 1 * S + 50 * MS, "httpd", 32, 0, 0, 500, false);
    /* an op past the gap closes it first */
    assert(fg_batch_op(&b, &d, 2 * S, "power", 256, 0, 0, 500, false));
    assert(d.ops == 2 && d.mixed && !strcmp(d.who, "power") && d.wbytes == 288);
    assert(b.ops == 1 && !b.mixed && b.first_us == 2 * S - 500);
    /* reads alone: logged only when they held the cache off long enough */
    fg_batch_t r = {0};
    fg_batch_op(&r, &d, 5 * S, "httpd", 0, 0, 256, 100, true);
    assert(fg_batch_idle(&r, &d, 6 * S) && !fg_batch_worth(&d) && d.anim_ops == 0 && d.read_us == 100);
    for (int i = 0; i < 300; i++) fg_batch_op(&r, &d, 7 * S + i * MS, "httpd", 0, 0, 256, 100, false);
    assert(fg_batch_idle(&r, &d, 8 * S) && fg_batch_worth(&d) && d.read_us == 30000 && d.rbytes == 300 * 256);
}

int main(void)
{
    test_hold_and_go();
    test_cap();
    test_force();
    test_writer();
    test_batches();
    printf("FLASH GATE PASS: writes hold while animating, go when calm, at the cap and before a restart; none lost\n");
    return 0;
}
