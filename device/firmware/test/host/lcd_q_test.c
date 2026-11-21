/*
 * lcd_q_test.c: the CO5300 driver's transfers (boards/waveshare/esp32-s3-touch-amoled-1.75c/lcd_q.c) against a fake
 * SPI device: what reaches the wire (chip-select frames, their cmd/addr and bytes), that a bounce buffer or a DMA source
 * is never changed while a transfer reads it, that the bus is taken only with nothing in flight, that every wait is
 * bounded, and the recoveries: a lost end, a transfer that never ends, a queue call refused; a window on the round
 * glass: what it shows sent as it is, its corners black.
 */
#include "lcd_q.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

/* ── the fake device: a FIFO the "ISR" completes in order when a result is asked for ── */
#define WIRE_MAX 460000
static struct {
    lq_tx_t q[64]; uint32_t sum[64]; int slot[64];   /* queued, not yet ended (in order) */
    int nq;
    int ret[64], nret;                                /* ended, results not taken */
    bool held, stall, lose_next; int fail_queue_at, queues;
    int resets, waits_blocking, bad, closes;
    bool in_frame;                                    /* CS low (a frame open: the last chunk kept it) */
    uint8_t wire[WIRE_MAX]; int nwire; int frames; uint8_t frame_cmd[64]; uint32_t frame_addr[64]; int frame_start[64];
} F;
static uint8_t g_dma[96 * 1024];                      /* "internal DMA" memory: the DMA reads it as is */
static uint8_t g_ps[440 * 1024];                      /* "PSRAM": bounced */

static uint32_t sum(const lq_tx_t *t)
{
    const uint8_t *p = t->data ? t->data : t->d;
    uint32_t s = 2166136261u;
    for (uint32_t i = 0; i < t->len; i++) s = (s ^ p[i]) * 16777619u;
    return s;
}
static void complete_one(void)
{
    const lq_tx_t *t = &F.q[0];
    CHECK(sum(t) == F.sum[0], "a transfer's data changed while it was in flight");
    if (F.in_frame && (t->flags & LQ_CMD) && t->addr == 0 && !t->len) { F.closes++; F.in_frame = false; goto ended; }   /* (the NOP closing a cut frame) */
    if (!F.in_frame) {
        CHECK(t->flags & LQ_CMD, "a frame opens without its command");
        if (F.frames < 64) { F.frame_cmd[F.frames] = t->cmd; F.frame_addr[F.frames] = t->addr; F.frame_start[F.frames] = F.nwire; }
        F.frames++;
    } else CHECK(!(t->flags & LQ_CMD), "a command inside a held frame");
    const uint8_t *p = t->data ? t->data : t->d;
    if (F.nwire + (int)t->len <= WIRE_MAX) { memcpy(F.wire + F.nwire, p, t->len); F.nwire += (int)t->len; }
    F.in_frame = (t->flags & LQ_KEEP) != 0;
ended:
    if (!F.lose_next) F.ret[F.nret++] = F.slot[0]; else F.lose_next = false;
    memmove(&F.q[0], &F.q[1], sizeof(F.q[0]) * (size_t)(F.nq - 1)); memmove(&F.sum[0], &F.sum[1], sizeof(F.sum[0]) * (size_t)(F.nq - 1));
    memmove(&F.slot[0], &F.slot[1], sizeof(F.slot[0]) * (size_t)(F.nq - 1));
    F.nq--;
}
static int f_queue(void *ctx, int slot, const lq_tx_t *t)
{
    (void)ctx;
    F.queues++;
    if (F.fail_queue_at && F.queues == F.fail_queue_at) return 0x101;   /* (ESP_ERR_NO_MEM) */
    CHECK(F.nq + F.nret < LQ_DEPTH, "more than the device's queue holds");
    for (int i = 0; i < F.nq; i++) CHECK(F.slot[i] != slot, "a slot reused while queued");
    for (int i = 0; i < F.nret; i++) CHECK(F.ret[i] != slot, "a slot reused before its end was taken");
    CHECK(!(t->flags & LQ_KEEP) || F.held, "CS kept without the bus held");
    CHECK(t->len <= LQ_XFER, "a chunk over the bus's max");
    if (t->data && !(t->data >= g_dma && t->data < g_dma + sizeof g_dma)) F.bad++;   /* (a non-DMA source handed to the IDF) */
    F.q[F.nq] = *t; F.sum[F.nq] = sum(t); F.slot[F.nq] = slot; F.nq++;
    return 0;
}
static int f_result(void *ctx, int *slot, int wait_ms)
{
    (void)ctx;
    CHECK(wait_ms >= 0 && wait_ms <= LQ_WAIT_MS, "an unbounded wait");
    if (!F.nret && F.nq && !F.stall) complete_one();
    if (!F.nret) { if (wait_ms) F.waits_blocking++; return LQ_E_TIMEOUT; }
    *slot = F.ret[0];
    memmove(&F.ret[0], &F.ret[1], sizeof(F.ret[0]) * (size_t)(F.nret - 1)); F.nret--;
    return 0;
}
static int f_hold(void *ctx, bool on)
{
    (void)ctx;
    if (on) CHECK(!F.nq, "the bus taken with a transfer in flight (the IDF would wait for ever on it)");
    F.held = on;
    return 0;
}
static int f_reset(void *ctx)
{
    (void)ctx;
    if (F.nq) return LQ_E_STATE;                       /* (the IDF refuses: a transfer still queued) */
    F.nret = 0; F.resets++;                            /* (CS as it was: a cut frame stays open till closed) */
    return 0;
}
static bool f_dma(void *ctx, const void *p) { (void)ctx; return (const uint8_t *)p >= g_dma && (const uint8_t *)p < g_dma + sizeof g_dma; }
static const lq_io_t IO = { f_queue, f_result, f_hold, f_reset, f_dma, NULL };

