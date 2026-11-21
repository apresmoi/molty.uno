/* flash_gate: the flash's quiet window (plain C, host-tested: test/host/flash_gate_test.c).
 *
 * While the SPI flash is written or erased the cache is off: code in flash cannot run on either core, PSRAM cannot be
 * reached and interrupts that are not IRAM-safe wait (a sector erase: tens of ms; a 12 KB SPIFFS file: ~0.4 s of both
 * cores, measured). The fluid's two tasks and their frames live there, so a write mid-animation freezes the
 * screen. A write that can wait (the log's copy, the battery history, the do-not-disturb switch) holds while the screen
 * animates and goes once it has been calm FG_CALM_US, or once held FG_HOLD_MAX_US, or at once when a restart or power
 * off is next (forced): it holds nothing in RAM of its own, its data stays where it lives (the log ring, the history
 * ring), so nothing is lost by waiting. Writes that cannot wait (an update, a package the human just installed, Wi-Fi
 * credentials, a setting the human just changed) are never held: they are the moment the human asked for.
 *
 * Every write and erase of the flash's partitions (and reads long enough to matter) is counted and logged, a batch a line
 * (ops of one task with less than FG_BATCH_GAP_US between them): who, bytes, how long, how many while animating.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define FG_CALM_US      (1500LL * 1000)          /* the screen calm this long: a held write goes      */
#define FG_HOLD_MAX_US  (600LL * 1000000)        /* ... or held this long already (still animating)    */
#define FG_BATCH_GAP_US (250LL * 1000)           /* ops further apart than this are two batches        */
#define FG_READ_LOG_US  (20 * 1000)              /* a batch of reads alone is logged from this long on */

typedef struct { bool held; int64_t since; } fg_hold_t;   /* a write waiting: since when */
typedef struct {
    uint32_t ops, anim_ops;            /* writes + erases; of them while the screen animated                     */
    uint32_t held, flushed, capped;    /* writes held; of them gone since; of those, gone at the cap (animating) */
    uint32_t forced;                   /* writes gone because a restart / power off was next                    */
    uint32_t hold_max_ms;              /* the longest hold                                                       */
} fg_stats_t;

/* One look of a writer with a write due: true = it goes now. calm_us: how long the screen has been calm (0: animating
 * now). force: a restart or power off is next. Counts into st. */
bool fg_step(fg_hold_t *h, fg_stats_t *st, int64_t now_us, int64_t calm_us, bool force);

typedef struct {
    char who[16]; bool mixed;          /* the first op's task; others joined                         */
    int64_t first_us, last_us;
    uint32_t ops, anim_ops, wbytes, ebytes, rbytes, busy_us, read_us;   /* (anim_ops, busy_us: the writes + erases) */
} fg_batch_t;
/* One op ended at now_us (dur_us long; w/e/r: bytes written, erased, read; anim: the screen animated at its start or
 * end). It joins the batch b, or b is closed into *done first (true) and the op starts the next. */
bool fg_batch_op(fg_batch_t *b, fg_batch_t *done, int64_t now_us, const char *who, uint32_t w, uint32_t e, uint32_t r,
                 uint32_t dur_us, bool anim);
/* b quiet FG_BATCH_GAP_US by now: closed into *done, true. */
bool fg_batch_idle(fg_batch_t *b, fg_batch_t *done, int64_t now_us);
/* A closed batch worth a log line: it wrote or erased, or its reads held the cache off FG_READ_LOG_US. */
bool fg_batch_worth(const fg_batch_t *b);
