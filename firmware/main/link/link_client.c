#include "link_client.h"
#include "link_identity.h"
#include "link_protocol.h"
#include "link_cmd.h"
#include "link_audio.h"
#include "link_pair.h"
#include "link_ask.h"
#include "self_update.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "mole_diag.h"
#include "log_upload.h"
#include "nvs.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

#include "sdkconfig.h"
#define LINK_DEFAULT_URL CONFIG_MOLTY_DEFAULT_RELAY   /* main/Kconfig.projbuild */
static const char *TAG = "agent_link";
static char s_url[LINK_URL_MAX] = LINK_DEFAULT_URL;
static SemaphoreHandle_t s_config, s_send;
static esp_websocket_client_handle_t s_ws;
static QueueHandle_t s_rxq;
static atomic_bool s_reconnect, s_forget, s_lost, s_auth, s_connected;
static atomic_uint s_close_code;
static bool s_ready;
/* Owned exclusively by the websocket callback; freed only after stop joins it. */
static uint8_t *s_rx;
static size_t s_rx_len, s_frame_offset;
static uint8_t s_opcode;
typedef struct { uint8_t *data; size_t len; uint8_t opcode; bool open; } rx_msg_t;

bool link_client_connected(void) { return atomic_load(&s_connected); }

/* ── activity log uploads (docs/AGENT-LINK.md "Activity log"; link task only) ──
 * The relay's welcome says whether it takes {"t":"log"} frames (log.max_lines; an older
 * relay sends no `log`: nothing is uploaded) and its wall clock (`now`: the time sync for
 * the lines' uptime stamps). After each welcome: the previous boot's lines once per boot,
 * then this boot's backlog since the last frame sent (at most 2 frames, newest); then new
 * lines whenever a job ends. One frame per LOG_UPLOAD_GAP_MS at most. The relay dedupes by
 * (boot, seq), so a frame lost with the socket is simply sent again. */
static int s_log_max;                 /* welcome.log.max_lines; 0 = this relay takes no uploads */
static uint32_t s_log_cur, s_prev_cur;
static bool s_log_due, s_log_first, s_prev_done, s_prev_first = true;
static int64_t s_log_last;

static void log_welcome(const cJSON *j, int64_t now_us)
{
    const cJSON *wall = cJSON_GetObjectItemCaseSensitive(j, "now");
    if (cJSON_IsNumber(wall) && wall->valuedouble > 0) mole_diag_link_sync((uint32_t)(now_us / 1000), (int64_t)wall->valuedouble);
    const cJSON *max = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "log"), "max_lines");
    s_log_max = !cJSON_IsNumber(max) || max->valuedouble < 1 ? 0
              : max->valuedouble > LOG_UPLOAD_LINES_MAX ? LOG_UPLOAD_LINES_MAX : (int)max->valuedouble;
    s_log_due = s_log_first = s_log_max > 0;
}

static void log_upload(int64_t now_us)
{
    if (mole_diag_log_take_request()) s_log_due = true;   /* a job ended */
    if (!link_client_connected() || s_log_max <= 0 || now_us - s_log_last < LOG_UPLOAD_GAP_MS * 1000LL) return;
    cJSON *f = NULL;
    uint32_t *cur = NULL, save = 0;
    bool *first = NULL;
    if (!s_prev_done) {
        cur = &s_prev_cur; save = *cur; first = &s_prev_first;
        f = mole_diag_log_prev_frame(cur, s_log_max, s_prev_first ? 2 * s_log_max : 0);
        s_prev_first = false;
        if (!f) s_prev_done = true;
    }
    if (!f && s_log_due) {
        cur = &s_log_cur; save = *cur; first = &s_log_first;
        f = mole_diag_log_frame(cur, s_log_max, s_log_first ? 2 * s_log_max : 0);
        s_log_first = false;
        if (!f) s_log_due = false;
    }
    if (!f) return;
    s_log_last = now_us;
    if (link_send_json(f) != ESP_OK) { *cur = save; *first = true; atomic_store(&s_lost, true); }   /* again after the reconnect */
    cJSON_Delete(f);
}

