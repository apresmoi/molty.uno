/*
 * SPDX-License-Identifier: MIT
 *
 * lcd_q.c: the CO5300 driver's transfers (lcd_q.h). Plain C, no IDF.
 */
#include "lcd_q.h"

#include <string.h>

void lq_init(lq_t *q, const lq_io_t *io, uint8_t *const bufs[LQ_BUFS], const lq_tx_t *close)
{
    memset(q, 0, sizeof(*q));
    q->io = io; q->close = *close;
    for (int b = 0; b < LQ_BUFS; b++) q->buf[b] = bufs ? bufs[b] : NULL;
}

static int put(lq_t *q, const lq_tx_t *t);
static int take(lq_t *q, uint32_t upto, uint8_t at);

/* back to clean: the bus let go, whatever ended taken; a transfer still counted (lost, or never ended): the device
 * reset (refused: stuck, tried again by the next call); a cut frame closed */
static bool recover(lq_t *q)
{
    const lq_io_t *io = q->io;
    if (q->held) { io->hold(io->ctx, false); q->held = false; }
    int slot;
    while (q->tail != q->head && io->result(io->ctx, &slot, 0) == 0) q->tail++;
    if (q->tail != q->head) {
        if (io->reset(io->ctx) != 0) { q->stuck = true; return false; }
        q->resets++; q->dropped += q->head - q->tail; q->tail = q->head;
    }
    for (int b = 0; b < LQ_BUFS; b++) q->use[b] = 0;
    q->stuck = false; q->at = LQ_AT_IDLE;
    if (q->open) { q->open = false; if (put(q, &q->close) || take(q, q->head, LQ_AT_DRAIN)) return !q->stuck; }   /* (its own failure recovers again, once) */
    return true;
}

static int fail(lq_t *q, int err)
{
    q->fails++; q->err = err; q->err_at = q->at;
    recover(q);
    return err;
}

/* the ends of the transactions before upto (bounded waits) */
static int take(lq_t *q, uint32_t upto, uint8_t at)
{
    const lq_io_t *io = q->io;
    while ((int32_t)(upto - q->tail) > 0) {
        int slot = -1;
        q->at = at;
        const int e = io->result(io->ctx, &slot, LQ_WAIT_MS);
        if (e) return fail(q, e);
        const bool order = slot == (int)(q->tail % LQ_DEPTH);
        q->tail++;
        if (!order) return fail(q, LQ_E_STATE);
    }
    q->at = LQ_AT_IDLE;
    return 0;
}

static int put(lq_t *q, const lq_tx_t *t)
{
    int e = take(q, q->head + 1 - LQ_DEPTH, LQ_AT_SLOT);   /* (a free slot) */
    if (e) return e;
    q->at = LQ_AT_QUEUE;
    if ((e = q->io->queue(q->io->ctx, (int)(q->head % LQ_DEPTH), t))) return fail(q, e);
    q->head++;
    q->open = (t->flags & LQ_KEEP) != 0;
    q->at = LQ_AT_IDLE;
    return 0;
}

/* every earlier transfer out: the bus is free (taking it never waits) and so is the last source */
static int begin(lq_t *q)
{
    if (q->stuck && !recover(q)) return LQ_E_STATE;
    if (q->inject && q->head == q->tail) { q->inject = false; q->head++; }   /* EXPERIMENT: a transfer that never ends */
    return take(q, q->head, LQ_AT_DRAIN);
}

int lq_wait(lq_t *q) { return begin(q); }

int lq_reg(lq_t *q, uint8_t cmd, uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (len > 4) return LQ_E_STATE;
    int e = begin(q);
    if (e) return e;
    lq_tx_t t = { NULL, len, addr, cmd, LQ_CMD, { 0 } };
    if (len) memcpy(t.d, data, len);
    if ((e = put(q, &t))) return e;
    return take(q, q->head, LQ_AT_DRAIN);
}

int lq_window(lq_t *q, const lq_tx_t *pre, int npre, uint8_t cmd, uint32_t addr, const uint8_t *px, uint32_t n)
{
    const lq_io_t *io = q->io;
    int e = begin(q);
    if (e) return e;
    const bool bounce = q->buf[0] && !io->dma_ok(io->ctx, px);
    const uint32_t step = bounce ? LQ_BUF : LQ_XFER;
    if (n > step) {                                     /* (more than one chunk: the chip select held between them) */
        q->at = LQ_AT_HOLD;
        if ((e = io->hold(io->ctx, true))) return fail(q, e);
        q->held = true;
    }
    for (int i = 0; i < npre; i++) if ((e = put(q, &pre[i]))) return e;
    for (uint32_t off = 0; off < n;) {
        const uint32_t k = n - off < step ? n - off : step;
        const uint8_t *src = px + off;
        const int b = q->nb;
        if (bounce) {
            if ((e = take(q, q->use[b], LQ_AT_BUF))) return e;
            memcpy(q->buf[b], src, k);
            src = q->buf[b];
        }
        const lq_tx_t t = { src, k, off ? 0 : addr, off ? 0 : cmd,
                            (uint8_t)((off ? 0 : LQ_CMD) | LQ_QUAD | (off + k < n ? LQ_KEEP : 0)), { 0 } };
        if ((e = put(q, &t))) return e;
        if (bounce) { q->use[b] = q->head; q->nb = (b + 1) % LQ_BUFS; }
        off += k;
    }
    if (q->held) { io->hold(io->ctx, false); q->held = false; }
    return 0;
}