static uint8_t *const g_bounce = g_dma + 48 * 1024;   /* (the bounce buffers: internal DMA RAM too) */
static const lq_tx_t NOP = { NULL, 0, 0, 0x02, LQ_CMD, { 0 } };
static lq_t Q;
static void fresh_with(int n, uint32_t size)
{
    memset(&F, 0, sizeof F);
    lq_init(&Q, &IO, &NOP);
    lq_bounce(&Q, g_bounce, n, size);
}
static void fresh(void) { fresh_with(LQ_BUFS, LQ_BUF); }
static void fill(uint8_t *p, int n, int seed) { for (int i = 0; i < n; i++) p[i] = (uint8_t)(i * 7 + seed * 13 + (i >> 8)); }
static const lq_tx_t PRE[2] = { { NULL, 4, 0x2A00, 0x02, LQ_CMD, { 1, 2, 3, 4 } }, { NULL, 4, 0x2B00, 0x02, LQ_CMD, { 5, 6, 7, 8 } } };
static int window(const uint8_t *px, uint32_t n) { return lq_window(&Q, PRE, 2, 0x32, 0x2C00, px, n, NULL); }
/* the last window on the wire: its three frames (CASET, RASET, RAMWR + px) */
static void check_window(const uint8_t *want, uint32_t n, const char *what)
{
    CHECK(F.frames >= 3, "%s: %d frames", what, F.frames);
    const int f = F.frames - 3;
    CHECK(F.frame_cmd[f] == 0x02 && F.frame_addr[f] == 0x2A00 && F.frame_cmd[f + 2] == 0x32 && F.frame_addr[f + 2] == 0x2C00, "%s: commands", what);
    CHECK(F.nwire - F.frame_start[f + 2] == (int)n && !memcmp(F.wire + F.frame_start[f + 2], want, n), "%s: the pixels on the wire", what);
    CHECK(!F.in_frame && !F.held && !Q.held, "%s: the frame closed, the bus let go", what);
}

static void test_bounced(int nb, uint32_t bs)
{
    fresh_with(nb, bs);
    static uint8_t want[10000];
    fill(g_ps, 10000, 1); memcpy(want, g_ps, 10000);
    CHECK(window(g_ps, 10000) == 0, "bounced window");
    fill(g_ps, 10000, 2);                               /* (its source reused at once: it was bounced) */
    CHECK(lq_wait(&Q) == 0, "wait");
    check_window(want, 10000, "bounced");
    CHECK(F.bad == 0, "a PSRAM source reached the IDF");
    CHECK(Q.head == Q.tail && Q.head == 2 + (10000 + bs - 1) / bs, "2 commands + chunks of %u: %u", (unsigned)bs, (unsigned)Q.head);
    /* a small one: one chunk, the bus never held */
    fill(g_ps, 300, 3); memcpy(want, g_ps, 300);
    CHECK(window(g_ps, 300) == 0 && lq_wait(&Q) == 0, "small window");
    check_window(want, 300, "small");
    /* a full frame, back to back with another */
    fill(g_ps, 466 * 466 * 2, 4);
    static uint8_t full[466 * 466 * 2]; memcpy(full, g_ps, sizeof full);
    F.frames = 0; F.nwire = 0;
    CHECK(window(g_ps, sizeof full) == 0 && lq_wait(&Q) == 0, "full frame");
    check_window(full, sizeof full, "full");
}

