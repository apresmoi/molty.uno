#include "wifi_retry.h"

void wifi_retry_init(wifi_retry_t *r)
{
    *r = (wifi_retry_t){0};
}

int32_t wifi_retry_event(wifi_retry_t *r, wifi_retry_ev_t ev, bool joining)
{
    switch (ev) {
    case WIFI_RETRY_EV_CONNECT:
        r->armed = true;
        return -1;
    case WIFI_RETRY_EV_GOT_IP:
        r->armed = true;
        r->next_ms = 0;
        r->watching = r->dhcp_kicked = r->leaving = false;
        r->lease_fails = 0;
        return -1;
    case WIFI_RETRY_EV_FORGET:
        wifi_retry_init(r);
        return -1;
    case WIFI_RETRY_EV_ASSOCIATED:
        r->watching = r->armed && !joining;
        r->dhcp_kicked = r->leaving = false;
        if (!r->watching) return -1;
        {
            const uint32_t w = WIFI_LEASE_FIRST_MS << (r->lease_fails < 3 ? r->lease_fails : 3);
            return (int32_t)(w < WIFI_LEASE_MAX_MS ? w : WIFI_LEASE_MAX_MS);
        }
    case WIFI_RETRY_EV_JOIN_FAILED:
        joining = false;                  /* the join is over: it is the caller's own report */
        /* fall through */
    case WIFI_RETRY_EV_DISCONNECTED:
        r->watching = r->leaving = false; /* the reconnect owns it now: the ladder counts nothing */
        if (!r->armed || joining) return -1;
        {
            const uint32_t d = r->next_ms ? r->next_ms : WIFI_RETRY_MIN_MS;
            r->next_ms = d >= WIFI_RETRY_MAX_MS / 2 ? WIFI_RETRY_MAX_MS : d * 2;
            return (int32_t)d;
        }
    }
    return -1;
}

wifi_lease_t wifi_retry_lease_due(wifi_retry_t *r, bool joining, bool ip_lost, uint32_t *wait_ms)
{
    if (!r->armed || joining) return WIFI_LEASE_NONE;
    if (ip_lost && !r->leaving) r->watching = true;   /* a lease lost while associated is a wait too */
    if (!r->watching) return WIFI_LEASE_NONE;
    if (r->lease_fails == 0 && !r->dhcp_kicked) {
        r->dhcp_kicked = true;
        *wait_ms = WIFI_LEASE_DHCP_MS;
        return WIFI_LEASE_DHCP;
    }
    r->watching = false;
    r->leaving = true;                    /* until the disconnect: nothing more to count here */
    const wifi_lease_t a = r->lease_fails == 0 ? WIFI_LEASE_REJOIN : WIFI_LEASE_AVOID;
    if (r->lease_fails < 255) r->lease_fails++;
    return a;
}
