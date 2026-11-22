#include "ap_pick.h"
#include <stddef.h>
#include <string.h>

#define AP_MEM_MAGIC 0x41504d31u   /* "APM1" */

static const uint8_t ZERO[6];

static uint32_t mem_check(const ap_mem_t *m)
{
    uint32_t h = 2166136261u;                       /* FNV-1a over everything before check */
    const uint8_t *p = (const uint8_t *)m;
    for (size_t i = 0; i < offsetof(ap_mem_t, check); i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static void seal(ap_mem_t *m)
{
    m->magic = AP_MEM_MAGIC;
    m->check = mem_check(m);
}

static bool is_set(const uint8_t b[6]) { return memcmp(b, ZERO, 6) != 0; }

static bool bad_now(const ap_mem_t *m, uint32_t now_ms)
{
    return is_set(m->bad) && (int32_t)(m->bad_until_ms - now_ms) > 0;   /* wraps every 49 days */
}

void ap_mem_boot(ap_mem_t *m)
{
    uint8_t good[6];
    const bool ok = m->magic == AP_MEM_MAGIC && m->check == mem_check(m);
    memcpy(good, ok ? m->good : ZERO, 6);
    memset(m, 0, sizeof(*m));
    memcpy(m->good, good, 6);
    seal(m);
}

void ap_mem_good(ap_mem_t *m, const uint8_t bssid[6])
{
    memcpy(m->good, bssid, 6);
    if (!memcmp(m->bad, bssid, 6)) { memset(m->bad, 0, 6); m->bad_until_ms = 0; }
    seal(m);
}

void ap_mem_bad(ap_mem_t *m, const uint8_t bssid[6], uint32_t now_ms)
{
    memcpy(m->bad, bssid, 6);
    m->bad_until_ms = now_ms + AP_AVOID_MS;
    seal(m);
}

void ap_mem_forget(ap_mem_t *m)
{
    memset(m, 0, sizeof(*m));
    seal(m);
}

bool ap_mem_active(const ap_mem_t *m, uint32_t now_ms)
{
    return is_set(m->good) || bad_now(m, now_ms);
}

void ap_seen_add(ap_seen_t *seen, int *n, const uint8_t bssid[6], int8_t rssi, uint8_t channel)
{
    int at = *n;
    if (at == AP_PICK_MAX) {                        /* full: replace the weakest, if weaker */
        at = 0;
        for (int i = 1; i < AP_PICK_MAX; i++) if (seen[i].rssi < seen[at].rssi) at = i;
        if (seen[at].rssi >= rssi) return;
    } else {
        (*n)++;
    }
    memcpy(seen[at].bssid, bssid, 6);
    seen[at].rssi = rssi;
    seen[at].channel = channel;
}

int ap_pick(const ap_mem_t *m, const ap_seen_t *seen, int n, uint32_t now_ms, ap_why_t *why)
{
    const bool avoid = bad_now(m, now_ms);
    int best = -1, last = -1, any = -1;
    for (int i = 0; i < n; i++) {
        if (any < 0 || seen[i].rssi > seen[any].rssi) any = i;
        if (avoid && !memcmp(seen[i].bssid, m->bad, 6)) continue;
        if (best < 0 || seen[i].rssi > seen[best].rssi) best = i;
        if (is_set(m->good) && !memcmp(seen[i].bssid, m->good, 6)) last = i;
    }
    if (best < 0) best = any;                       /* only the avoided one in range: better than none */
    if (last >= 0 && seen[last].rssi + AP_PICK_MARGIN_DB >= seen[best].rssi) {
        *why = AP_PICK_LAST;
        return last;
    }
    *why = best < 0 ? AP_PICK_NONE : AP_PICK_BEST;
    return best;
}
