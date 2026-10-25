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
