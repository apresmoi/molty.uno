/*
 * SPDX-License-Identifier: MIT
 * USB NCM glue evolved from Espressif's tusb_ncm / sta2eth examples (Unlicense OR CC0-1.0).
 *
 * network.c — the device's two network interfaces + mDNS + the Wi-Fi setup API.
 *
 *   USB-C: TinyUSB NCM (Ethernet over USB). The device is 10.13.37.1/24 and runs
 *          a DHCP server, so a laptop gets an address with no driver install.
 *   Wi-Fi: STA only, credentials in NVS (namespace "wifi", keys "ssid"/"pass").
 *   mDNS:  "molty.local" on every registered interface (each answers with its
 *          own address) + "molty-<6 hex>.local" on Wi-Fi only.
 *   HTTP:  GET /api/status, GET /api/scan, POST /api/set_creds, POST /api/forget.
 */
#include "network.h"
#include "http.h"
#include "wifi_retry.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_netif_net_stack.h"
#include "lwip/esp_netif_net_stack.h"
#include "esp_netif_defaults.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "nvs.h"
#include "tinyusb.h"
#include "tinyusb_net.h"
#include "apps/dhcpserver/dhcpserver_options.h"
#include "sdkconfig.h"

static const char *TAG = "net";

// ── Static IP config for the USB NCM side ─────────────────────────────────────
// Device: NET_USB_IP (10.13.37.1)/24 — the host receives 10.13.37.x via DHCP

// ── WiFi state ─────────────────────────────────────────────────────────────────
typedef enum {
    WIFI_PORTAL_STATE_SETUP_NEEDED  = 0,
    WIFI_PORTAL_STATE_CONNECTING    = 1,
    WIFI_PORTAL_STATE_CONNECTED     = 2,
    WIFI_PORTAL_STATE_DISCONNECTED  = 3,
} wifi_portal_state_t;

static volatile wifi_portal_state_t s_wifi_state = WIFI_PORTAL_STATE_SETUP_NEEDED;
static char s_ssid_buf[64]  = {};
static char s_ip_buf[20]    = {};

// ── netif handle (USB side) ────────────────────────────────────────────────────
static esp_netif_t *s_usb_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static bool         s_sta_in_mdns = false;

// ── Per-interface delegated mDNS hostnames ─────────────────────────────────────
// s_unique_hostname  = "molty-<6hex>"  e.g. "molty-a1b2c3" — Wi-Fi-only delegate
// s_main_hostname    = "molty-fw-<12hex>" e.g. "molty-fw-a1b2c3d4e5f6" — main hostname
//   (users never type it; it exists because mdns_hostname_set() requires one)
// s_unique_delegate_present tracks whether the Wi-Fi delegate has been registered
//   so we can guard against double-add on reconnect and clean up on disconnect.
static char s_unique_hostname[20];   // "molty-" + 6 hex chars + NUL = 14 bytes; 20 is safe
static char s_main_hostname[28];     // "molty-fw-" + 12 hex chars + NUL = 22 bytes; 28 is safe
static bool s_unique_delegate_present = false;

// ── Event group for WiFi connect/disconnect ────────────────────────────────────
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
static EventGroupHandle_t s_wifi_event_group = NULL;
static SemaphoreHandle_t  s_wifi_op = NULL;       /* one join or scan at a time */

// ── Reconnect after a drop (wifi_retry.h): IDF never reconnects a station by itself ──
static wifi_retry_t       s_retry;                /* the event handler's (the default event loop) */
static esp_timer_handle_t s_retry_timer = NULL;
static volatile bool      s_joining = false;      /* a join owns the radio: it reports its own result */

static void retry_timer_cb(void *arg)
{
    (void)arg;
    if (s_joining || !s_wifi_op) return;          /* a join reports its own result */
    if (xSemaphoreTake(s_wifi_op, 0) != pdTRUE) { /* a scan has the radio: try again after it */
        esp_timer_start_once(s_retry_timer, (uint64_t)WIFI_RETRY_MIN_MS * 1000);
        return;
    }
    s_wifi_state = WIFI_PORTAL_STATE_CONNECTING;
    esp_err_t e = esp_wifi_connect();
    xSemaphoreGive(s_wifi_op);
    if (e != ESP_OK) ESP_LOGW(TAG, "reconnect: esp_wifi_connect %s", esp_err_to_name(e));
}

