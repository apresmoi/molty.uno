#include "wifi_retry.h"

void wifi_retry_init(wifi_retry_t *r)
{
    r->next_ms = 0;
    r->armed = false;
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
        return -1;
    case WIFI_RETRY_EV_FORGET:
        r->armed = false;
        r->next_ms = 0;
        return -1;
    case WIFI_RETRY_EV_DISCONNECTED:
        if (!r->armed || joining) return -1;
        {
            const uint32_t d = r->next_ms ? r->next_ms : WIFI_RETRY_MIN_MS;
            r->next_ms = d >= WIFI_RETRY_MAX_MS / 2 ? WIFI_RETRY_MAX_MS : d * 2;
            return (int32_t)d;
        }
    }
    return -1;
}
