/* Minimal, deterministic SDK boundary for compiling the actual link sources. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
typedef int esp_err_t;
#define ESP_OK 0
#define CONFIG_MOLTY_DEFAULT_RELAY "wss://relay.invalid"
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_NOT_SUPPORTED 4
#define ESP_ERR_INVALID_SIZE 5
#define ESP_ERR_TIMEOUT 6
#define ESP_ERR_NVS_NOT_FOUND 7
#define ESP_ERR_NOT_FOUND 8
#define NVS_READWRITE 1
#define NVS_READONLY 0
typedef int nvs_handle_t;
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *);
esp_err_t nvs_erase_all(nvs_handle_t);
esp_err_t nvs_commit(nvs_handle_t);
uint32_t esp_random(void);
void esp_fill_random(void *, size_t);
int64_t esp_timer_get_time(void);
typedef struct { char version[32]; } esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
int mbedtls_base64_encode(unsigned char *, size_t, size_t *, const unsigned char *, size_t);
const char *esp_err_to_name(esp_err_t);
typedef void *SemaphoreHandle_t;
typedef struct sdk_queue *QueueHandle_t;
typedef struct sdk_ring *RingbufHandle_t;
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define pdPASS 1
#define RINGBUF_TYPE_BYTEBUF 0
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, unsigned);
int xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
#define configASSERT(x) do { if (!(x)) abort(); } while (0)
QueueHandle_t xQueueCreate(unsigned, size_t);
int xQueueSend(QueueHandle_t, const void *, unsigned);
int xQueueReceive(QueueHandle_t, void *, unsigned);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, void *);
RingbufHandle_t xRingbufferCreateWithCaps(size_t, int, int);
void *xRingbufferReceive(RingbufHandle_t, size_t *, unsigned);
void *xRingbufferReceiveUpTo(RingbufHandle_t, size_t *, unsigned, size_t);
void vRingbufferReturnItem(RingbufHandle_t, void *);
int xRingbufferSend(RingbufHandle_t, const void *, size_t, unsigned);
void *heap_caps_malloc(size_t, int);
/* molty_board.h: the board seam. sdk_caps = what the stub board has (all by default);
 * the accept / reject buttons read sdk_gpio[0] / sdk_gpio[6] (BOOT / PWR, active low). */
#define MOLTY_CAP_DISPLAY     (1u << 0)
#define MOLTY_CAP_TOUCH       (1u << 1)
#define MOLTY_CAP_IMU         (1u << 2)
#define MOLTY_CAP_MIC         (1u << 3)
#define MOLTY_CAP_SPEAKER     (1u << 4)
#define MOLTY_CAP_BTN_ACCEPT  (1u << 5)
#define MOLTY_CAP_BTN_REJECT  (1u << 6)
typedef enum { MOLTY_BTN_ACCEPT = 0, MOLTY_BTN_REJECT = 1 } molty_button_t;
extern uint32_t sdk_caps;
bool molty_board_has(uint32_t caps);
const char *molty_board_id(void);
bool molty_button_pressed(molty_button_t b);
esp_err_t mole_creature_select(const char *);
const char *mole_creature_current(void);
esp_err_t mole_creature_state_get(char **);
esp_err_t mole_creature_state_set(const char *);
esp_err_t molty_speaker_stream_begin(uint32_t);
esp_err_t molty_speaker_stream_write(const int16_t *, size_t);
esp_err_t molty_speaker_stream_drain(void);
void molty_speaker_stream_end(void);
uint32_t molty_speaker_stream_played(void);
esp_err_t molty_mic_stream_start(uint32_t, uint32_t, uint8_t);
esp_err_t molty_mic_stream_read(int16_t *, size_t, size_t *);
void molty_mic_stream_stop(void);
uint8_t molty_mic_gain(void);
extern uint8_t sdk_mic_gain;
void vTaskDelay(unsigned ticks);
extern esp_err_t sdk_rx_error;
extern unsigned sdk_rx_chunks;
extern bool sdk_rx_open;   /* the mic stream is open */
/* mic samples for read #chunk (NULL: digital silence) */
extern void (*sdk_rx_fill)(int16_t *buf, size_t n, unsigned chunk);
#define tskNO_AFFINITY 0x7fffffff
int xTaskCreatePinnedToCoreWithCaps(void (*)(void *), const char *, unsigned, void *, unsigned, void *, int, int);
void heap_caps_free(void *);
size_t heap_caps_get_free_size(int);
#define MALLOC_CAP_INTERNAL 4
void mole_diag_event(bool, const char *, ...);
/* activity log (mole_diag.h): job tags counted; uploads served by sdk_log_frame (NULL: none) */
void mole_diag_job_begin(const char *job);
void mole_diag_job_end(const char *job);
bool mole_diag_log_take_request(void);
void mole_diag_link_sync(uint32_t up_ms, int64_t wall_ms);
cJSON *mole_diag_log_frame(uint32_t *cursor, int max_lines, int backlog);
cJSON *mole_diag_log_prev_frame(uint32_t *cursor, int max_lines, int backlog);
extern unsigned sdk_job_begins, sdk_job_ends;
extern char sdk_job_last_end[160];
extern bool sdk_log_request;
extern uint32_t sdk_sync_up;
extern int64_t sdk_sync_wall;
extern cJSON *(*sdk_log_frame)(bool prev, uint32_t *cursor, int max_lines, int backlog);
typedef void *esp_event_base_t;
typedef struct { int close_status_code, op_code, data_len, payload_len, payload_offset; char *data_ptr; bool fin; } esp_websocket_event_data_t;
enum { WEBSOCKET_EVENT_DISCONNECTED, WEBSOCKET_EVENT_CLOSED, WEBSOCKET_EVENT_ERROR,
       WEBSOCKET_EVENT_CONNECTED, WEBSOCKET_EVENT_DATA, WEBSOCKET_EVENT_ANY };