static void retry_after(int32_t ms)
{
    if (ms < 0 || !s_retry_timer) return;
    esp_timer_stop(s_retry_timer);                 /* one pending retry at most */
    ESP_LOGI(TAG, "Wi-Fi dropped: reconnecting in %ld ms", (long)ms);
    esp_timer_start_once(s_retry_timer, (uint64_t)ms * 1000);
}

// ─────────────────────────────────────────────────────────────────────────────
// NVS helpers — namespace "wifi", keys "ssid" / "pass"
// Arduino Preferences stores strings in NVS verbatim under the namespace you
// pass to prefs.begin(). The key names are stored as-is, so nvs_get_str with
// the same names reads them back correctly.
// ─────────────────────────────────────────────────────────────────────────────

static esp_err_t nvs_read_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open("wifi", NVS_READONLY, &h);
    if (ret != ESP_OK) return ret;

    ret = nvs_get_str(h, "ssid", ssid, &ssid_len);
    if (ret == ESP_OK) {
        size_t pl = pass_len;
        ret = nvs_get_str(h, "pass", pass, &pl);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            pass[0] = '\0';
            ret = ESP_OK;
        }
    }
    nvs_close(h);
    return ret;
}

static esp_err_t nvs_write_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open("wifi", NVS_READWRITE, &h), TAG, "nvs_open failed");
    esp_err_t ret = nvs_set_str(h, "ssid", ssid);
    if (ret == ESP_OK) ret = nvs_set_str(h, "pass", pass ? pass : "");
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

static esp_err_t nvs_forget_creds(void)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open("wifi", NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;
    nvs_erase_key(h, "ssid");   // ignore NOT_FOUND
    nvs_erase_key(h, "pass");
    ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

// ─────────────────────────────────────────────────────────────────────────────
// USB NCM → esp_netif glue
// We need to forward packets between the tinyusb_net callbacks and the lwIP
// netif. The approach mirrors IDF's sta2eth example (components/network/sta2eth).
// ─────────────────────────────────────────────────────────────────────────────

// Called by tinyusb_net when a frame arrives from the USB host.
// We copy the buffer (tinyusb owns the original) and hand it to esp_netif.
static esp_err_t usb_recv_callback(void *buffer, uint16_t len, void *ctx)
{
    if (!s_usb_netif) return ESP_OK;

    void *buf_copy = malloc(len);
    if (!buf_copy) {
        ESP_LOGE(TAG, "OOM copying USB rx frame");
        return ESP_ERR_NO_MEM;
    }
    memcpy(buf_copy, buffer, len);
    return esp_netif_receive(s_usb_netif, buf_copy, len, NULL);
}

// Called by esp_netif when lwIP wants to transmit a frame.
static esp_err_t netif_transmit(void *h, void *buffer, size_t len)
{
    esp_err_t ret = tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(200));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "USB tx failed: %d", ret);
    }
    return ESP_OK;
}

// Called by esp_netif to free buffers it allocated in usb_recv_callback.
static void netif_free_rx_buf(void *h, void *buffer)
{
    free(buffer);
}