static void test_direct(void)
{
    fresh();
    fill(g_dma, 9000, 5);
    static uint8_t want[9000]; memcpy(want, g_dma, 9000);
    CHECK(window(g_dma, 9000) == 0, "direct window");
    CHECK(lq_wait(&Q) == 0, "wait");                    /* (a DMA source is read until the next call) */
    check_window(want, 9000, "direct");
    CHECK(Q.head == 2 + (9000 + LQ_XFER - 1) / LQ_XFER, "2 commands + chunks of %d: %u", LQ_XFER, (unsigned)Q.head);
    /* two windows in a row: the second's begin takes the first's ends before its own commands */
    fill(g_dma + 9000, 4000, 6);
    CHECK(window(g_dma, 9000) == 0 && window(g_dma + 9000, 4000) == 0 && lq_wait(&Q) == 0, "back to back");
    check_window(g_dma + 9000, 4000, "second");
}

static void test_reg(void)
{
    fresh();
    const uint8_t b = 0xFF;
    CHECK(lq_reg(&Q, 0x02, 0x5100, &b, 1) == 0 && Q.head == Q.tail && !F.nq, "a register write is out when it returns");
    CHECK(F.frames == 1 && F.nwire == 1 && F.wire[0] == 0xFF && F.frame_addr[0] == 0x5100, "its frame");
    CHECK(lq_reg(&Q, 0x02, 0x1100, NULL, 0) == 0 && F.frames == 2, "no data");
    CHECK(lq_reg(&Q, 0x02, 0x1100, &b, 5) != 0, "more than 4 bytes refused");
}

static void test_lost(void)
{
    fresh();
    fill(g_ps, 10000, 7);
    F.lose_next = true;                                 /* (the first CASET's end never comes back) */
    const int e = window(g_ps, 10000);
    CHECK(e == LQ_E_TIMEOUT, "a lost end times out: %d", e);
    CHECK(Q.fails == 1 && Q.resets == 1 && Q.dropped == 1 && Q.head == Q.tail && !Q.stuck && !Q.held && !F.held && !F.in_frame && F.closes == 1, "reset clean: fails %u resets %u dropped %u",
          (unsigned)Q.fails, (unsigned)Q.resets, (unsigned)Q.dropped);
    static uint8_t want[10000]; memcpy(want, g_ps, 10000);
    F.frames = 0; F.nwire = 0;
    CHECK(window(g_ps, 10000) == 0 && lq_wait(&Q) == 0, "back after the reset");
    check_window(want, 10000, "after lost");
}

static void test_stall(void)
{
    fresh();
    fill(g_ps, 20000, 8);
    F.stall = true;                                     /* (nothing ends: the device's interrupt gone) */
    int e = window(g_ps, 20000);
    CHECK(e != 0 && Q.stuck && !F.held, "a transfer that never ends: stuck, the bus let go (%d)", e);
    const int w0 = F.waits_blocking;
    for (int i = 0; i < 5; i++) CHECK(window(g_ps, 20000) == LQ_E_STATE && lq_wait(&Q) == LQ_E_STATE, "stuck: refused at once");
    CHECK(F.waits_blocking == w0, "stuck: no call waits (%d waits)", F.waits_blocking - w0);
    F.stall = false;                                    /* (its transfers end after all) */
    static uint8_t want[20000]; memcpy(want, g_ps, 20000);
    F.frames = 0; F.nwire = 0;
    CHECK(window(g_ps, 20000) == 0 && lq_wait(&Q) == 0 && !Q.stuck && F.closes == 1, "back once they ended, the cut frame closed (%d)", F.closes);
    check_window(want, 20000, "after stall");
}

