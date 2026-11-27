/* rhythm_stub.c: the beat player and the tone player as the link tests that do not test them see them: none plays
 * (link_rhythm_test.c tests the real ones). */
#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"
bool rhythm_wants_speaker(void) { return false; }
void rhythm_dev_play(bool (*job_waiting)(void)) { (void)job_waiting; }
esp_err_t rhythm_cmd_play(const cJSON *args, cJSON **out) { (void)args; (void)out; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t rhythm_cmd_set(const cJSON *args, cJSON **out) { (void)args; (void)out; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t rhythm_cmd_stop(const cJSON *args, cJSON **out) { (void)args; (void)out; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t wave_cmd_play(const cJSON *args, cJSON **out) { (void)args; (void)out; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t wave_cmd_set(const cJSON *args, cJSON **out) { (void)args; (void)out; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t wave_cmd_stop(const cJSON *args, cJSON **out) { (void)args; (void)out; return ESP_ERR_NOT_SUPPORTED; }