static esp_err_t send_data(const void *data, size_t len, bool binary)
{
    if (!s_send || !data) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_send, portMAX_DELAY);
    int n = -1;
    if (s_ws && esp_websocket_client_is_connected(s_ws)) {
        n = binary ? esp_websocket_client_send_bin(s_ws, data, (int)len, pdMS_TO_TICKS(2000))
                   : esp_websocket_client_send_text(s_ws, data, (int)len, pdMS_TO_TICKS(2000));
    }
    xSemaphoreGive(s_send);
    if (n != (int)len) mole_diag_event(true, "link.send_fail %s len=%u n=%d", binary ? "bin" : "text", (unsigned)len, n);
    return n == (int)len ? ESP_OK : ESP_FAIL;
}

esp_err_t link_send_binary(const void *data, size_t len) { return send_data(data, len, true); }
esp_err_t link_send_json(const cJSON *j)
{
    char *body = cJSON_PrintUnformatted(j);
    if (!body) return ESP_ERR_NO_MEM;
    esp_err_t e = send_data(body, strlen(body), false);
    free(body);
    return e;
}

static void websocket_event(void *arg, esp_event_base_t base, int32_t id, void *event)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *d = event;
    if (d && d->close_status_code >= LINK_CLOSE_MALFORMED && d->close_status_code <= LINK_CLOSE_FORGOTTEN) {
        atomic_store(&s_close_code, d->close_status_code);
        if (d->close_status_code == LINK_CLOSE_AUTH_FAILED) atomic_store(&s_auth, true);
    }
    if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED || id == WEBSOCKET_EVENT_ERROR) {
        if (!atomic_load(&s_lost)) {
            /* transport/TLS detail is in the ring already: esp-tls + websocket_client ESP_LOGE lines */
            mole_diag_event(true, "link.ws_%s close=%u",
                            id == WEBSOCKET_EVENT_ERROR ? "error" : id == WEBSOCKET_EVENT_CLOSED ? "closed" : "disconnected",
                            atomic_load(&s_close_code));
        }
        atomic_store(&s_lost, true); atomic_store(&s_connected, false); return;
    }
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        mole_diag_event(false, "link.ws_open");
        rx_msg_t m = {.open = true};
        if (xQueueSend(s_rxq, &m, 0) != pdTRUE) atomic_store(&s_lost, true);
        return;
    }
    if (id != WEBSOCKET_EVENT_DATA) return;
    if (d->op_code == 8) {
        if (d->data_len >= 2) {
            unsigned code = ((uint8_t)d->data_ptr[0] << 8) | (uint8_t)d->data_ptr[1];
            atomic_store(&s_close_code, code);
            if (code == LINK_CLOSE_AUTH_FAILED) atomic_store(&s_auth, true);
        }
        mole_diag_event(true, "link.close_frame code=%u", atomic_load(&s_close_code));
        atomic_store(&s_connected, false); atomic_store(&s_lost, true); return;
    }
    if (d->op_code > 2) return; /* native WebSocket ping/pong handled by component */
    if (d->payload_offset == 0) {
        s_frame_offset = 0;
        if (d->op_code == 1 || d->op_code == 2) {
            if (s_opcode) { atomic_store(&s_lost, true); return; }
            s_opcode = d->op_code; s_rx_len = 0;
        } else if (!s_opcode) { atomic_store(&s_lost, true); return; }
    }
    size_t limit = s_opcode == 1 ? LINK_TEXT_MAX : LINK_MESSAGE_MAX;
    if (d->data_len < 0 || d->payload_len < 0 || d->payload_offset < 0 ||
        (size_t)d->payload_offset != s_frame_offset ||
        (size_t)d->data_len + s_frame_offset > (size_t)d->payload_len ||
        (size_t)d->payload_len > limit - s_rx_len + s_frame_offset ||
        (size_t)d->data_len > limit - s_rx_len) {
        atomic_store(&s_lost, true); return;
    }
    if (!s_rx) s_rx = heap_caps_malloc(limit + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rx) { atomic_store(&s_lost, true); return; }
    memcpy(s_rx + s_rx_len, d->data_ptr, d->data_len);
    s_rx_len += d->data_len; s_frame_offset += d->data_len;
    if (d->fin && s_frame_offset == (size_t)d->payload_len) {
        s_rx[s_rx_len] = 0;
        rx_msg_t m = {.data = s_rx, .len = s_rx_len, .opcode = s_opcode};
        if (xQueueSend(s_rxq, &m, pdMS_TO_TICKS(750)) != pdTRUE) { free(s_rx); atomic_store(&s_lost, true); }
        s_rx = NULL; s_rx_len = 0; s_opcode = 0;
    }
}

