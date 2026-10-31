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
 *     nor after a forget (no credentials), nor before the first connect was ever asked for. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define WIFI_RETRY_MIN_MS 1000u
#define WIFI_RETRY_MAX_MS 30000u

typedef struct {
    uint32_t next_ms;      /* the next back-off; 0 = the first one (WIFI_RETRY_MIN_MS) */
    bool armed;            /* a station with credentials was asked to connect: drops are retried */
} wifi_retry_t;

typedef enum {
    WIFI_RETRY_EV_CONNECT,       /* the station was asked to connect with saved credentials */
    WIFI_RETRY_EV_GOT_IP,        /* connected: the back-off starts over */
    WIFI_RETRY_EV_DISCONNECTED,  /* the station lost (or never got) its AP */
    WIFI_RETRY_EV_FORGET,        /* credentials erased: never retry */
} wifi_retry_ev_t;

void wifi_retry_init(wifi_retry_t *r);
/* One station event. joining: a join (set_creds / Improv) is running and reports itself.
 * Returns the delay in ms before esp_wifi_connect(), or -1: do nothing. */
int32_t wifi_retry_event(wifi_retry_t *r, wifi_retry_ev_t ev, bool joining);