// Create the esp_netif for the USB side with a static IP and DHCP server.
static esp_err_t usb_netif_init(void)
{
    // NCM device MAC: locally-administered, device side (bit 1 of first octet = 1)
    // The host-side MAC is set in tinyusb_net_config.mac_addr below.
    // Both MACs must differ — use adjacent LAA addresses.
    uint8_t device_mac[6] = {0x02, 0x02, 0x11, 0x22, 0x33, 0x02};

    // 1) Inherent config: DHCP server + auto-up (static IP, no DHCP client)
    static esp_netif_ip_info_t ip_info = {};
    IP4_ADDR(&ip_info.ip,      10, 13, 37,   1);
    // gw = 0.0.0.0 so the DHCP server does NOT advertise itself as the default
    // gateway. Otherwise macOS may route general internet traffic through this
    // USB Ethernet (which has no upstream) and the user sees "no internet" until
    // they manually re-order network services.
    IP4_ADDR(&ip_info.gw,      0,  0,  0,    0);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    esp_netif_inherent_config_t base_cfg = {
        .flags    = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP),
        .ip_info  = &ip_info,
        .if_key   = "USB_NCM",
        .if_desc  = "usb ncm portal",
        .route_prio = 10,
    };

    // 2) Driver config: point to our transmit + free functions
    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle              = (void *)1,      // singleton — must be non-NULL
        .transmit            = netif_transmit,
        .driver_free_rx_buffer = netif_free_rx_buf,
    };

    // 3) Stack config: use the lwIP Ethernet netif stack (same as real Ethernet)
    struct esp_netif_netstack_config lwip_cfg = {
        .lwip = {
            .init_fn  = ethernetif_init,
            .input_fn = ethernetif_input,
        }
    };

    esp_netif_config_t cfg = {
        .base   = &base_cfg,
        .driver = &driver_cfg,
        .stack  = &lwip_cfg,
    };

    s_usb_netif = esp_netif_new(&cfg);
    if (!s_usb_netif) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return ESP_FAIL;
    }

    esp_netif_set_mac(s_usb_netif, device_mac);

    // DHCP server lease time: 3600 s (1 hour)
    uint32_t lease_secs = 3600;
    esp_netif_dhcps_option(s_usb_netif, ESP_NETIF_OP_SET,
                           IP_ADDRESS_LEASE_TIME,
                           &lease_secs, sizeof(lease_secs));

    // Bring the interface up (driver is already "started" via tinyusb)
    esp_netif_action_start(s_usb_netif, 0, 0, 0);
    esp_netif_create_ip6_linklocal(s_usb_netif);   // AAAA for mDNS (see wifi_event_handler)

    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// mDNS — advertise hostname "molty" on the USB netif
// lwIP's built-in mdns_resp requires LWIP_MDNS_RESPONDER which is not exposed
// via Kconfig in ESP-IDF 5.x lwIP port. We use the espressif/mdns component
// declared in idf_component.yml instead.
// NOTE: At build time we check whether CONFIG_MDNS_MAX_SERVICES is defined
// (espressif/mdns component) and fall back to a no-op if not present.
// ─────────────────────────────────────────────────────────────────────────────
#if defined(CONFIG_MDNS_MAX_SERVICES)
#include "mdns.h"
void net_mdns_start(void)
{
    // ── Derive both hostnames from the Wi-Fi STA MAC ──────────────────────────
    // esp_read_mac() works even before Wi-Fi is started; the MAC is burned into
    // eFuse at the factory. We use ESP_MAC_WIFI_STA (the "base" MAC).  On fail
    // we fall back to all-zeros — the hostnames remain well-defined, just non-unique.
    uint8_t mac[6] = {0};
    esp_err_t mac_err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (mac_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_read_mac failed (%s) — using zero MAC for hostnames", esp_err_to_name(mac_err));
    }

    // s_main_hostname  = "molty-fw-<12 lowercase hex>"  e.g. "molty-fw-a1b2c3d4e5f6"
    // s_unique_hostname = "molty-<6 lowercase hex of last 3 bytes>" e.g. "molty-a1b2c3"
    snprintf(s_main_hostname, sizeof(s_main_hostname),
             "molty-fw-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_unique_hostname, sizeof(s_unique_hostname),
             "molty-%02x%02x%02x",
             mac[3], mac[4], mac[5]);

    // ── mDNS init ─────────────────────────────────────────────────────────────
    // Main hostname is the hidden per-device unique one. Nobody types this; it
    // satisfies the library's requirement for a main hostname and auto-advertises
    // on all registered netifs (dual-A-record is fine — it's not user-facing).
    // "molty" is the MAIN hostname: the responder answers it on every registered
    // netif with THAT netif's own address (USB -> 10.13.37.1, Wi-Fi -> DHCP IP).
    // It used to be a delegated hostname pinned to 10.13.37.1, and delegates are
    // answered verbatim on all netifs — so Wi-Fi clients got the USB address and
    // http://molty.local never loaded over Wi-Fi (fixed 2026-10-25).
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("molty"));
    ESP_ERROR_CHECK(mdns_instance_name_set("molty setup portal"));

    // ── Register USB netif ────────────────────────────────────────────────────
    // We disabled STA/AP auto-registration in sdkconfig (CONFIG_MDNS_PREDEF_NETIF_STA=n)
    // so the main hostname doesn't leak onto home Wi-Fi. Register the USB netif
    // explicitly so mDNS receives + responds to queries on it.
    if (s_usb_netif) {
        ESP_ERROR_CHECK(mdns_register_netif(s_usb_netif));
        ESP_ERROR_CHECK(mdns_netif_action(s_usb_netif,
            (mdns_event_actions_t)(MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4 |
                                   MDNS_EVENT_ENABLE_IP6 | MDNS_EVENT_ANNOUNCE_IP6)));
    }


    // ── HTTP service ──────────────────────────────────────────────────────────
    // Service advertises under the instance name (mdns_instance_name_set above),
    // so Bonjour/Avahi browsers find it as "molty setup portal._http._tcp.local"
    // regardless of which hostname they resolve.
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    ESP_LOGI(TAG, "mDNS hostnames: molty.local (per-interface address), %s.local (Wi-Fi when up)",
             s_unique_hostname);
}
#else
void net_mdns_start(void)
{
    // espressif/mdns component not in idf_component.yml — mDNS unavailable.
    // Users can still reach the portal at http://10.13.37.1/
    ESP_LOGW(TAG, "mDNS component not found — access portal at http://" NET_USB_IP "/");
}
#endif

