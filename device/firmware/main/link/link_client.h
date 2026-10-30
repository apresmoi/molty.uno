#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"
esp_err_t link_client_init(void);
esp_err_t link_http_register(httpd_handle_t server);
esp_err_t link_send_json(const cJSON *j);
esp_err_t link_send_binary(const void *data, size_t len);
bool link_client_connected(void);
/* For the screen's status mark (sense.c): Wi-Fi has an IP, the relay welcomed this
 * socket, the relay refused the device key (4401) and the 5-minute retry is pending. */
void link_client_health(bool *wifi, bool *relay, bool *auth_failed);
