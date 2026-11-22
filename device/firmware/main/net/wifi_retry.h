/* wifi_retry — when the station comes back after a drop (plain C, host-tested:
 * test/host/wifi_retry_test.c).
 *
 * ESP-IDF never reconnects a station on its own: after WIFI_EVENT_STA_DISCONNECTED the
 * driver stays idle until someone calls esp_wifi_connect(). Before this module nobody did
 * (only the boot connect, a join and a scan), so one weak-signal drop left Molty offline
 * until a reboot — no relay, no OTA over Wi-Fi, "W wifi: Haven't to connect" every second.
 *
 * network.c feeds the station events in; the answer is when to call esp_wifi_connect()
 * again (a one-shot timer), or never:
 *   - a drop of a station that has saved credentials: retry, 1 s → 2 → 4 → … capped at
 *     WIFI_RETRY_MAX_MS, for as long as it takes (an AP that comes back is found again);
 *   - a got-IP resets the back-off;
 *   - no retry while a join (set_creds / Improv) owns the radio — it reports its own result —
 *     nor after a forget (no credentials), nor before the first connect was ever asked for;
 *   - a join that fails falls back to the saved network, on the back-off. Its own disconnects
 *     were not retried, so before this a failed set_creds (a wrong password, an extender that
 *     refuses) left the station idle for good, aimed at the network it could not join
 *     (2026-11-16: four failed joins to an extender, offline 6+ minutes until a reboot);
 *   - joined but no address (no DHCP lease): the lease ladder, below.
 *
 * The lease ladder. A drop is not the only way to be off the network: a station can be
 * associated and never get an address (a repeater that still answers for Molty from before a
 * restart, an OFFER lost on a weak hop, the DHCP client parked after a decline). Before this
 * nothing reacted and Molty waited as long as the network took (2026-11-22: 220 s after a
 * restart, with a fine signal). Now, from each association that gets no address:
 *   1. WIFI_LEASE_FIRST_MS: restart the DHCP client (a fresh DISCOVER with short timeouts, and a
 *      client parked after a decline asks again). Cheap: the association stays.
 *   2. WIFI_LEASE_DHCP_MS after that: leave and join again (the access point is chosen as on any
 *      join, net/ap_pick.h: normally the same one). A fresh association makes the access point and
 *      the mesh learn again where Molty is.
 *   3. an association after that one with no address: leave, and avoid that access point for a
 *      while (another one of the same network if one is in range).
 * Each association in a row without an address waits twice as long (10 s, 20 s, 30 s, capped),
 * and the rejoins go through the reconnect back-off above, so a network that is really down sees
 * about one join a minute once both have reached their caps. The IDF "IP lost" notice (posted 120 s after the DHCP client
 * started with no address, and after a lease lost while associated) ends the current wait at once.
 * Never during a join (it has its own timeout, and a slow or captive network there is the join's
 * failure to report, not a lease to chase), never after a forget. An address resets it all. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define WIFI_RETRY_MIN_MS 1000u
#define WIFI_RETRY_MAX_MS 30000u
/* A good lease comes 1 to 3 s after the join, a slow router's in 5 to 10 s; by 10 s the DHCP
 * client has sent five DISCOVERs (0, 0.5, 1.5, 3.5, 7.5 s) with no answer. */
#define WIFI_LEASE_FIRST_MS 10000u
#define WIFI_LEASE_DHCP_MS   8000u   /* after the DHCP restart: four more DISCOVERs */
#define WIFI_LEASE_MAX_MS   30000u

typedef struct {
    uint32_t next_ms;      /* the next back-off; 0 = the first one (WIFI_RETRY_MIN_MS) */
    bool armed;            /* a station with credentials was asked to connect: drops are retried */
    bool watching;         /* associated, no address yet: a lease wait runs */
    bool dhcp_kicked;      /* this association's DHCP client was restarted (step 1) */
    bool leaving;          /* the ladder left this AP: the disconnect is on its way */
    uint8_t lease_fails;   /* associations in a row left without an address */
} wifi_retry_t;

typedef enum {
    WIFI_RETRY_EV_CONNECT,       /* the station was asked to connect with saved credentials */
    WIFI_RETRY_EV_GOT_IP,        /* connected: the back-off and the lease ladder start over */
    WIFI_RETRY_EV_DISCONNECTED,  /* the station lost (or never got) its AP */
    WIFI_RETRY_EV_FORGET,        /* credentials erased: never retry */
    WIFI_RETRY_EV_JOIN_FAILED,   /* a join gave up: the saved network (if any) is restored, retry it */
    WIFI_RETRY_EV_ASSOCIATED,    /* joined an AP (STA_CONNECTED): returns the lease wait, or -1 */
} wifi_retry_ev_t;

typedef enum {
    WIFI_LEASE_NONE,    /* nothing to do: an address, a join, not associated */
    WIFI_LEASE_DHCP,    /* restart the DHCP client, then wait *wait_ms more */
    WIFI_LEASE_REJOIN,  /* leave the AP: the reconnect joins again */
    WIFI_LEASE_AVOID,   /* leave the AP and avoid it for a while (net/ap_pick.h) */
} wifi_lease_t;

void wifi_retry_init(wifi_retry_t *r);
/* One station event. joining: a join (set_creds / Improv) is running and reports itself.
 * Returns the delay in ms before esp_wifi_connect() (WIFI_RETRY_EV_ASSOCIATED: before
 * wifi_retry_lease_due()), or -1: do nothing. */
int32_t wifi_retry_event(wifi_retry_t *r, wifi_retry_ev_t ev, bool joining);
/* The lease wait ran out (ip_lost false), or IDF posted IP_EVENT_STA_LOST_IP while associated
 * (ip_lost true: it also starts the ladder after a lease lost later on). The next step. */
wifi_lease_t wifi_retry_lease_due(wifi_retry_t *r, bool joining, bool ip_lost, uint32_t *wait_ms);