// ─────────────────────────────────────────────────────────────────────────────
// WiFi event handler — used only for the blocking connect attempt in set_creds
// ─────────────────────────────────────────────────────────────────────────────
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED && s_sta_netif) {
        // Link-local IPv6 so mDNS can answer AAAA: the responder never sends an
        // NSEC "no AAAA" record, so macOS getaddrinfo waited 5 s on every
        // molty.local lookup for an AAAA answer that never came.
        esp_netif_create_ip6_linklocal(s_sta_netif);
    } else if (base == IP_EVENT && event_id == IP_EVENT_GOT_IP6) {
#if defined(CONFIG_MDNS_MAX_SERVICES)
        ip_event_got_ip6_t *ev6 = (ip_event_got_ip6_t *)data;
        if (ev6 && (ev6->esp_netif == s_usb_netif || (ev6->esp_netif == s_sta_netif && s_sta_in_mdns))) {
            mdns_netif_action(ev6->esp_netif,
                (mdns_event_actions_t)(MDNS_EVENT_ENABLE_IP6 | MDNS_EVENT_ANNOUNCE_IP6));
        }
#endif
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        /* a forget already said "setup needed": that one is never retried */
        retry_after(wifi_retry_event(&s_retry, s_wifi_state == WIFI_PORTAL_STATE_SETUP_NEEDED
                                     ? WIFI_RETRY_EV_FORGET : WIFI_RETRY_EV_DISCONNECTED, s_joining));
        if (s_wifi_state != WIFI_PORTAL_STATE_SETUP_NEEDED) s_wifi_state = WIFI_PORTAL_STATE_DISCONNECTED;
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
#if defined(CONFIG_MDNS_MAX_SERVICES)
        // Unregister the STA netif from mDNS (the main-hostname A-record on Wi-Fi).
        if (s_sta_in_mdns && s_sta_netif) {
            mdns_netif_action(s_sta_netif, MDNS_EVENT_DISABLE_IP4);
            mdns_unregister_netif(s_sta_netif);
            s_sta_in_mdns = false;
            ESP_LOGI(TAG, "mDNS: STA interface removed (Wi-Fi disconnected)");
        }
        // Remove the Wi-Fi-only delegated hostname (molty-<id>.local).
        // This ensures there is no stale A-record pointing at a now-unreachable IP.
        if (s_unique_delegate_present) {
            esp_err_t rem = mdns_delegate_hostname_remove(s_unique_hostname);
            if (rem == ESP_OK) {
                ESP_LOGI(TAG, "mDNS delegate removed: %s.local", s_unique_hostname);
            } else if (rem == ESP_ERR_NOT_FOUND) {
                ESP_LOGD(TAG, "mDNS delegate %s.local not found on remove (already gone)", s_unique_hostname);
            } else {
                ESP_LOGW(TAG, "mDNS delegate remove %s.local failed: %s", s_unique_hostname, esp_err_to_name(rem));
            }
            s_unique_delegate_present = false;
        }
#endif
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip_buf, sizeof(s_ip_buf), IPSTR, IP2STR(&ev->ip_info.ip));
        s_wifi_state = WIFI_PORTAL_STATE_CONNECTED;
        wifi_retry_event(&s_retry, WIFI_RETRY_EV_GOT_IP, s_joining);
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        ESP_LOGI(TAG, "WiFi connected: ip=%s", s_ip_buf);
#if defined(CONFIG_MDNS_MAX_SERVICES)
        // Register the STA netif so the main (hidden) hostname resolves on Wi-Fi
        // too.  USB stays registered always; the main hostname gets dual A-records
        // (one per interface) — that is acceptable since users never type it.
        if (!s_sta_in_mdns && s_sta_netif) {
            esp_err_t mr = mdns_register_netif(s_sta_netif);
            if (mr == ESP_OK) {
                mdns_netif_action(s_sta_netif,
                    (mdns_event_actions_t)(MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4 |
                                   MDNS_EVENT_ENABLE_IP6 | MDNS_EVENT_ANNOUNCE_IP6));
                s_sta_in_mdns = true;
                ESP_LOGI(TAG, "mDNS: STA interface registered for main hostname");
            } else {
                ESP_LOGW(TAG, "mDNS: STA register failed: %s", esp_err_to_name(mr));
            }
        }
        // Add the Wi-Fi-only delegated hostname: molty-<id>.local → <wifi-ip>.
        // Guard against double-add on reconnect without an intervening disconnect.
        if (!s_unique_delegate_present) {
            mdns_ip_addr_t sta_addr = {
                .addr = {
                    .u_addr = { .ip4 = { .addr = ev->ip_info.ip.addr } },
                    .type = ESP_IPADDR_TYPE_V4,
                },
                .next = NULL,
            };
            esp_err_t da = mdns_delegate_hostname_add(s_unique_hostname, &sta_addr);
            if (da == ESP_OK) {
                s_unique_delegate_present = true;
                ESP_LOGI(TAG, "mDNS delegate added: %s.local → %s", s_unique_hostname, s_ip_buf);
            } else {
                ESP_LOGW(TAG, "mDNS delegate add %s.local failed: %s", s_unique_hostname, esp_err_to_name(da));
            }
        } else {
            // Already present (reconnect without disconnect event) — update the address.
            mdns_ip_addr_t sta_addr = {
                .addr = {
                    .u_addr = { .ip4 = { .addr = ev->ip_info.ip.addr } },
                    .type = ESP_IPADDR_TYPE_V4,
                },
                .next = NULL,
            };
            esp_err_t da = mdns_delegate_hostname_set_address(s_unique_hostname, &sta_addr);
            if (da == ESP_OK) {
                ESP_LOGI(TAG, "mDNS delegate updated: %s.local → %s", s_unique_hostname, s_ip_buf);
            } else {
                ESP_LOGW(TAG, "mDNS delegate update %s.local failed: %s", s_unique_hostname, esp_err_to_name(da));
            }
        }