static bool wifi_ready(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    wifi_ap_record_t ap;
    return sta && esp_netif_is_netif_up(sta) && esp_netif_get_ip_info(sta, &ip) == ESP_OK
        && ip.ip.addr && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

void link_client_health(bool *wifi, bool *relay, bool *auth_failed)
{
    *wifi = wifi_ready();
    *relay = link_client_connected();
    *auth_failed = atomic_load(&s_auth);
}

static void disconnect(const char *why)
{
    mole_diag_event(true, "link.disconnect why=%s close=%u int_free=%u", why, atomic_load(&s_close_code),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    atomic_store(&s_connected, false);
    xSemaphoreTake(s_send, portMAX_DELAY);
    esp_websocket_client_handle_t ws = s_ws; s_ws = NULL;
    xSemaphoreGive(s_send);
    if (ws) { esp_websocket_client_stop(ws); esp_websocket_client_destroy(ws); }
    free(s_rx); s_rx = NULL; s_rx_len = 0; s_opcode = 0;
    rx_msg_t m;
    while (xQueueReceive(s_rxq, &m, 0) == pdTRUE) free(m.data);
    link_audio_cancel(); link_pair_cancel(); link_ask_cancel();   /* link down: no event */
}

static esp_err_t connect_session(void)
{
    char uri[LINK_URL_MAX + 64];
    xSemaphoreTake(s_config, portMAX_DELAY);
    snprintf(uri, sizeof(uri), "%s/v1/device?id=%s", s_url, link_identity_id());
    xSemaphoreGive(s_config);
    esp_websocket_client_config_t cfg = {
        .uri = uri, .disable_auto_reconnect = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .task_stack = 6144, .buffer_size = 4096,
        /* below the fluid (2): a TLS handshake is ~1 s of pure CPU, and at the
         * component's default prio 5 it froze sim + panel for 1050 ms on every
         * (re)connect (/api/diag stall.sim gap=1050ms, websocket_task 100%). */
        .task_prio = 1,
        .network_timeout_ms = 5000, .ping_interval_sec = 20, .pingpong_timeout_sec = 40,
    };
    esp_websocket_client_handle_t ws = esp_websocket_client_init(&cfg);
    if (!ws) return ESP_ERR_NO_MEM;
    esp_err_t e = esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, websocket_event, NULL);
    if (e != ESP_OK) { esp_websocket_client_destroy(ws); return e; }
    xSemaphoreTake(s_send, portMAX_DELAY); s_ws = ws; xSemaphoreGive(s_send);
    atomic_store(&s_lost, false);
    atomic_store(&s_close_code, 0);
    mole_diag_event(false, "link.connect int_free=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return esp_websocket_client_start(ws);
}

static const char *error_code(esp_err_t e)
{
    switch (e) {
    case ESP_ERR_INVALID_ARG: return "invalid_args";
    case ESP_ERR_INVALID_STATE: return "busy";
    case ESP_ERR_NOT_FOUND: return "unknown_command";
    case ESP_ERR_NOT_SUPPORTED: return "unsupported";   /* this board lacks the hardware */
    case ESP_ERR_NO_MEM: return "oom";
    case LINK_CMD_REFUSED: return link_cmd_refusal();   /* the handler's own code */
    default: return "device_error";
    }
}

static void command(const cJSON *j)
{
    cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "id");
    cJSON *name = cJSON_GetObjectItemCaseSensitive(j, "name");
    cJSON *args = cJSON_GetObjectItemCaseSensitive(j, "args");
    if (!cJSON_IsString(id) || strlen(id->valuestring) > 128) return;
    cJSON *empty = NULL, *result = NULL;
    if (!args) { empty = cJSON_CreateObject(); args = empty; }
    int64_t t0 = esp_timer_get_time();
    esp_err_t e = cJSON_IsString(name) ? link_cmd_run(name->valuestring, args, &result) : ESP_ERR_INVALID_ARG;
    mole_diag_event(e != ESP_OK, "link.cmd %s -> %s %ums", cJSON_IsString(name) ? name->valuestring : "?",
                    e == ESP_OK ? "ok" : error_code(e), (unsigned)((esp_timer_get_time() - t0) / 1000));
    cJSON_Delete(empty);
    cJSON *res = cJSON_CreateObject();
    if (!res) { cJSON_Delete(result); return; }
    cJSON_AddStringToObject(res, "t", "res"); cJSON_AddStringToObject(res, "id", id->valuestring);
    cJSON_AddBoolToObject(res, "ok", e == ESP_OK);
    if (e == ESP_OK) cJSON_AddItemToObject(res, "result", result ? result : cJSON_CreateObject());
    else { cJSON_Delete(result); cJSON_AddStringToObject(res, "error", error_code(e)); }
    link_send_json(res); cJSON_Delete(res);
}

static void link_task(void *arg)
{
    (void)arg;
    bool session = false, forgetting = false;
    unsigned backoff = 1;
    int64_t next = 0, started = 0, last_ping = 0, last_pong = 0, forget_deadline = 0;
    char announced[9] = "";
    for (;;) {
        int64_t now = esp_timer_get_time();
        bool reset = atomic_exchange(&s_reconnect, false);
        if (!forgetting && atomic_exchange(&s_forget, false)) {
            forgetting = true;
            forget_deadline = now;
            if (link_client_connected()) {
                forget_deadline += 3000000;
                cJSON *j = link_forget_frame();
                if (j) { link_send_json(j); cJSON_Delete(j); }
            }
        }
        /* Do not stop/join the authenticated socket before its remote revoke
         * finishes. A timeout also releases an unresponsive relay after 3 s. */
        if (forgetting && (atomic_load(&s_close_code) == LINK_CLOSE_FORGOTTEN ||
                           esp_timer_get_time() >= forget_deadline)) {
            if (session) disconnect("forget");
            session = false;
            xSemaphoreTake(s_config, portMAX_DELAY);
            esp_err_t e = link_identity_forget();
            if (e == ESP_OK) strcpy(s_url, LINK_DEFAULT_URL);
            xSemaphoreGive(s_config);
            if (e != ESP_OK) ESP_LOGE(TAG, "forget failed: %s", esp_err_to_name(e));
            forgetting = false;
            reset = true;
        }
        if (forgetting) {
            /* Callbacks still process the close; queued commands cannot start
             * new work while revocation is pending. */
            rx_msg_t pending;
            int64_t left = forget_deadline - esp_timer_get_time();
            unsigned wait_ms = left > 50000 ? 50 : left > 0 ? (unsigned)(left / 1000) : 0;
            if (xQueueReceive(s_rxq, &pending, pdMS_TO_TICKS(wait_ms)) == pdTRUE) free(pending.data);
            continue;
        }
        if (reset) {
            if (session) disconnect("reset");
            session = false; next = now; backoff = 1; atomic_store(&s_auth, false);
            atomic_store(&s_close_code, 0);
        }
        xSemaphoreTake(s_config, portMAX_DELAY);
        link_pair_tick();
        xSemaphoreGive(s_config);
        link_audio_tick();
        link_ask_tick();
        xSemaphoreTake(s_config, portMAX_DELAY);
        link_identity_tick();
        bool rotated = strcmp(announced, link_identity_code()) != 0;
        cJSON *code = link_client_connected() && rotated ? link_code_frame(link_identity_code()) : NULL;
        char current_code[9]; strcpy(current_code, link_identity_code());
        xSemaphoreGive(s_config);
        if (code) {
            if (link_send_json(code) == ESP_OK) strcpy(announced, current_code);
            else atomic_store(&s_lost, true);
            cJSON_Delete(code);
        }
        bool online = wifi_ready();
        if (session && (!online || atomic_load(&s_lost) ||
            (!link_client_connected() && now - started > 15000000) ||
            (link_client_connected() && now - last_pong > 45000000))) {
            disconnect(!online ? "wifi" : atomic_load(&s_lost) ? "lost" :
                       !link_client_connected() ? "no_welcome_15s" : "pong_timeout_45s");
            session = false;
            next = esp_timer_get_time() + (int64_t)link_retry_seconds(
                atomic_load(&s_auth) ? LINK_CLOSE_AUTH_FAILED : atomic_load(&s_close_code), backoff) * 1000000;
            backoff = backoff < 32 ? backoff * 2 : 60;
        }
        if (!session && online && now >= next) {
            session = true; started = now;
            if (connect_session() != ESP_OK) atomic_store(&s_lost, true);
        }
        rx_msg_t m;
        if (xQueueReceive(s_rxq, &m, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (m.open) {
                xSemaphoreTake(s_config, portMAX_DELAY);
                cJSON *hello = link_identity_hello();
                strcpy(announced, link_identity_code());
                xSemaphoreGive(s_config);
                if (!hello || link_send_json(hello) != ESP_OK) atomic_store(&s_lost, true);
                cJSON_Delete(hello);
            } else if (m.opcode == 2) {
                if (link_client_connected()) link_audio_receive(m.data, m.len);
            } else if (m.opcode == 1 && !memchr(m.data, 0, m.len)) {
                cJSON *j = cJSON_ParseWithOpts((char *)m.data, NULL, true);
                cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "t");
                if (cJSON_IsString(t)) {
                    if (!strcmp(t->valuestring, "welcome")) {
                        atomic_store(&s_connected, true); atomic_store(&s_auth, false);
                        mole_diag_event(false, "link.welcome after %ums", (unsigned)((now - started) / 1000));
                        self_update_welcomed();   /* a freshly updated image: valid now (update_core.h) */
                        log_welcome(j, now);
                        backoff = 1; last_pong = now; last_ping = now;
                    } else if (!strcmp(t->valuestring, "pong")) last_pong = now;
                    else if (!strcmp(t->valuestring, "ping")) {
                        cJSON *pong = cJSON_CreateObject();
                        cJSON_AddStringToObject(pong, "t", "pong"); link_send_json(pong); cJSON_Delete(pong);
                    } else if (link_client_connected() && !strcmp(t->valuestring, "cmd")) command(j);
                    else if (link_client_connected() && !strcmp(t->valuestring, "pair.request")) {
                        xSemaphoreTake(s_config, portMAX_DELAY);
                        link_pair_request(j);
                        xSemaphoreGive(s_config);
                    }
                }
                cJSON_Delete(j);
            }
            free(m.data);
        }
        log_upload(esp_timer_get_time());
        if (link_client_connected() && now - last_ping >= 20000000) {
            cJSON *ping = cJSON_CreateObject();
            cJSON_AddStringToObject(ping, "t", "ping");
            if (link_send_json(ping) != ESP_OK) atomic_store(&s_lost, true);
            cJSON_Delete(ping); last_ping = now;
        }
    }
}

/* Local configuration never exposes device_key. Link state is owned by link_task. */
static esp_err_t reply(httpd_req_t *r, cJSON *j)
{
    char *body = cJSON_PrintUnformatted(j); cJSON_Delete(j);
    if (!body) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(r, "application/json");
    esp_err_t e = httpd_resp_sendstr(r, body); free(body); return e;
}

static esp_err_t http_get(httpd_req_t *r)
{
    if (!s_ready) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "link unavailable");
    cJSON *j = cJSON_CreateObject();
    xSemaphoreTake(s_config, portMAX_DELAY);
    cJSON_AddStringToObject(j, "device_id", link_identity_id());
    cJSON_AddStringToObject(j, "link_code", link_identity_code());
    cJSON_AddStringToObject(j, "relay_url", s_url);
    link_pair_status(j);
    xSemaphoreGive(s_config);
    cJSON_AddBoolToObject(j, "connected", link_client_connected());
    cJSON_AddBoolToObject(j, "auth_failed", atomic_load(&s_auth));
    cJSON_AddNumberToObject(j, "close_code", atomic_load(&s_close_code));
    return reply(r, j);
}

