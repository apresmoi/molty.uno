#include "link_identity.h"
#include "link_protocol.h"
#include "molty_board.h"
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "nvs.h"

static char s_id[LINK_ID_LEN + 1], s_code[9];
static uint8_t s_key[32];
static int64_t s_rotated;

const char *link_identity_id(void) { return s_id; }
const char *link_identity_code(void) { return s_code; }

static uint32_t random_word(void *ctx) { (void)ctx; return esp_random(); }

void link_identity_rotate(void)
{
    link_code_generate(s_code, random_word, NULL);
    s_rotated = esp_timer_get_time();
}

bool link_identity_tick(void)
{
    if (esp_timer_get_time() - s_rotated < 900000000LL) return false;
    link_identity_rotate();
    return true;
}

esp_err_t link_identity_init(void)
{
    nvs_handle_t n;
    esp_err_t e = nvs_open("agent_link", NVS_READWRITE, &n);
    if (e != ESP_OK) return e;
    size_t len = sizeof(s_key);
    esp_err_t key_err = nvs_get_blob(n, "device_key", s_key, &len);
    size_t id_len = sizeof(s_id);
    esp_err_t id_err = nvs_get_str(n, "device_id", s_id, &id_len);
    /* Older firmware stored only a key for its MAC-derived ID. Migrate both
     * together: the old public ID must never be reused with a new key. */
    if ((key_err == ESP_OK || key_err == ESP_ERR_NVS_NOT_FOUND) &&
        (id_err == ESP_OK || id_err == ESP_ERR_NVS_NOT_FOUND) &&
        (key_err == ESP_ERR_NVS_NOT_FOUND || id_err == ESP_ERR_NVS_NOT_FOUND)) {
        link_id_generate(s_id, random_word, NULL);
        esp_fill_random(s_key, sizeof(s_key));
        e = nvs_set_str(n, "device_id", s_id);
        if (e == ESP_OK) e = nvs_set_blob(n, "device_key", s_key, sizeof(s_key));
        if (e == ESP_OK) e = nvs_commit(n);
    } else if (key_err != ESP_OK) e = key_err;
    else if (id_err != ESP_OK) e = id_err;
    else if (len != sizeof(s_key) || !link_id_valid(s_id)) e = ESP_ERR_INVALID_SIZE;
    nvs_close(n);
    if (e == ESP_OK) link_identity_rotate();
    return e;
}

esp_err_t link_identity_forget(void)
{
    nvs_handle_t n;
    esp_err_t e = nvs_open("agent_link", NVS_READWRITE, &n);
    if (e != ESP_OK) return e;
    e = nvs_erase_all(n);
    if (e == ESP_OK) e = nvs_commit(n);
    nvs_close(n);
    if (e == ESP_OK) {
        memset(s_key, 0, sizeof(s_key));
        e = link_identity_init();
    }
    return e;
}

cJSON *link_identity_hello(void)
{
    char key[48]; size_t len;
    if (mbedtls_base64_encode((unsigned char *)key, sizeof(key), &len, s_key, sizeof(s_key))) return NULL;
    key[len] = 0;
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
    cJSON_AddStringToObject(j, "t", "hello");
    cJSON_AddStringToObject(j, "id", s_id);
    cJSON_AddStringToObject(j, "key", key);
    memset(key, 0, sizeof(key));
    cJSON_AddStringToObject(j, "fw", esp_app_get_description()->version);
    /* say / listen only where the board has a speaker / mic (the relay refuses those
     * jobs up front otherwise); board names the hardware for agents and support */
    const char *caps[12]; int n = 0;
    caps[n++] = "fluid";
    if (molty_board_has(MOLTY_CAP_SPEAKER)) caps[n++] = "say";
    if (molty_board_has(MOLTY_CAP_SPEAKER)) caps[n++] = "say_cues";   /* say {cues:true}: word-timed text */
    if (molty_board_has(MOLTY_CAP_MIC)) caps[n++] = "listen";
    if (molty_board_has(MOLTY_CAP_MIC)) caps[n++] = "listen_stop";   /* the relay may end a listen early (turn_end) */
    if (molty_board_has(MOLTY_CAP_MIC)) caps[n++] = "listen_window";   /* ms optional (the setting), 0..30000, 0 = unlimited */
    if (molty_board_has(MOLTY_CAP_MIC)) caps[n++] = "ack";   /* acknowledges the end of a turn itself (chirp + thinking) */
    caps[n++] = "show"; caps[n++] = "mood"; caps[n++] = "creature";
    if (molty_board_has(MOLTY_CAP_TOUCH)) caps[n++] = "ask";   /* answered by a tap */
    if (molty_board_has(MOLTY_CAP_TOUCH)) caps[n++] = "update";   /* self-update, confirmed by a tap */
    cJSON_AddItemToObject(j, "caps", cJSON_CreateStringArray(caps, n));
    cJSON_AddStringToObject(j, "board", molty_board_id());
    cJSON_AddStringToObject(j, "link_code", s_code);
    return j;
}
