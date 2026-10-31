/* SPDX-License-Identifier: MIT */
/* network — USB-C NCM + Wi-Fi STA + mDNS + the Wi-Fi setup API. See network.c. */
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

/* The device's address on the USB-C (NCM) link. */
#define NET_USB_IP "10.13.37.1"

/* After esp_netif_init + the default event loop. Order: usb, mdns, wifi. */
esp_err_t net_usb_init(void);
void      net_mdns_start(void);
esp_err_t net_wifi_init(void);   /* STA; connects in the background if creds are saved */

/* Wi-Fi operations shared by the HTTP API below and the serial setup (improv_serial.c).
 * Joining and scanning are serialized: two callers never drive the radio at once. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { char ssid[33]; int8_t rssi; bool open; } net_ap_t;

/** True once net_wifi_init() has run (the radio exists). */
bool      net_wifi_ready(void);
/** True while the station is associated with an AP (STA_CONNECTED .. STA_DISCONNECTED; network.c
 *  listens from before esp_wifi_start, so a connect made before a later listener is not missed). */
bool      net_wifi_associated(void);
/** Join `ssid` and, once connected, save it to NVS (namespace "wifi"). Blocks up to
 *  timeout_ms. ESP_OK connected; ESP_FAIL the AP refused / disconnected; ESP_ERR_TIMEOUT. */
esp_err_t net_wifi_join(const char *ssid, const char *pass, uint32_t timeout_ms);
/** Scan (blocking); up to `max` access points, strongest first. Returns the count or -1. */
int       net_wifi_scan(net_ap_t *out, int max);
/** True when connected; the station's IPv4 address into ip. */
bool      net_wifi_ip(char *ip, size_t len);

/* GET /api/status, GET /api/scan, POST /api/set_creds, POST /api/forget. */
esp_err_t net_http_register(httpd_handle_t server);