static esp_err_t http_set(httpd_req_t *r)
{
    if (!s_ready) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "link unavailable");
    if (r->content_len <= 0 || r->content_len >= LINK_URL_MAX + 64) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "invalid body");
    char body[LINK_URL_MAX + 64]; int got = 0;
    while (got < r->content_len) {
        int n = httpd_req_recv(r, body + got, r->content_len - got);
        if (n <= 0) return ESP_FAIL;
        got += n;
    }
    body[got] = 0;
    cJSON *j = cJSON_ParseWithOpts(body, NULL, true);
    cJSON *url = cJSON_GetObjectItemCaseSensitive(j, "relay_url");
    if (!cJSON_IsString(url) || !link_url_valid(url->valuestring)) {
        cJSON_Delete(j); return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "expected ws or wss relay origin");
    }
    xSemaphoreTake(s_config, portMAX_DELAY);
    nvs_handle_t n;
    esp_err_t e = nvs_open("agent_link", NVS_READWRITE, &n);
    if (e == ESP_OK) {
        e = nvs_set_str(n, "link_url", url->valuestring);
        if (e == ESP_OK) e = nvs_commit(n);
        nvs_close(n);
    }
    if (e == ESP_OK) strcpy(s_url, url->valuestring);
    xSemaphoreGive(s_config); cJSON_Delete(j);
    if (e != ESP_OK) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
    atomic_store(&s_reconnect, true);
    return http_get(r);
}

