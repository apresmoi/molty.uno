/* heap_guard — internal-RAM self-protection (plain C, host-tested: test/host/heap_guard_test.c).
 *
 * Internal RAM is what Wi-Fi, lwIP, TLS and the LCD's DMA bounce buffers live on. Below ~12 KB
 * free they start failing (log.md 2026-10-25, and the 2026-10-31 hour that ended at 2 KB with
 * Wi-Fi gone and every LCD flush failing). mole_diag's sampler feeds this the internal free
 * size once a second; it answers with what to do:
 *   HG_LOW  — free stayed under HEAP_GUARD_LOW for HEAP_GUARD_LOW_S seconds: log one
 *             "heap.low" warning with the suspects (once per episode, not per second);
 *   HG_SHED — with the first warning of the boot: give back the discretionary internal RAM
 *             (the fluid's hot fields move to PSRAM: slower frames, never a starved radio);
 *   HG_OK   — the episode is over: free stayed above HEAP_GUARD_OK for HEAP_GUARD_OK_S seconds.
 * A listen's dip (a 6.4 KB mic DMA chunk, a TLS handshake: seconds) never counts: the
 * condition must hold for whole seconds in a row. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define HEAP_GUARD_LOW   (12u * 1024)
#define HEAP_GUARD_LOW_S 3u
#define HEAP_GUARD_OK    (20u * 1024)
#define HEAP_GUARD_OK_S  10u

enum { HG_NONE = 0, HG_LOW = 1, HG_SHED = 2, HG_OK = 4 };

typedef struct {
    uint32_t below, above;   /* seconds in a row under LOW / over OK */
    bool low;                /* in a low episode (warned) */
    bool shed;               /* the shed already happened this boot */
    uint32_t episodes;
} heap_guard_t;

void heap_guard_init(heap_guard_t *g);
/* once a second: the internal free size now → HG_* flags */
int heap_guard_step(heap_guard_t *g, uint32_t free_now);
