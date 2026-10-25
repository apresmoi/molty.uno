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

/* GET /api/status, GET /api/scan, POST /api/set_creds, POST /api/forget. */
esp_err_t net_http_register(httpd_handle_t server);
