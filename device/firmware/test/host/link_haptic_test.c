/* haptic.c on the host: the link's buzz and POST /api/haptic (their args, errors and answers), and that
 * nothing reaches the motor while the mic is open or without a motor. The PWM and timer are stubs. */
#include "link_sdk_stub.h"
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_TIMER_0 0
#define LEDC_CHANNEL_0 0
#define LEDC_AUTO_CLK 0
#define GPIO_MODE_OUTPUT 1
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
typedef struct { int speed_mode, duty_resolution, timer_num, freq_hz, clk_cfg; } ledc_timer_config_t;
typedef struct { int gpio_num, speed_mode, channel, timer_sel, duty, hpoint; } ledc_channel_config_t;
typedef struct { void (*callback)(void *); const char *name; } esp_timer_create_args_t;
typedef int *esp_timer_handle_t;
esp_err_t ledc_timer_config(const ledc_timer_config_t *c);
esp_err_t ledc_channel_config(const ledc_channel_config_t *c);
esp_err_t ledc_set_duty(int mode, int chan, uint32_t duty);
esp_err_t ledc_update_duty(int mode, int chan);
esp_err_t ledc_stop(int mode, int chan, uint32_t idle);
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h);
esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t us);
esp_err_t esp_timer_stop(esp_timer_handle_t h);
bool esp_timer_is_active(esp_timer_handle_t h);
int gpio_set_level(int pin, int level);
int gpio_set_direction(int pin, int mode);
int molty_haptic_gpio(void);
bool molty_mic_stream_active(void);
esp_err_t http_send_json(httpd_req_t *r, const char *json);
int http_recv_full(httpd_req_t *r, char *buf, int total_len);
#include "../../main/haptic/haptic.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* the board: a motor on GPIO17 or none; the mic streaming or not */
static int pin = -1; static bool mic_open;
int molty_haptic_gpio(void) { return pin; }
bool molty_mic_stream_active(void) { return mic_open; }
/* the motor: every duty it was driven at (ons), the last one */
static unsigned ons; static uint32_t duty;
esp_err_t ledc_timer_config(const ledc_timer_config_t *c) { (void)c; return ESP_OK; }
esp_err_t ledc_channel_config(const ledc_channel_config_t *c) { (void)c; return ESP_OK; }
esp_err_t ledc_set_duty(int m, int c, uint32_t d) { (void)m; (void)c; duty = d; ons++; return ESP_OK; }
esp_err_t ledc_update_duty(int m, int c) { (void)m; (void)c; return ESP_OK; }
esp_err_t ledc_stop(int m, int c, uint32_t i) { (void)m; (void)c; (void)i; return ESP_OK; }
static int timer;
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h) { (void)a; *h = &timer; return ESP_OK; }
static uint64_t armed_us;   /* the last segment's length */
esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t us) { (void)h; armed_us = us; return ESP_OK; }
esp_err_t esp_timer_stop(esp_timer_handle_t h) { (void)h; return ESP_OK; }
bool esp_timer_is_active(esp_timer_handle_t h) { (void)h; return false; }
int gpio_set_level(int p, int l) { (void)p; (void)l; return 0; }
int gpio_set_direction(int p, int m) { (void)p; (void)m; return 0; }
/* the link's refusal codes (link_cmd.c) */
static const char *refused;
esp_err_t link_cmd_refuse(const char *code) { refused = code; return LINK_CMD_REFUSED; }
/* HTTP: the body posted, the answer sent */
static const char *posted; static char answer[160];
int http_recv_full(httpd_req_t *r, char *buf, int n) { (void)r; memcpy(buf, posted, (size_t)n); return n; }
esp_err_t http_send_json(httpd_req_t *r, const char *json) { (void)r; snprintf(answer, sizeof answer, "%s", json); return ESP_OK; }

/* the link's buzz: its result as text in out (or ""), the refusal code in refused */
static char out[160];
static esp_err_t buzz(const char *args) {
    cJSON *a = cJSON_Parse(args), *r = NULL; assert(a);
    refused = NULL; out[0] = 0;
    esp_err_t e = haptic_cmd(a, &r);
    if (r) { char *t = cJSON_PrintUnformatted(r); snprintf(out, sizeof out, "%s", t); cJSON_free(t); }
    assert((e == ESP_OK) == (r != NULL));
    cJSON_Delete(a); cJSON_Delete(r);
    return e;
}
static const char *post(const char *body) {
    httpd_req_t r = { .content_len = (int)strlen(body) };
    posted = body; answer[0] = 0;
    assert(http_post(&r) == ESP_OK);
    return answer;
}

