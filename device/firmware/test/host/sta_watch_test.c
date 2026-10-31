/* sta_watch (main/diag/sta_watch.h): rssi is sampled while the station is associated — also
 * when it associated before the diag registered its handlers — and skipped while it is down,
 * with one wifi.disconnected per outage. */
#include "sta_watch.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    sta_watch_t w;

    /* the regression: the boot connect delivered STA_CONNECTED before mole_diag_start;
     * seeded from network.c's state, the diag samples rssi with no event of its own */
    sta_watch_init(&w);
    assert(!sta_watch_up(&w));
    sta_watch_seed(&w, true);
    assert(sta_watch_up(&w));
    /* then an outage: quiet while down, logged once however often the reconnect fails */
    assert(sta_watch_event(&w, STA_WATCH_EV_DISCONNECTED));
    assert(!sta_watch_up(&w));
    for (int i = 0; i < 50; i++) {
        assert(!sta_watch_event(&w, STA_WATCH_EV_DISCONNECTED));
        assert(!sta_watch_up(&w));
    }
    /* back: sampled again; the next outage is news again */
    assert(!sta_watch_event(&w, STA_WATCH_EV_CONNECTED));
    assert(sta_watch_up(&w));
    assert(sta_watch_event(&w, STA_WATCH_EV_DISCONNECTED));
    assert(!sta_watch_event(&w, STA_WATCH_EV_DISCONNECTED));

    /* booted without Wi-Fi (no creds / AP away): down; the first failure is logged once */
    sta_watch_init(&w);
    sta_watch_seed(&w, false);
    assert(!sta_watch_up(&w));
    assert(sta_watch_event(&w, STA_WATCH_EV_DISCONNECTED));
    assert(!sta_watch_event(&w, STA_WATCH_EV_DISCONNECTED));
    assert(!sta_watch_event(&w, STA_WATCH_EV_CONNECTED));
    assert(sta_watch_up(&w));

    /* an event that raced in between registration and the seed is fresher: the seed loses */
    sta_watch_init(&w);
    sta_watch_event(&w, STA_WATCH_EV_DISCONNECTED);
    sta_watch_seed(&w, true);
    assert(!sta_watch_up(&w));
    sta_watch_init(&w);
    sta_watch_event(&w, STA_WATCH_EV_CONNECTED);
    sta_watch_seed(&w, false);
    assert(sta_watch_up(&w));

    printf("sta_watch: ok\n");
    return 0;
}