typedef struct sdk_ws *esp_websocket_client_handle_t;
typedef struct {
    const char *uri;
    bool disable_auto_reconnect;
    void (*crt_bundle_attach)(void);
    int task_stack, buffer_size, task_prio, network_timeout_ms, ping_interval_sec, pingpong_timeout_sec;
} esp_websocket_client_config_t;
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *);
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t, int, void (*)(void *, esp_event_base_t, int32_t, void *), void *);
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t);
esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t);
esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t);
bool esp_websocket_client_is_connected(esp_websocket_client_handle_t);
int esp_websocket_client_send_text(esp_websocket_client_handle_t, const char *, int, unsigned);
int esp_websocket_client_send_bin(esp_websocket_client_handle_t, const void *, int, unsigned);
void esp_crt_bundle_attach(void);
typedef void *esp_netif_t;
typedef struct { struct { unsigned addr; } ip; } esp_netif_ip_info_t;
typedef struct { int rssi; } wifi_ap_record_t;
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *);
bool esp_netif_is_netif_up(esp_netif_t *);
esp_err_t esp_netif_get_ip_info(esp_netif_t *, esp_netif_ip_info_t *);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *);
#define ESP_LOG_WARN 1
#define ESP_LOGE(tag, fmt, ...) ((void)(tag))
void esp_log_level_set(const char *, int);
typedef void *httpd_handle_t;
typedef struct { int content_len; } httpd_req_t;
typedef struct { const char *uri; int method; esp_err_t (*handler)(httpd_req_t *); } httpd_uri_t;
#define HTTP_GET 0
#define HTTP_POST 1
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define HTTPD_400_BAD_REQUEST 400
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_status(httpd_req_t *, const char *);
esp_err_t httpd_resp_sendstr(httpd_req_t *, const char *);
esp_err_t httpd_resp_send_err(httpd_req_t *, int, const char *);
int httpd_req_recv(httpd_req_t *, char *, size_t);
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *);

extern int64_t sdk_now;
extern void (*sdk_wait)(void);
extern unsigned sdk_random, sdk_commits, sdk_erases;
/* mutexes held right now; whether the last state set ran with one held */
extern int sdk_locks; extern bool sdk_state_locked;
/* run as a mutex is acquired / about to be released (critical-section boundaries) */
extern void (*sdk_on_lock)(void), (*sdk_on_unlock)(void);
extern char sdk_id[64], sdk_url[384], sdk_state[512], sdk_text[4097], sdk_mood[16];
extern const char *sdk_saved_state;   /* what mole_creature_state_get reports */
extern uint8_t sdk_key[32];
extern size_t sdk_key_len, sdk_written;
extern int sdk_gpio[7];
extern unsigned sdk_drained;
/* speaker streams opened / closed; begin refuses with this (the speaker is someone else's);
 * called on every write with what is written */
extern unsigned sdk_speaker_begins, sdk_speaker_ends;
extern esp_err_t sdk_speaker_begin_error;
extern void (*sdk_on_speaker_write)(const int16_t *pcm, size_t n);
extern esp_err_t sdk_play_error;
/* the creature refuses a state set / a creature switch with this (ESP_OK: accepts) */
extern esp_err_t sdk_state_error, sdk_select_error;
extern cJSON *sdk_sent;