static esp_err_t http_forget(httpd_req_t *r)
{
    if (!s_ready) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "link unavailable");
    atomic_store(&s_forget, true);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_status(r, "202 Accepted");
    return httpd_resp_sendstr(r, "{\"ok\":true,\"pending\":true}");
}

esp_err_t link_http_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/api/link", .method = HTTP_GET, .handler = http_get},
        {.uri = "/api/link", .method = HTTP_POST, .handler = http_set},
        {.uri = "/api/link/forget", .method = HTTP_POST, .handler = http_forget},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t e = httpd_register_uri_handler(server, &routes[i]);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}

esp_err_t link_client_init(void)
{
    s_config = xSemaphoreCreateMutex(); s_send = xSemaphoreCreateMutex();
    s_rxq = xQueueCreate(8, sizeof(rx_msg_t));
    if (!s_config || !s_send || !s_rxq) return ESP_ERR_NO_MEM;
    esp_err_t e = link_identity_init();
    if (e != ESP_OK) return e;
    nvs_handle_t n;
    e = nvs_open("agent_link", NVS_READONLY, &n);
    if (e != ESP_OK) return e;
    size_t len = sizeof(s_url);
    e = nvs_get_str(n, "link_url", s_url, &len); nvs_close(n);
    if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) return e;
    if (!link_url_valid(s_url)) return ESP_ERR_INVALID_ARG;
    e = link_pair_init(); if (e != ESP_OK) return e;
    e = link_audio_init(); if (e != ESP_OK) return e;
    link_cmd_init();
    /* The component's debug trace prints wire payloads, including hello. */
    esp_log_level_set("websocket_client", ESP_LOG_WARN);
    s_ready = true;
    if (xTaskCreate(link_task, "agent_link", 8192, NULL, 3, NULL) != pdPASS) {
        s_ready = false; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
