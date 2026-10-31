/* sta_watch — the diagnostics' view of the Wi-Fi station (plain C, host-tested:
 * test/host/sta_watch_test.c).
 *
 * mole_diag asks the AP for its rssi only while the station is associated (the driver answers
 * that call with a "W wifi: Haven't to connect to a suitable AP now!" line, once a second for
 * an outage) and logs one wifi.disconnected per outage.
 *
 * The diag registers its Wi-Fi handlers late in boot, after the boot connect has usually
 * delivered STA_CONNECTED already: event tracking alone never saw that association, so rssi
 * read 0 until the first reconnect. sta_watch_seed() supplies the state at registration time
 * (from network.c, which listens from before esp_wifi_start); an event seen since init is
 * fresher and wins over the seed. */
#pragma once
#include <stdbool.h>

typedef struct {
    bool up;             /* associated: there is an AP to ask for its rssi */
    bool evented;        /* a station event arrived since init: a seed is stale */
    bool outage_logged;  /* this outage's wifi.disconnected was logged */
} sta_watch_t;

typedef enum {
    STA_WATCH_EV_CONNECTED,     /* WIFI_EVENT_STA_CONNECTED */
    STA_WATCH_EV_DISCONNECTED,  /* WIFI_EVENT_STA_DISCONNECTED (a drop, or a retry that failed) */
} sta_watch_ev_t;

void sta_watch_init(sta_watch_t *w);
/* The station's state when the handlers were registered; ignored once an event arrived. */
void sta_watch_seed(sta_watch_t *w, bool associated);
/* One station event. Returns true when it is news worth logging: the first disconnect of an
 * outage (the retries that fail again every 1..30 s, net/wifi_retry.h, are not). */
bool sta_watch_event(sta_watch_t *w, sta_watch_ev_t ev);
bool sta_watch_up(const sta_watch_t *w);
