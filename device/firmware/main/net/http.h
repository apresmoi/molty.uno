/* SPDX-License-Identifier: MIT */
/* http — the device HTTP API (port 80). See http.c for the route map. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

/* Start the server and register every route group. */
esp_err_t http_server_start(void);
/* Start the /ws/imu broadcast task (after http_server_start). */
void      http_imu_ws_start(void);

/* Helpers shared by the route files. */
esp_err_t http_send_json(httpd_req_t *r, const char *json);   /* + CORS, no-store */
__attribute__((access(write_only, 2, 3)))
int       http_recv_full(httpd_req_t *r, char *buf, int total_len);   /* whole body; <=0 on error */
bool      http_json_get_str(const char *json, const char *key, char *out, size_t out_len);
void      http_reboot_soon(const char *task_name);   /* esp_restart() ~500 ms later, after the response flushes */
