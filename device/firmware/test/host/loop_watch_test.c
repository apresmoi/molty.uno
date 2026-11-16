/* loop_watch (main/diag/loop_watch.h): a dispatcher task (event loop, esp_timer) that stops
 * running its no-op for 30 s is wedged; a beat stamped after the sampler read its clock is
 * fresh; the ms clock wrapping is no wedge. */
#include "loop_watch.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    loop_watch_t w;
    /* boot: watching starts now, not at 0 (a sampler that starts at t=40 s must not abort) */
    loop_watch_init(&w, 40000);
    assert(loop_watch_stuck(&w, 40000) == 0);
    assert(loop_watch_stuck(&w, 69999) == 0);
    /* a healthy loop: a beat a second, never stuck however long it runs */
    for (uint32_t t = 41000; t < 41000 + 3600000; t += 1000) {
        loop_watch_beat(&w, t);
        assert(loop_watch_stuck(&w, t + 999) == 0);
    }
    /* the wedge (a handler that never returns): no beat from t0; 29.999 s fine, 30 s aborts */
    const uint32_t t0 = 5000000;
    loop_watch_beat(&w, t0);
    assert(loop_watch_stuck(&w, t0 + 29999) == 0);
    assert(loop_watch_stuck(&w, t0 + 30000) == 30000);
    assert(loop_watch_stuck(&w, t0 + 45000) == 45000);
    /* the beat raced the sampler (stamped after it read now): fresh, not 49 days stale */
    loop_watch_beat(&w, t0 + 50001);
    assert(loop_watch_stuck(&w, t0 + 50000) == 0);
    /* across the uint32 ms wrap (49.7 days of uptime) */
    loop_watch_beat(&w, 0xFFFFF000u);
    assert(loop_watch_stuck(&w, 0x00000100u) == 0);
    assert(loop_watch_stuck(&w, 0xFFFFF000u + 30000u) == 30000);
    puts("LOOP WATCH PASS: 30 s without a beat is a wedge, a beat a second never is, raced beats and the ms wrap are fresh");
    return 0;
}