#endif
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// WiFi init — STA mode, no auto-connect
// ─────────────────────────────────────────────────────────────────────────────
esp_err_t net_wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    s_wifi_op = xSemaphoreCreateMutex();
    if (!s_wifi_event_group || !s_wifi_op) return ESP_ERR_NO_MEM;
    wifi_retry_init(&s_retry);
    const esp_timer_create_args_t retry_args = { .callback = retry_timer_cb, .name = "wifi_retry" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&retry_args, &s_retry_timer), TAG, "retry timer");

    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_cfg), TAG, "esp_wifi_init failed");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL),
        TAG, "register wifi handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL),
        TAG, "register ip handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, wifi_event_handler, NULL),
        TAG, "register ip6 handler");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set STA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");
    // Modem sleep (the IDF default, WIFI_PS_MIN_MODEM) parks the radio between
    // DTIM beacons: every inbound packet waits for the AP's next beacon. Measured
    // 2026-10-25: LAN ping 72 ms avg / 186 ms max with it, 14 ms avg without.
    // Molty is an interactive surface on USB power; latency wins. Runtime knob:
    // POST /api/diag/wifi_ps?mode=none|min|max.
    esp_wifi_set_ps(WIFI_PS_NONE);

    // If there are saved credentials, connect now so /api/status can return
    // "connected" immediately after the portal starts.
    char saved_ssid[64] = {};
    char saved_pass[128] = {};
    if (nvs_read_creds(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass)) == ESP_OK
        && saved_ssid[0] != '\0') {
        ESP_LOGI(TAG, "Saved creds found, connecting to \"%s\"", saved_ssid);
        wifi_config_t wcfg = {};
        strncpy((char *)wcfg.sta.ssid,     saved_ssid, sizeof(wcfg.sta.ssid)     - 1);
        strncpy((char *)wcfg.sta.password, saved_pass, sizeof(wcfg.sta.password) - 1);
        esp_wifi_set_config(WIFI_IF_STA, &wcfg);
        strncpy(s_ssid_buf, saved_ssid, sizeof(s_ssid_buf) - 1);
        s_wifi_state = WIFI_PORTAL_STATE_CONNECTING;
        wifi_retry_event(&s_retry, WIFI_RETRY_EV_CONNECT, false);   /* a drop from here is retried */
        esp_wifi_connect();
        // Don't block here — let it connect in background while HTTP server comes up
    }

    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /api/status