static void test_queue_fail(void)
{
    fresh();
    fill(g_ps, 20000, 9);
    F.fail_queue_at = 5;                                /* (the third chunk refused: no memory) */
    CHECK(window(g_ps, 20000) == 0x101, "a refused queue call is returned");
    CHECK(Q.head == Q.tail && !Q.held && !F.held && !Q.stuck && Q.resets == 0 && !F.nq && F.closes == 1 && !F.in_frame,
          "clean after it, no reset needed, the cut frame closed");
    F.fail_queue_at = 0;
    static uint8_t want[20000]; memcpy(want, g_ps, 20000);
    F.frames = 0; F.nwire = 0;
    CHECK(window(g_ps, 20000) == 0 && lq_wait(&Q) == 0, "the next window");
    check_window(want, 20000, "after a refused queue");
}

static void test_no_bounce(void)
{
    memset(&F, 0, sizeof F);
    lq_init(&Q, &IO, &NOP);                             /* (no bounce buffers: the IDF bounces, as before) */
    fill(g_ps, 9000, 11);
    CHECK(window(g_ps, 9000) == 0 && lq_wait(&Q) == 0, "without bounce buffers");
    check_window(g_ps, 9000, "no bounce");
    CHECK(F.bad > 0, "its PSRAM chunks go to the IDF as they are");
}

/* a window on the round glass (466 px): every pixel it shows on the wire as its source, every one past the rim by more
 * than LQ_DISC_PAD black; a whole glass and a box off its middle, bounced; a DMA source as it is */
static void test_disc(void)
{
    enum { S = 466 };
    static uint8_t want[S * S * 2];
    int rim = 0;
    for (int y = 0; y < S; y++) {                       /* (the rows' spans against the circle itself) */
        int a, b;
        lq_disc_row(S, y, &a, &b);
        for (int x = 0; x < S; x++) {
            const float d = hypotf(x + .5f - S / 2.f, y + .5f - S / 2.f);
            const bool in = x >= a && x < b;
            rim += (d <= S / 2.f && !in) || (d > S / 2.f + LQ_DISC_PAD + 1.f && in);
        }
    }
    CHECK(rim == 0, "disc: rows' spans hold the glass and end within %d px past its rim (%d px off)", LQ_DISC_PAD + 1, rim);
    static const struct { int x, y, w, h; } WIN[] = { { 0, 0, S, S }, { 8, 2, 120, 60 }, { 300, 180, 166, 100 }, { 0, 400, S, 66 } };
    for (int q = 0; q < 4; q++) {
        const int x0 = WIN[q].x, y0 = WIN[q].y, w = WIN[q].w, h = WIN[q].h;
        const uint32_t n = (uint32_t)(w * h * 2);
        fresh();
        fill(g_ps, (int)n, 20 + q);
        int black = 0;
        for (int y = 0; y < h; y++) {
            int a, b;
            lq_disc_row(S, y0 + y, &a, &b);
            for (int x = 0; x < w; x++) {
                const size_t o = (size_t)(y * w + x) * 2;
                const bool in = x0 + x >= a && x0 + x < b;
                want[o] = in ? g_ps[o] : 0; want[o + 1] = in ? g_ps[o + 1] : 0;
                black += !in;
            }
        }
        const lq_disc_t g = { x0, y0, w, S };
        CHECK(lq_window(&Q, PRE, 2, 0x32, 0x2C00, g_ps, n, &g) == 0 && lq_wait(&Q) == 0, "disc window %d", q);
        check_window(want, n, "disc");
        if (!q) CHECK(black > S * S / 6 && black < S * S / 4, "disc: a whole glass sends its corners black (%d px)", black);
    }
    fresh();                                            /* (a DMA source: the IDF reads it, as it is) */
    fill(g_dma, 9320, 30); memcpy(want, g_dma, 9320);
    const lq_disc_t g = { 0, 0, S, S };
    CHECK(lq_window(&Q, PRE, 2, 0x32, 0x2C00, g_dma, 9320, &g) == 0 && lq_wait(&Q) == 0, "disc, a DMA source");
    check_window(want, 9320, "disc direct");
}

int main(void)
{
    test_bounced(LQ_BUFS, LQ_BUF); test_bounced(2, 2048); test_bounced(1, 8192);
    test_direct(); test_reg(); test_lost(); test_stall(); test_queue_fail(); test_no_bounce(); test_disc();
    if (g_fail) { printf("lcd_q: %d FAILED\n", g_fail); return 1; }
    printf("LCD_Q PASS: frames on the wire, sources untouched in flight, bus taken idle, waits bounded, recoveries clean, the round glass's corners black\n");
    return 0;
}
