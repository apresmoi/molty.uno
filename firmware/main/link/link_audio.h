#pragma once
#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"
#include <stddef.h>
esp_err_t link_audio_init(void);
esp_err_t link_audio_say(const cJSON *args, cJSON **result);
esp_err_t link_audio_listen(const cJSON *args, cJSON **result);
esp_err_t link_audio_listen_stop(const cJSON *args, cJSON **result);
esp_err_t link_audio_receive(const void *data, size_t len);
void link_audio_tick(void);
void link_audio_cancel(void);
bool link_audio_busy(void);
/* Shake to talk (link_protocol.h): the sense task hands a shake over. False = not taken (no mic,
 * or the console's setting is off): the caller sends the plain shake event itself. Taken: the
 * link task starts the shake's listen when nothing else owns the device, and sends the shake
 * event (naming the listen, or plain). */
bool link_audio_shake(double peak_g, int reversals, unsigned ms);
/* Link task: the relay's welcome listed listen_adopt (true), or the link went down (false). */
void link_audio_relay_adopts(bool on);