// ─────────────────────────────────────────────────────────────────────────────
static esp_err_t handler_status(httpd_req_t *r)
{
    char buf[256];
    const char *state_str;

    switch (s_wifi_state) {
        case WIFI_PORTAL_STATE_SETUP_NEEDED:  state_str = "setup_needed";  break;
        case WIFI_PORTAL_STATE_CONNECTING:    state_str = "connecting";    break;
        case WIFI_PORTAL_STATE_CONNECTED:     state_str = "connected";     break;
        case WIFI_PORTAL_STATE_DISCONNECTED:  state_str = "disconnected";  break;
        default:                              state_str = "unknown";        break;
    }

    if (s_wifi_state == WIFI_PORTAL_STATE_CONNECTED) {
        int rssi = 0;
        esp_wifi_sta_get_rssi(&rssi);
        snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d}",
            state_str, s_ssid_buf, s_ip_buf, rssi);
    } else if (s_ssid_buf[0]) {
        snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"state\":\"%s\",\"ssid\":\"%s\"}",
            state_str, s_ssid_buf);
    } else {
        snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"state\":\"%s\"}",
            state_str);
    }

    return http_send_json(r, buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /api/scan
// Triggers a blocking WiFi scan, sorts by RSSI descending, returns up to 12.
// ─────────────────────────────────────────────────────────────────────────────
bool net_wifi_ready(void) { return s_wifi_op != NULL; }

bool net_wifi_ip(char *ip, size_t len)
{
    if (s_wifi_state != WIFI_PORTAL_STATE_CONNECTED || !s_ip_buf[0]) return false;
    snprintf(ip, len, "%s", s_ip_buf);
    return true;
}

int net_wifi_scan(net_ap_t *out, int max)
{
    if (!s_wifi_op || max <= 0) return -1;
    xSemaphoreTake(s_wifi_op, portMAX_DELAY);
    // Reconnect afterwards if we were connected (the scan may drop the association).
    bool was_connected = (s_wifi_state == WIFI_PORTAL_STATE_CONNECTED);

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {   // blocking
        xSemaphoreGive(s_wifi_op);
        return -1;
    }

    uint16_t num = 12;
    wifi_ap_record_t records[12];
    esp_wifi_scan_get_ap_records(&num, records);

    // Sort by RSSI descending (bubble sort, max 12 entries)
    for (uint16_t i = 0; i + 1 < num; i++) {
        for (uint16_t j = i + 1; j < num; j++) {
            if (records[j].rssi > records[i].rssi) {
                wifi_ap_record_t tmp = records[i];
                records[i] = records[j];
                records[j] = tmp;
            }
        }
    }
    int n = num < max ? num : max;
    for (int i = 0; i < n; i++) {
        memcpy(out[i].ssid, records[i].ssid, sizeof(out[i].ssid) - 1);
        out[i].ssid[sizeof(out[i].ssid) - 1] = '\0';
        out[i].rssi = records[i].rssi;
        out[i].open = records[i].authmode == WIFI_AUTH_OPEN;
    }

    if (was_connected) {
        // Reconnect in background
        esp_wifi_connect();
    }
    xSemaphoreGive(s_wifi_op);
    return n;
}

static esp_err_t handler_scan(httpd_req_t *r)
{
    net_ap_t aps[12];
    int num = net_wifi_scan(aps, 12);
    if (num < 0) {
        return http_send_json(r, "{\"ok\":false,\"error\":\"scan_failed\"}");
    }

    // Build JSON by hand — no heap allocation needed, stack is fine for 12 entries
    // Max per entry: ~80 chars (ssid 32 + rssi -100 + open + punctuation)
    // Total max: ~1200 chars + overhead → 1536 byte stack buf is safe
    char buf[1536];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"ok\":true,\"networks\":[");

    for (int i = 0; i < num && pos < (int)sizeof(buf) - 40; i++) {
        // Escape SSID: replace " and \ to avoid JSON injection
        char safe_ssid[70];
        size_t si = 0;
        for (size_t ci = 0; aps[i].ssid[ci] && si < sizeof(safe_ssid) - 3; ci++) {
            unsigned char c = (unsigned char)aps[i].ssid[ci];
            if (c == '"' || c == '\\') { safe_ssid[si++] = '\\'; }
            safe_ssid[si++] = (char)c;
        }
        safe_ssid[si] = '\0';

        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
            (i == 0 ? "" : ","),
            safe_ssid,
            (int)aps[i].rssi,
            aps[i].open ? "true" : "false");
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    return http_send_json(r, buf);
}


