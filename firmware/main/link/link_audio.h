#pragma once
#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"
#include <stddef.h>
esp_err_t link_audio_init(void);
esp_err_t link_audio_say(const cJSON *args, cJSON **result);
esp_err_t link_audio_listen(const cJSON *args, cJSON **result);
esp_err_t link_audio_receive(const void *data, size_t len);
void link_audio_tick(void);
void link_audio_cancel(void);
bool link_audio_busy(void);
