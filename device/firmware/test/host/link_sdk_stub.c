#include "link_sdk_stub.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int64_t sdk_now;
void (*sdk_wait)(void);
unsigned sdk_random, sdk_commits, sdk_erases;
char sdk_id[64], sdk_url[384], sdk_state[512], sdk_text[4097], sdk_mood[16];
const char *sdk_saved_state = "{\"state\":\"blob\"}";
uint8_t sdk_key[32];
size_t sdk_key_len, sdk_written;
int sdk_gpio[7] = {1,1,1,1,1,1,1};
unsigned sdk_drained;
esp_err_t sdk_play_error;
cJSON *sdk_sent;

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out) { (void)mode; assert(!strcmp(name,"agent_link")); *out=1; return ESP_OK; }
void nvs_close(nvs_handle_t n) { (void)n; }
esp_err_t nvs_get_blob(nvs_handle_t n, const char *key, void *out, size_t *len) {
    (void)n; assert(!strcmp(key,"device_key"));
    if (!sdk_key_len) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < sdk_key_len) return ESP_ERR_INVALID_SIZE;
    memcpy(out,sdk_key,sdk_key_len); *len=sdk_key_len; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t n, const char *key, const void *data, size_t len) {
    (void)n; assert(!strcmp(key,"device_key") && len==32); memcpy(sdk_key,data,len); sdk_key_len=len; return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t n, const char *key, char *out, size_t *len) {
    (void)n; const char *value=!strcmp(key,"device_id") ? sdk_id : sdk_url;
    if (!value[0]) return ESP_ERR_NVS_NOT_FOUND;
    size_t size=strlen(value)+1; if (*len<size) return ESP_ERR_INVALID_SIZE;
    memcpy(out,value,size); *len=size; return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t n, const char *key, const char *value) {
    (void)n; strcpy(!strcmp(key,"device_id") ? sdk_id : sdk_url,value); return ESP_OK;
}
esp_err_t nvs_erase_all(nvs_handle_t n) { (void)n; sdk_erases++; sdk_id[0]=sdk_url[0]=0; sdk_key_len=0; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t n) { (void)n; sdk_commits++; return ESP_OK; }
uint32_t esp_random(void) { return sdk_random++; }
void esp_fill_random(void *p, size_t n) { for (size_t i=0;i<n;i++) ((uint8_t *)p)[i]=esp_random(); }
int64_t esp_timer_get_time(void) { return sdk_now; }
const esp_app_desc_t *esp_app_get_description(void) { static const esp_app_desc_t d={"test"}; return &d; }
/* The platform base64 implementation is outside the unit under test. */
int mbedtls_base64_encode(unsigned char *out, size_t cap, size_t *len, const unsigned char *in, size_t size) {
    (void)in; assert(cap>=45 && size==32); memset(out,'A',43); out[43]='='; *len=44; return 0;
}
const char *esp_err_to_name(esp_err_t e) { (void)e; return "mock_error"; }
int sdk_locks; bool sdk_state_locked;
void (*sdk_on_lock)(void), (*sdk_on_unlock)(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t s, unsigned t) { assert(s); (void)t; sdk_locks++; if (sdk_on_lock) sdk_on_lock(); return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t s) { assert(s && sdk_locks>0); if (sdk_on_unlock) sdk_on_unlock(); sdk_locks--; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }
struct sdk_queue { size_t count, cap, item; unsigned char data[]; };
QueueHandle_t xQueueCreate(unsigned n, size_t item) {
    QueueHandle_t q=calloc(1,sizeof(*q)+n*item); assert(q); q->cap=n; q->item=item; return q;
}
int xQueueSend(QueueHandle_t q, const void *p, unsigned t) {
    (void)t; if(q->count==q->cap) return 0; memcpy(q->data+q->count++*q->item,p,q->item); return pdTRUE;
}
int xQueueReceive(QueueHandle_t q, void *p, unsigned t) {
    if(!q->count && t && sdk_wait) { sdk_now+=(int64_t)t*1000; sdk_wait(); }
    if(!q->count) return 0;
    memcpy(p,q->data,q->item); q->count--; memmove(q->data,q->data+q->item,q->count*q->item); return pdTRUE;
}
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned prio, void *out) {
    (void)fn;(void)name;(void)stack;(void)arg;(void)prio;(void)out; return pdPASS;
}
struct sdk_ring { size_t cap, len; unsigned char data[]; };
RingbufHandle_t xRingbufferCreateWithCaps(size_t size, int type, int caps) {
    (void)type;(void)caps; RingbufHandle_t r=calloc(1,sizeof(*r)+size); assert(r); r->cap=size; return r;
}
void *xRingbufferReceiveUpTo(RingbufHandle_t r, size_t *n, unsigned ticks, size_t max) {
    (void)ticks; if (!r->len) return NULL; *n=r->len>max?max:r->len;
    void *p=malloc(*n); assert(p); memcpy(p,r->data,*n); r->len-=*n; memmove(r->data,r->data+*n,r->len); return p;
}
void *xRingbufferReceive(RingbufHandle_t r, size_t *n, unsigned t) { return xRingbufferReceiveUpTo(r,n,t,SIZE_MAX); }
void vRingbufferReturnItem(RingbufHandle_t r, void *p) { (void)r; free(p); }
int xRingbufferSend(RingbufHandle_t r, const void *p, size_t n, unsigned t) {
    (void)t; if(n>r->cap-r->len) return 0; memcpy(r->data+r->len,p,n); r->len+=n; return pdTRUE;
}
/* live blocks through the firmware's allocators (heap_caps_* here, cJSON via sdk_count_cjson):
 * the soak tests (link_audio_pair_test.c test_soak) hold this flat across many turns */