// ─────────────────────────────────────────────────────────────────────────────
// POST /api/set_creds
// Body: {"ssid":"...","password":"..."}
// Tries to connect with 15 s timeout. On success saves to NVS.
// ─────────────────────────────────────────────────────────────────────────────
static esp_err_t handler_set_creds(httpd_req_t *r)
{
    // Read body
    int content_len = r->content_len;
    if (content_len <= 0 || content_len > 512) {
        return http_send_json(r, "{\"ok\":false,\"error\":\"bad_request\"}");
    }

    char body[512 + 1];
    int received = http_recv_full(r, body, content_len);
    if (received <= 0) {
        return http_send_json(r, "{\"ok\":false,\"error\":\"recv_failed\"}");
    }
    body[received] = '\0';

    // Parse
    char ssid[64]  = {};
    char pass[128] = {};
    if (!http_json_get_str(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        return http_send_json(r, "{\"ok\":false,\"error\":\"missing_ssid\"}");
    }
    http_json_get_str(body, "password", pass, sizeof(pass));  // empty = open network

    esp_err_t e = net_wifi_join(ssid, pass, 15000);
    if (e == ESP_OK) {
        int rssi = 0;
        esp_wifi_sta_get_rssi(&rssi);

        char resp[128];
        snprintf(resp, sizeof(resp),
            "{\"ok\":true,\"ip\":\"%s\",\"rssi\":%d}", s_ip_buf, rssi);
        return http_send_json(r, resp);
    } else {
        // Get WiFi error code
        wifi_ap_record_t ap;
        int wifi_status = (e == ESP_FAIL) ? 4 : 255;
        // Try to get a more specific code
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_ERR_WIFI_NOT_CONNECT) {
            wifi_status = 4;
        }

        char resp[128];
        snprintf(resp, sizeof(resp),
            "{\"ok\":false,\"error\":\"connect_failed\",\"wifi_status\":%d}", wifi_status);
        return http_send_json(r, resp);
    }
}

