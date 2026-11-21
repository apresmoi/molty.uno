/*
 * fluid_tx.h: the drawer's side of the hand-over to the sender task (mole_fluid.c "Sender"), apart from FreeRTOS so the
 * host tests drive it. The drawer queues a frame's windows (n), then a frame end; the sender answers each frame end once
 * its windows are out (open: frame ends not answered yet).
 *
 * A wait on the sender is bounded. Once one runs out the sender is late: from then on no call waits on it (each only takes
 * the answers that came meanwhile), and nothing else may talk to the panel (the sender may still be in a transfer) until
 * it has answered every frame end; then it is back. So a sender that is slow once costs one bound, never one a frame.
 */
#pragma once

#include <stdbool.h>

typedef struct { int n, open; bool late; } fluid_tx_t;
typedef struct {
    bool (*end)(void *ctx, int wait_ms);   /* a frame end queued (false: no room within wait_ms)                     */
    bool (*done)(void *ctx, int wait_ms);  /* one frame end answered (false: none within wait_ms)                     */
    void *ctx;
} fluid_tx_io_t;

static inline bool fluid_tx_idle(const fluid_tx_t *t) { return !t->n && !t->open; }

/* Every window queued is out and the sender idle: true. False: it has not answered (within bound_ms the first time, at
 * once while it is late). *went_late: this call found it late (to log once an episode). */
static inline bool fluid_tx_drain(fluid_tx_t *t, const fluid_tx_io_t *io, int bound_ms, bool *went_late) {
    const int w = t->late ? 0 : bound_ms;
    if (t->n) {
        if (!io->end(io->ctx, w)) goto late;
        t->n = 0; t->open++;
    }
    for (; t->open; t->open--) if (!io->done(io->ctx, w)) goto late;
    t->late = false;
    return true;
late:
    if (!t->late && went_late) *went_late = true;
    t->late = true;
    return false;
}
