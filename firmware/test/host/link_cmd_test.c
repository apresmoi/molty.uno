/* link_cmd.c on the host: which moods count as explicit (link_cmd_mood_gen). link_audio
 * reverts to idle at a listen/say's end only when this count did not move
 * (link_audio_pair_test.c test_mood_survives). */
#include "link_sdk_stub.h"
typedef struct { float ax, ay, az, gx, gy, gz; uint32_t ms; } molty_imu_sample_t;
bool molty_imu_get(molty_imu_sample_t *out);
esp_err_t molty_speaker_set_volume(uint8_t vol);
cJSON *mole_diag_json(int timeline_secs);
char *mole_diag_log_text(uint32_t since, size_t max_bytes, uint32_t *next, bool json);
#include "../../main/link/link_cmd.c"
#include <assert.h>
#include <stdio.h>

bool molty_imu_get(molty_imu_sample_t *out) { (void)out; return false; }
esp_err_t molty_speaker_set_volume(uint8_t vol) { (void)vol; return ESP_OK; }
cJSON *mole_diag_json(int t) { (void)t; return cJSON_CreateObject(); }
char *mole_diag_log_text(uint32_t s, size_t m, uint32_t *n, bool j) { (void)s;(void)m;(void)n;(void)j; return NULL; }
esp_err_t mole_fluid_set_text(const char *t, int len) { (void)t;(void)len; return ESP_OK; }
bool link_pair_active(void) { return false; }
esp_err_t link_audio_say(const cJSON *a, cJSON **o) { (void)a;(void)o; return ESP_OK; }
esp_err_t link_audio_listen(const cJSON *a, cJSON **o) { (void)a;(void)o; return ESP_OK; }

static esp_err_t run(const char *name, const char *args) {
    cJSON *a=cJSON_Parse(args),*out=NULL; assert(a);
    esp_err_t e=link_cmd_run(name,a,&out); cJSON_Delete(a); cJSON_Delete(out); return e;
}
int main(void) {
    link_cmd_init();
    unsigned g=link_cmd_mood_gen();
    /* the device's own moods (listening / speaking / idle around a job) are not explicit */
    assert(link_cmd_mood("listening",-1)==ESP_OK && link_cmd_mood_gen()==g);
    /* the link's mood command is, once accepted, with or without hold_s */
    assert(run("mood","{\"mood\":\"happy\",\"hold_s\":0}")==ESP_OK && link_cmd_mood_gen()==g+1);
    assert(strstr(sdk_state,"\"mood\":\"happy\"") && strstr(sdk_state,"\"hold_s\":0"));
    assert(run("mood","{\"mood\":\"thinking\"}")==ESP_OK && link_cmd_mood_gen()==g+2);
    /* rejected: not counted */
    assert(run("mood","{\"mood\":\"furious\"}")==ESP_ERR_INVALID_ARG && link_cmd_mood_gen()==g+2);
    assert(run("mood","{\"mood\":\"happy\",\"hold_s\":-1}")==ESP_ERR_INVALID_ARG && link_cmd_mood_gen()==g+2);
    assert(run("show","{\"text\":\"hi\"}")==ESP_OK && link_cmd_mood_gen()==g+2);
    /* local HTTP state bodies: only a top-level mood string counts */
    assert(link_cmd_state_set_explicit("{\"mood\":\"sleepy\",\"hold_s\":30}")==ESP_OK && link_cmd_mood_gen()==g+3);
    assert(link_cmd_state_set_explicit("{\"state\":\"text\",\"text\":\"my \\\"mood\\\" is fine\"}")==ESP_OK && link_cmd_mood_gen()==g+3);
    assert(link_cmd_state_set_explicit("{\"state\":\"blob\"}")==ESP_OK && link_cmd_mood_gen()==g+3);
    assert(link_cmd_state_set_explicit("not json \"mood\"")==ESP_OK && link_cmd_mood_gen()==g+3);
    /* the race: an explicit mood's apply + count, and a job end's check + idle, share one
     * lock (HTTP task vs link task). Every mood write below must run with it held. */
    sdk_state_locked=false; assert(link_cmd_state_set_explicit("{\"mood\":\"happy\"}")==ESP_OK && sdk_state_locked);
    sdk_state_locked=false; assert(run("mood","{\"mood\":\"sleepy\"}")==ESP_OK && sdk_state_locked);
    unsigned g0=link_cmd_mood_gen();
    strcpy(sdk_state,""); sdk_state_locked=false; link_cmd_mood_idle_unless(g0);
    assert(strstr(sdk_state,"\"mood\":\"idle\"") && sdk_state_locked);
    /* an explicit mood since the snapshot: idle is skipped */
    assert(run("mood","{\"mood\":\"thinking\"}")==ESP_OK);
    link_cmd_mood_idle_unless(g0); assert(strstr(sdk_state,"\"mood\":\"thinking\""));
    assert(sdk_locks==0);
    cJSON_Delete(sdk_sent);
    puts("LINK CMD PASS: explicit moods counted (link mood cmd, local state POST) under the mood lock, own/rejected moods not");
}