esp_err_t net_wifi_join(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    if (!s_wifi_op) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_wifi_op, portMAX_DELAY);
    s_joining = true;                              /* its own disconnects are not drops to retry */
    if (s_retry_timer) esp_timer_stop(s_retry_timer);

    // Disconnect if currently connected
    if (s_wifi_state == WIFI_PORTAL_STATE_CONNECTED ||
        s_wifi_state == WIFI_PORTAL_STATE_CONNECTING) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // Set new config and connect
    wifi_config_t wcfg = {};
    strncpy((char *)wcfg.sta.ssid,     ssid, sizeof(wcfg.sta.ssid)     - 1);
    strncpy((char *)wcfg.sta.password, pass, sizeof(wcfg.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &wcfg);

    strncpy(s_ssid_buf, ssid, sizeof(s_ssid_buf) - 1);
    s_ssid_buf[sizeof(s_ssid_buf) - 1] = '\0';
    s_ip_buf[0] = '\0';
    s_wifi_state = WIFI_PORTAL_STATE_CONNECTING;

    // Clear event bits before connecting
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdTRUE,    // clear on exit
                                           pdFALSE,   // any bit
                                           pdMS_TO_TICKS(timeout_ms));
    esp_err_t e;
    if (bits & WIFI_CONNECTED_BIT) {
        // Save to NVS
        esp_err_t nret = nvs_write_creds(ssid, pass);
        if (nret != ESP_OK) {
            ESP_LOGW(TAG, "NVS write failed: %s", esp_err_to_name(nret));
        }
        e = ESP_OK;
    } else {
        s_wifi_state = WIFI_PORTAL_STATE_DISCONNECTED;
        e = (bits & WIFI_FAIL_BIT) ? ESP_FAIL : ESP_ERR_TIMEOUT;
    }
    s_joining = false;
    xSemaphoreGive(s_wifi_op);
    return e;
}


static esp_err_t handler_forget(httpd_req_t *r)
{
    esp_err_t ret = nvs_forget_creds();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS forget failed: %s", esp_err_to_name(ret));
    }

    s_ssid_buf[0] = '\0';
    s_ip_buf[0]   = '\0';
    s_wifi_state  = WIFI_PORTAL_STATE_SETUP_NEEDED;
    esp_wifi_disconnect();

    http_send_json(r, "{\"ok\":true}");

    // Reboot in background so the HTTP response finishes first
    http_reboot_soon("reboot");
    return ESP_OK;
}



// ─────────────────────────────────────────────────────────────────────────────
// USB-C bring-up: TinyUSB NCM driver + the lwIP netif behind it
// ─────────────────────────────────────────────────────────────────────────────
esp_err_t net_usb_init(void)
{
    // Keep the host-side MAC different from the device-side one (usb_netif_init).
    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
    };
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "tinyusb_driver_install");

    tinyusb_net_config_t net_cfg = {
        // Host-side MAC: locally-administered, host side
        .mac_addr        = {0x02, 0x02, 0x11, 0x22, 0x33, 0x01},
        .on_recv_callback = usb_recv_callback,
        .free_tx_buffer  = NULL,   // we allocate copies in usb_recv_callback
        .user_context    = NULL,
    };
    ESP_RETURN_ON_ERROR(tinyusb_net_init(TINYUSB_USBDEV_0, &net_cfg), TAG, "tinyusb_net_init");
    ESP_LOGI(TAG, "TinyUSB NCM driver initialized");

    ESP_RETURN_ON_ERROR(usb_netif_init(), TAG, "usb netif");
    ESP_LOGI(TAG, "USB netif up: " NET_USB_IP "/24, DHCP server active");
    return ESP_OK;
}

esp_err_t net_http_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/status", .method = HTTP_GET, .handler = handler_status},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = handler_scan},
        {.uri = "/api/set_creds", .method = HTTP_POST, .handler = handler_set_creds},
        {.uri = "/api/forget", .method = HTTP_POST, .handler = handler_forget},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t e = httpd_register_uri_handler(server, &routes[i]);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}
