/*
 * SPDX-License-Identifier: MIT
 *
 * net_clock.c: the date from network time (net_clock.h).
 */
#include "net_clock.h"

#include <stdatomic.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "net_clock";

#define CLOCK_VALID_S 1735689600LL                 /* 2025-01-01: before it, the clock was never set (a power-on) */

static atomic_bool s_started;
static atomic_llong s_synced_us;                   /* uptime of the last sync, 0 none */

static void synced(struct timeval *tv) {
    atomic_store(&s_synced_us, esp_timer_get_time());
    ESP_LOGI(TAG, "time synced: %lld", (long long)tv->tv_sec);
}

void net_clock_start(void) {
    if (atomic_exchange(&s_started, true)) return;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_MOLTY_SNTP_SERVER);
    cfg.sync_cb = synced;
    const esp_err_t e = esp_netif_sntp_init(&cfg);
    if (e != ESP_OK) { ESP_LOGW(TAG, "sntp: %s", esp_err_to_name(e)); atomic_store(&s_started, false); }
}

double net_clock_utc(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec >= CLOCK_VALID_S ? (double)tv.tv_sec + tv.tv_usec * 1e-6 : 0.0;
}

cJSON *net_clock_json(void) {
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
    const double t = net_clock_utc();
    const int64_t at = atomic_load(&s_synced_us);
    if (t > 0.0) cJSON_AddNumberToObject(j, "utc", (double)(long long)t); else cJSON_AddNullToObject(j, "utc");
    cJSON_AddStringToObject(j, "source", at ? "sntp" : t > 0.0 ? "kept" : "none");
    if (at) cJSON_AddNumberToObject(j, "synced_s_ago", (double)((esp_timer_get_time() - at) / 1000000)); else cJSON_AddNullToObject(j, "synced_s_ago");
    return j;
}
