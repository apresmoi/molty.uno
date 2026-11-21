/*
 * SPDX-License-Identifier: MIT
 *
 * lcd_q.h: the CO5300 driver's transfers (lcd_co5300.c), apart from the IDF so the host tests drive them
 * (test/host/lcd_q_test.c).
 *
 * Every transfer is a queued SPI transaction on a ring of LQ_DEPTH slots; their ends are taken back in order, each
 * wait bounded (LQ_WAIT_MS). Nothing here waits for ever: no polling transaction, and the bus is held (for the chip
 * select across a window's chunks) only once every earlier transfer has ended, so taking it never waits on one.
 * Pixels from memory the DMA cannot read (PSRAM) go through bounce buffers (lq_bounce: LQ_BUFS of LQ_BUF bytes, given
 * once at init): the SPI master never allocates. A window's source may be reused once the call returns when it was bounced;
 * a DMA-capable one is read until the next call (lq_wait or the next transfer). A bounced window's pixels off the round
 * glass (lq_disc_t) go out black and are not read: the copy out of PSRAM is what a whole glass's send waits on while the
 * other core is busy in PSRAM too (bench 25: 12.5 ms quiet, up to 18), and the corners are a fifth of it.
 *
 * A wait that runs out, a slot back out of order, or a call the IDF refuses: logged by the caller (lq_t.fails), the
 * bus let go, whatever ended taken, and when a transfer is still counted the device is removed and added again (reset):
 * the ring is clean and the next call goes on. A frame cut after a chunk that kept the chip select low is closed with
 * the panel's no-op command (lq_init's close): its bytes end the cut pixel stream, its end raises the chip select. A reset the IDF refuses (a transfer still in its queue) leaves it stuck:
 * each later call tries again at once, without waiting, and fails meanwhile (the caller repaints when it is back).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define LQ_BUFS    2        /* bounce buffers (one filled while the other goes out) ..                      */
#define LQ_BUF     4096     /* .. of this many bytes: LQ_BUFS x LQ_BUF internal DMA RAM, held from init (bench 17:
                             * 2 x 2048 sent a change's glass ~25 % slower, 3 x 4096 and 2 x 8192 ~1 ms faster)  */
#define LQ_DEPTH   (LQ_BUFS + 2)   /* transactions in flight at most: a window's two commands, its chunks       */
#define LQ_XFER    8192     /* a chunk at most (the bus's max_transfer_sz), straight from DMA-capable memory  */
#define LQ_WAIT_MS 200      /* a transfer's end, at most (a 4 KB chunk takes 0.1 ms; a flash erase holds the
                             * SPI interrupt off for tens of ms)                                              */

#define LQ_E_TIMEOUT 0x107  /* ESP_ERR_TIMEOUT       */
#define LQ_E_STATE   0x103  /* ESP_ERR_INVALID_STATE */

enum { LQ_CMD = 1, LQ_QUAD = 2, LQ_KEEP = 4 };   /* lq_tx_t.flags: cmd + address phases; data on 4 lines; CS held after */
/* where a call is (lq_t.at, the stuck sender's log line): idle, waiting for every earlier end, for a free slot, for a
 * bounce buffer, taking the bus, queueing */
enum { LQ_AT_IDLE, LQ_AT_DRAIN, LQ_AT_SLOT, LQ_AT_BUF, LQ_AT_HOLD, LQ_AT_QUEUE };

typedef struct {
    const uint8_t *data;    /* NULL: d (len <= 4, copied at queueing)        */
    uint32_t len, addr;     /* data bytes; the address phase (24 bits)       */
    uint8_t  cmd, flags;    /* the command phase (8 bits); LQ_*              */
    uint8_t  d[4];
} lq_tx_t;

typedef struct {
    int  (*queue)(void *ctx, int slot, const lq_tx_t *t);    /* 0, or the IDF's error                                 */
    int  (*result)(void *ctx, int *slot, int wait_ms);       /* the oldest end (*slot: its slot), 0; else the error   */
    int  (*hold)(void *ctx, bool on);                        /* the bus taken (CS kept across chunks) or let go       */
    int  (*reset)(void *ctx);                                /* the device removed and added again: 0, else the error */
    bool (*dma_ok)(void *ctx, const void *p);                /* the DMA reads p as is                                 */
    void *ctx;
} lq_io_t;

/* a window's place on a round glass of size x size px (lq_window): its pixels past the glass's rim by more than
 * LQ_DISC_PAD px are sent black */
typedef struct { int x, y, w, size; } lq_disc_t;
#define LQ_DISC_PAD 3

typedef struct {
    const lq_io_t *io;
    uint8_t  *buf[LQ_BUFS];      /* nbuf of bsz bytes; none: the IDF bounces each chunk itself (as esp_lcd did)    */
    int      nbuf; uint32_t bsz;
    uint32_t use[LQ_BUFS];       /* the transaction that last read each buffer (+1): free once tail reaches it    */
    uint32_t head, tail;             /* transactions queued, ends taken (since boot)                                  */
    int      nb;                     /* the next bounce buffer                                                        */
    bool     held, stuck, open;      /* the bus taken; a reset refused; a chunk kept CS low and its frame is not closed  */
    lq_tx_t  close;                  /* the panel's no-op command: closes a cut frame                                  */
    volatile uint8_t at;             /* LQ_AT_*                                                                       */
    uint32_t fails, resets, dropped; /* failures; resets made; transactions counted that never ended                  */
    int      err; uint8_t err_at;    /* the last failure: its error, where                                            */
    bool     inject;                 /* EXPERIMENT (bench stuck=1): the next call counts one transfer that never ends  */
} lq_t;

void lq_init(lq_t *q, const lq_io_t *io, const lq_tx_t *close);
/** The bounce buffers: n (<= LQ_BUFS) of size (<= LQ_XFER) bytes at mem, DMA-capable; NULL or 0: none. Only with
 *  every transfer out (lq_wait first); the old ones are free once it returns. */
void lq_bounce(lq_t *q, uint8_t *mem, int n, uint32_t size);
/** A short command (data <= 4 bytes), out when it returns. */
int  lq_reg(lq_t *q, uint8_t cmd, uint32_t addr, const uint8_t *data, uint32_t len);
/** The commands pre (short, in order), then n bytes of px as one chip-select frame (cmd and addr on its first chunk),
 *  queued: it returns while the last chunks still go out. disc (or NULL): px are the window's 2-byte pixels, and those
 *  off the round glass go out black, unread, when bounced. */
int  lq_window(lq_t *q, const lq_tx_t *pre, int npre, uint8_t cmd, uint32_t addr, const uint8_t *px, uint32_t n, const lq_disc_t *disc);
/** Row y of a round glass of size px: the columns it shows, [*a, *b) (LQ_DISC_PAD past its rim; none: *a == *b). */
void lq_disc_row(int size, int y, int *a, int *b);
/** Every transfer out (a stuck ring tried again first). */
int  lq_wait(lq_t *q);
