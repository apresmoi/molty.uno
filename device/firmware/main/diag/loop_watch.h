/* loop_watch — is a shared dispatcher task still running its queue? (plain C, host-tested:
 * test/host/loop_watch_test.c).
 *
 * The default event loop ("sys_evt") and the esp_timer task each run every handler / timer
 * callback in turn on one task: one callback that blocks forever wedges every other one, and
 * nothing notices (the task is blocked, not busy, so the task watchdog stays quiet).
 * Seen 2026-11-16: mdns 1.11.1's mdns_unregister_netif took its lock twice, so the Wi-Fi
 * disconnect handler never returned; the mdns timer then blocked the esp_timer task on the same
 * lock. No reconnect (its timer never fired), the ping kept sending into the dead station, and
 * internal RAM drained ~36 B/s until the wifi task's next allocation failed and it panicked.
 *
 * mole_diag's 1 Hz sampler sends each task a no-op (an event, a periodic timer); the task stamps
 * a beat when it runs it. No beat for LOOP_WATCH_LIMIT_MS: wedged, abort on purpose, so the
 * coredump holds every task's backtrace (who blocks, on what) instead of a slow starvation's
 * unrelated victim. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LOOP_WATCH_LIMIT_MS 30000u

typedef struct {
    volatile uint32_t beat_ms;   /* the last time the watched task ran its no-op */
} loop_watch_t;

/* now_ms: when watching starts (a beat is due from then on) */
void loop_watch_init(loop_watch_t *w, uint32_t now_ms);
/* from the watched task: its no-op ran */
void loop_watch_beat(loop_watch_t *w, uint32_t now_ms);
/* from the sampler: ms without a beat when that reaches LOOP_WATCH_LIMIT_MS, else 0. A beat
 * stamped after the caller read its clock counts as fresh (uint32 ms wraps in 49 days). */
uint32_t loop_watch_stuck(const loop_watch_t *w, uint32_t now_ms);
