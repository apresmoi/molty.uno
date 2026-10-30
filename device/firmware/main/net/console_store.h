/* SPDX-License-Identifier: MIT */
/* console_store — mounts /spiffs (also used by the creature file and the diag
 * log) and serves the console page at "/". See console_store.c. */
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

/* Early in boot, after NVS and before USB/Wi-Fi/httpd: format recovery, mount,
 * restore the stored console page into PSRAM. */
void console_store_init(void);

/* GET /, POST /dev/html, POST /dev/reset_html. */
esp_err_t console_http_register(httpd_handle_t server);

/* For /api/info. */
const char *console_spiffs_state(void);
bool console_html_recovered(void);
