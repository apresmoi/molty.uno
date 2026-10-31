/* wifi_retry (main/net/wifi_retry.h): a station that drops is reconnected — IDF never does it
 * by itself — with a capped back-off, never during a join nor after a forget. */
#include "wifi_retry.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    wifi_retry_t r; wifi_retry_init(&r);
    /* never asked to connect (no saved credentials): a disconnect is not retried */
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_DISCONNECTED, false) == -1);
    /* boot connect with saved credentials, then a drop: 1 s, 2, 4, 8, 16, 30, 30 … forever */
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_CONNECT, false) == -1);
    const int32_t want[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000};
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++)
        assert(wifi_retry_event(&r, WIFI_RETRY_EV_DISCONNECTED, false) == want[i]);
    for (int i = 0; i < 1000; i++) assert(wifi_retry_event(&r, WIFI_RETRY_EV_DISCONNECTED, false) == 30000);
    /* back on the AP: the next drop starts at 1 s again */
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_GOT_IP, false) == -1);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_DISCONNECTED, false) == 1000);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_GOT_IP, false) == -1);
    /* a join (set_creds / Improv) owns the radio: its own disconnects are not retried */
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_DISCONNECTED, true) == -1);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_DISCONNECTED, false) == 1000);   /* the back-off did not move */
    /* forget: no credentials, never again — until a join connects with new ones */
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_FORGET, false) == -1);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_DISCONNECTED, false) == -1);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_GOT_IP, false) == -1);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_DISCONNECTED, false) == 1000);
    /* first set_creds on a fresh device: the join's success (got IP) arms it */
    wifi_retry_init(&r);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_GOT_IP, true) == -1);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_DISCONNECTED, false) == 1000);
    puts("WIFI RETRY PASS: a dropped station reconnects (1 s doubling to 30 s, forever), got-IP resets, never during a join, never after a forget");
    return 0;
}
