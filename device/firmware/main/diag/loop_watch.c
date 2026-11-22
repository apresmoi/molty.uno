/* loop_watch — see loop_watch.h. */
#include "loop_watch.h"

void loop_watch_init(loop_watch_t *w, uint32_t now_ms) { w->beat_ms = now_ms; }

void loop_watch_beat(loop_watch_t *w, uint32_t now_ms) { w->beat_ms = now_ms; }

uint32_t loop_watch_stuck(const loop_watch_t *w, uint32_t now_ms)
{
    const int32_t since = (int32_t)(now_ms - w->beat_ms);
    return since >= (int32_t)LOOP_WATCH_LIMIT_MS ? (uint32_t)since : 0;
}