long sdk_heap_live;
void *heap_caps_malloc(size_t n, int caps) { (void)caps; void *p=malloc(n); if (p) sdk_heap_live++; return p; }
void heap_caps_free(void *p) { if (p) sdk_heap_live--; free(p); }
static void *count_malloc(size_t n) { void *p=malloc(n); if (p) sdk_heap_live++; return p; }
static void count_free(void *p) { if (p) sdk_heap_live--; free(p); }
void sdk_count_cjson(void) { cJSON_Hooks h={.malloc_fn=count_malloc,.free_fn=count_free}; cJSON_InitHooks(&h); }
size_t heap_caps_get_free_size(int caps) { (void)caps; return 100000; }
int xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned prio, void *out, int core, int caps) {
    (void)core; (void)caps; return xTaskCreate(fn, name, stack, arg, prio, out);
}
void mole_diag_event(bool important, const char *fmt, ...) { (void)important; (void)fmt; }
unsigned sdk_job_begins, sdk_job_ends;
char sdk_job_last_end[160];
bool sdk_log_request;
uint32_t sdk_sync_up;
int64_t sdk_sync_wall;
cJSON *(*sdk_log_frame)(bool prev, uint32_t *cursor, int max_lines, int backlog);
void mole_diag_job_begin(const char *job) { (void)job; sdk_job_begins++; }
void mole_diag_job_end(const char *job) { sdk_job_ends++; snprintf(sdk_job_last_end, sizeof(sdk_job_last_end), "%s", job); sdk_log_request = true; }
bool mole_diag_log_take_request(void) { bool r = sdk_log_request; sdk_log_request = false; return r; }
void mole_diag_link_sync(uint32_t up_ms, int64_t wall_ms) { sdk_sync_up = up_ms; sdk_sync_wall = wall_ms; }
cJSON *mole_diag_log_frame(uint32_t *c, int m, int b) { return sdk_log_frame ? sdk_log_frame(false, c, m, b) : NULL; }
cJSON *mole_diag_log_prev_frame(uint32_t *c, int m, int b) { return sdk_log_frame ? sdk_log_frame(true, c, m, b) : NULL; }
esp_err_t sdk_rx_error;
unsigned sdk_rx_chunks;
void (*sdk_rx_fill)(int16_t *buf, size_t n, unsigned chunk);
bool sdk_rx_open;
esp_err_t molty_mic_stream_start(uint32_t rate, uint32_t ms, uint8_t gain) {
    assert(rate==16000 && ms>0 && gain>0 && !sdk_rx_open); sdk_rx_open=true; return ESP_OK;
}
esp_err_t molty_mic_stream_read(int16_t *buf, size_t n, size_t *got) {
    assert(sdk_rx_open); if (sdk_rx_error) { *got=0; return sdk_rx_error; }
    if (sdk_rx_fill) sdk_rx_fill(buf,n,sdk_rx_chunks); else memset(buf,0,n*2);
    *got=n; sdk_rx_chunks++; return ESP_OK;
}
uint8_t sdk_mic_gain = 8;
uint8_t molty_mic_gain(void) { return sdk_mic_gain; }
/* a task delay: the clock moves on */
void vTaskDelay(unsigned ticks) { sdk_now+=(int64_t)ticks*1000; }
void molty_mic_stream_stop(void) { assert(sdk_rx_open); sdk_rx_open=false; }
uint32_t sdk_caps = 0xFFFFFFFFu;
bool molty_board_has(uint32_t caps) { return caps && (sdk_caps & caps) == caps; }
const char *molty_board_id(void) { return "test-board"; }
bool molty_button_pressed(molty_button_t b) {
    uint32_t need = b == MOLTY_BTN_ACCEPT ? MOLTY_CAP_BTN_ACCEPT : MOLTY_CAP_BTN_REJECT;
    return (sdk_caps & need) && sdk_gpio[b == MOLTY_BTN_ACCEPT ? 0 : 6] == 0;
}
esp_err_t mole_creature_select(const char *name) { assert(name); return sdk_select_error; }
const char *mole_creature_current(void) { return "fluid"; }
esp_err_t mole_creature_state_get(char **out) { *out=strdup(sdk_saved_state); return ESP_OK; }
esp_err_t sdk_state_error, sdk_select_error;
esp_err_t mole_creature_state_set(const char *json) { assert(strlen(json)<sizeof(sdk_state)); if(sdk_state_error) return sdk_state_error; strcpy(sdk_state,json); sdk_state_locked=sdk_locks>0; return ESP_OK; }
static size_t sdk_stream_samples;   /* since stream_begin; the DAC lags by a 1440-sample DMA ring */
unsigned sdk_speaker_begins, sdk_speaker_ends;
esp_err_t sdk_speaker_begin_error;
void (*sdk_on_speaker_write)(const int16_t *pcm, size_t n);
esp_err_t molty_speaker_stream_begin(uint32_t rate) {
    assert(rate==16000); if (sdk_speaker_begin_error) return sdk_speaker_begin_error;
    assert(sdk_speaker_begins==sdk_speaker_ends);   /* one owner at a time */
    sdk_speaker_begins++; sdk_stream_samples=0; return ESP_OK;
}
esp_err_t molty_speaker_stream_write(const int16_t *pcm, size_t n) {
    assert(pcm && sdk_speaker_begins==sdk_speaker_ends+1);
    sdk_written+=n*2; sdk_stream_samples+=n;
    if (sdk_on_speaker_write) sdk_on_speaker_write(pcm,n);
    return sdk_play_error;
}
uint32_t molty_speaker_stream_played(void) { return sdk_stream_samples>1440 ? (uint32_t)(sdk_stream_samples-1440) : 0; }
esp_err_t molty_speaker_stream_drain(void) { sdk_drained++; return sdk_play_error; }
void molty_speaker_stream_end(void) { sdk_speaker_ends++; }
void esp_crt_bundle_attach(void) {}
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) { (void)key; return (void *)1; }
bool esp_netif_is_netif_up(esp_netif_t *net) { (void)net; return true; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *net, esp_netif_ip_info_t *ip) { (void)net; ip->ip.addr=1; return ESP_OK; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap) { ap->rssi=-40; return ESP_OK; }
void esp_log_level_set(const char *tag, int level) { (void)tag; (void)level; }
esp_err_t httpd_resp_set_type(httpd_req_t *req,const char *type) { (void)req; (void)type; return ESP_OK; }
esp_err_t httpd_resp_set_status(httpd_req_t *req,const char *status) { (void)req; (void)status; return ESP_OK; }
esp_err_t httpd_resp_sendstr(httpd_req_t *req,const char *body) { (void)req; cJSON_Delete(sdk_sent); sdk_sent=cJSON_Parse(body); return ESP_OK; }
esp_err_t httpd_resp_send_err(httpd_req_t *req,int code,const char *body) { (void)req;(void)code;(void)body; return ESP_FAIL; }
int httpd_req_recv(httpd_req_t *req,char *out,size_t n) { (void)req;(void)out;(void)n; return -1; }
esp_err_t httpd_register_uri_handler(httpd_handle_t h,const httpd_uri_t *uri) { (void)h;(void)uri; return ESP_OK; }
