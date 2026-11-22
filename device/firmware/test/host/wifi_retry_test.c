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
    /* a failed join (wrong password, an AP that refuses): its own drops are not retried, but the
     * join's end is: back to the saved network on the back-off, which keeps growing */
    wifi_retry_init(&r);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_CONNECT, false) == -1);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_DISCONNECTED, true) == -1);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_JOIN_FAILED, true) == 1000);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_DISCONNECTED, false) == 2000);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_JOIN_FAILED, false) == 4000);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_GOT_IP, false) == -1);
    /* nothing saved (a fresh device, or after a forget): a failed join leaves it in setup */
    wifi_retry_init(&r);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_JOIN_FAILED, true) == -1);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_GOT_IP, false) == -1);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_FORGET, false) == -1);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_JOIN_FAILED, false) == -1);
    /* first set_creds on a fresh device: the join's success (got IP) arms it */
    wifi_retry_init(&r);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_GOT_IP, true) == -1);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_DISCONNECTED, false) == 1000);

    /* ── the lease ladder: joined, no address ── */
    uint32_t w = 0;
    wifi_retry_init(&r);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_CONNECT, false) == -1);
    assert(wifi_retry_lease_due(&r, false, false, &w) == WIFI_LEASE_NONE);       /* not associated yet */
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_ASSOCIATED, false) == 10000);
    /* 1. the first wait ran out: restart DHCP, keep the AP, 8 s more */
    assert(wifi_retry_lease_due(&r, false, false, &w) == WIFI_LEASE_DHCP && w == 8000);
    /* 2. still none: leave and join again; the disconnect is one step of the reconnect back-off */
    assert(wifi_retry_lease_due(&r, false, false, &w) == WIFI_LEASE_REJOIN);
    assert(wifi_retry_lease_due(&r, false, false, &w) == WIFI_LEASE_NONE);       /* a late timer: nothing */
    assert(wifi_retry_lease_due(&r, false, true, &w) == WIFI_LEASE_NONE);        /* nor the IP-lost notice */
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_DISCONNECTED, false) == 1000);
    /* 3. the next association waits twice as long, no DHCP restart, then avoids that AP */
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_ASSOCIATED, false) == 20000);
    assert(wifi_retry_lease_due(&r, false, false, &w) == WIFI_LEASE_AVOID);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_DISCONNECTED, false) == 2000);
    /* the wait doubles and is capped, the back-off keeps its own pace: one count each per round */
    const int32_t lease_want[] = {30000, 30000, 30000, 30000, 30000};
    const int32_t back_want[]  = {4000, 8000, 16000, 30000, 30000};
    for (unsigned i = 0; i < sizeof(lease_want) / sizeof(lease_want[0]); i++) {
        assert(wifi_retry_event(&r, WIFI_RETRY_EV_ASSOCIATED, false) == lease_want[i]);
        assert(wifi_retry_lease_due(&r, false, false, &w) == WIFI_LEASE_AVOID);
        assert(wifi_retry_event(&r, WIFI_RETRY_EV_DISCONNECTED, false) == back_want[i]);
    }
    for (int i = 0; i < 1000; i++) {
        assert(wifi_retry_event(&r, WIFI_RETRY_EV_ASSOCIATED, false) == 30000);
        assert(wifi_retry_lease_due(&r, false, false, &w) == WIFI_LEASE_AVOID);
        assert(wifi_retry_event(&r, WIFI_RETRY_EV_DISCONNECTED, false) == 30000);
    }
    /* an address at any point resets everything: the wait, the DHCP step, the back-off */
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_ASSOCIATED, false) == 30000);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_GOT_IP, false) == -1);
    assert(wifi_retry_lease_due(&r, false, false, &w) == WIFI_LEASE_NONE);       /* the timer that lost the race */
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_DISCONNECTED, false) == 1000);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_ASSOCIATED, false) == 10000);
    assert(wifi_retry_lease_due(&r, false, false, &w) == WIFI_LEASE_DHCP);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_GOT_IP, false) == -1);             /* the DHCP restart did it */
    assert(wifi_retry_lease_due(&r, false, false, &w) == WIFI_LEASE_NONE);
    /* a drop during the ladder: the reconnect takes over, the ladder counts nothing */
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_DISCONNECTED, false) == 1000);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_ASSOCIATED, false) == 10000);
    assert(wifi_retry_lease_due(&r, false, false, &w) == WIFI_LEASE_DHCP);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_DISCONNECTED, false) == 2000);     /* one step, as any drop */
    assert(wifi_retry_lease_due(&r, false, false, &w) == WIFI_LEASE_NONE);       /* its timer, late */
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_ASSOCIATED, false) == 10000);      /* still the first wait */
    assert(wifi_retry_lease_due(&r, false, false, &w) == WIFI_LEASE_DHCP);       /* and the cheap step again */
    /* the IP-lost notice ends the current wait at once */
    assert(wifi_retry_lease_due(&r, false, true, &w) == WIFI_LEASE_REJOIN);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_DISCONNECTED, false) == 4000);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_ASSOCIATED, false) == 20000);
    assert(wifi_retry_lease_due(&r, false, true, &w) == WIFI_LEASE_AVOID);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_GOT_IP, false) == -1);
    /* ... and a lease lost later, still associated, starts the ladder from its first step */
    assert(wifi_retry_lease_due(&r, false, true, &w) == WIFI_LEASE_DHCP && w == 8000);
    assert(wifi_retry_lease_due(&r, false, false, &w) == WIFI_LEASE_REJOIN);
    /* never during a setup join (a slow or captive network there is the join's to report) */
    wifi_retry_init(&r);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_CONNECT, false) == -1);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_ASSOCIATED, true) == -1);
    assert(wifi_retry_lease_due(&r, true, false, &w) == WIFI_LEASE_NONE);
    assert(wifi_retry_lease_due(&r, true, true, &w) == WIFI_LEASE_NONE);
    assert(wifi_retry_lease_due(&r, false, false, &w) == WIFI_LEASE_NONE);       /* the join's association: not watched */
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_ASSOCIATED, false) == 10000);      /* watched, then a join starts */
    assert(wifi_retry_lease_due(&r, true, false, &w) == WIFI_LEASE_NONE);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_JOIN_FAILED, true) == 1000);       /* back to the saved network */
    assert(wifi_retry_lease_due(&r, false, false, &w) == WIFI_LEASE_NONE);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_ASSOCIATED, false) == 10000);
    /* never on a fresh device (nothing saved, the first join arms it), never after a forget */
    wifi_retry_init(&r);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_ASSOCIATED, false) == -1);
    assert(wifi_retry_lease_due(&r, false, true, &w) == WIFI_LEASE_NONE);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_GOT_IP, true) == -1);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_ASSOCIATED, false) == 10000);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_FORGET, false) == -1);
    assert(wifi_retry_lease_due(&r, false, false, &w) == WIFI_LEASE_NONE);
    assert(wifi_retry_lease_due(&r, false, true, &w) == WIFI_LEASE_NONE);
    assert(wifi_retry_event(&r, WIFI_RETRY_EV_ASSOCIATED, false) == -1);
    puts("WIFI RETRY PASS: a dropped station reconnects (1 s doubling to 30 s, forever), got-IP resets, never during a join, never after a forget, a failed join falls back to the saved network");
    puts("WIFI LEASE PASS: joined with no address: DHCP restart at 10 s, rejoin 8 s later, then avoid the AP (waits 10, 20, 30 s capped), an address resets all, a drop hands over without double counts, IP-lost acts at once, never during a join or after a forget");
    return 0;
}