int main(void) {
    /* no motor: refused no_motor, nothing driven, no cue */
    haptic_init();
    assert(!haptic_available());
    assert(buzz("{\"pattern\":\"tap\"}") == LINK_CMD_REFUSED && !strcmp(refused, "no_motor"));
    assert(!strcmp(post("{\"ms\":100}"), "{\"ok\":false,\"error\":\"no_motor\"}"));
    assert(haptic_cue("double") == ESP_ERR_NOT_SUPPORTED && !ons);

    pin = 17; haptic_init();
    assert(haptic_available());
    /* a named pattern: what started, at full strength unless asked */
    assert(buzz("{\"pattern\":\"double\"}") == ESP_OK && !strcmp(out, "{\"ms\":210,\"strength\":100,\"pattern\":[60,90,60]}"));
    assert(ons == 1 && duty == 1024);
    assert(buzz("{\"pattern\":\"tick\",\"strength\":30}") == ESP_OK && !strcmp(out, "{\"ms\":30,\"strength\":30,\"pattern\":[30]}"));
    assert(buzz("{\"ms\":300}") == ESP_OK && !strcmp(out, "{\"ms\":300,\"strength\":100}"));
    assert(buzz("{\"pattern\":[80,60,80],\"strength\":60}") == ESP_OK && !strcmp(out, "{\"ms\":220,\"strength\":60,\"pattern\":[80,60,80]}"));
    assert(buzz("{\"ms\":60000}") == ESP_OK && !strcmp(out, "{\"ms\":2000,\"strength\":100}"));   /* the cap */
    /* an unknown name: its own code; bad args: invalid_args (the link's ESP_ERR_INVALID_ARG) */
    assert(buzz("{\"pattern\":\"bogus\"}") == LINK_CMD_REFUSED && !strcmp(refused, "unknown_pattern"));
    const char *bad[] = { "{}", "{\"ms\":100,\"pattern\":\"tap\"}", "{\"pattern\":5}", "{\"pattern\":[]}", "{\"pattern\":[0,100]}",
                          "{\"ms\":0}", "{\"ms\":\"100\"}", "{\"ms\":100,\"strength\":\"x\"}", "{\"pattern\":[1,2,3,4,5,6,7,8,9]}" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) assert(buzz(bad[i]) == ESP_ERR_INVALID_ARG && !refused);

    /* never into the open mic: dropped (not queued), the reason said, the motor untouched */
    const unsigned on0 = ons;
    mic_open = true;
    assert(buzz("{\"pattern\":\"tap\"}") == LINK_CMD_REFUSED && !strcmp(refused, "listening"));
    assert(!strcmp(post("{\"ms\":100}"), "{\"ok\":false,\"error\":\"listening\"}"));
    assert(haptic_cue("double") == ESP_ERR_INVALID_STATE);
    assert(ons == on0);
    mic_open = false;
    assert(haptic_cue("double") == ESP_OK && ons == on0 + 1);   /* nothing was kept for later: this is a new one */
    assert(haptic_cue("bogus") == ESP_ERR_INVALID_ARG);
    /* the touch feedback: its own length and strength (HAPTIC_TOUCH_*), apart from the agent's tick (30 ms, full) */
    assert(haptic_touch() == ESP_OK && ons == on0 + 2 && armed_us == HAPTIC_TOUCH_MS * 1000u && duty == HAPTIC_TOUCH_STRENGTH * 1024u / 100u);
    assert(haptic_cue("tick") == ESP_OK && armed_us == 30000 && duty == 1024);
    mic_open = true; assert(haptic_touch() == ESP_ERR_INVALID_STATE); mic_open = false;

    /* HTTP: the answers as before, and a name too */
    assert(!strcmp(post("{\"ms\":300,\"strength\":100}"), "{\"ok\":true,\"ms\":300,\"strength\":100}"));
    assert(!strcmp(post("{\"pattern\":[80,60,80],\"strength\":100}"), "{\"ok\":true,\"ms\":220,\"strength\":100,\"pattern\":[80,60,80]}"));
    assert(!strcmp(post("{\"pattern\":\"nudge\"}"), "{\"ok\":true,\"ms\":350,\"strength\":100,\"pattern\":[350]}"));
    assert(!strcmp(post("{\"pattern\":\"nope\"}"), "{\"ok\":false,\"error\":\"unknown_pattern\"}"));
    assert(!strcmp(post("{\"ms\":-1}"), "{\"ok\":false,\"error\":\"invalid_args\"}"));
    assert(!strcmp(post("not json"), "{\"ok\":false,\"error\":\"invalid_args\"}"));
    printf("LINK HAPTIC PASS: buzz args (names, ms, pattern, strength), unknown_pattern, invalid_args, no_motor, listening drops it, HTTP answers, the touch feedback at HAPTIC_TOUCH_* apart from the agent's tick\n");
    return 0;
}
