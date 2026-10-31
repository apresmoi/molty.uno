#pragma once
#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
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
/* Shake to talk: the sense task's IMU samples (g, esp_timer ms), every one. While a shake's
 * listen waits for the shake to settle (shake_settle.h) they decide where its audio begins;
 * otherwise a no-op. Any task. */
void link_audio_imu(float ax, float ay, float az, uint32_t ms);
/* Link task: the relay's hint {"t":"hint","job","k":"pause"|"question"} (cap "hints"): the
 * voice box heard a pause / a question in the running listen — Molty may react (a nod, a
 * glance: listen_react.h). Ignored unless `job` is the running listen. */
void link_audio_hint(const char *job, const char *kind);
/* Link task: the relay just showed a reaction on the creature during the running listen (an
 * `act`, or a `mood {react:true}`) that plays for hold_ms: Molty's own listener reactions
 * (listen_react.h) wait for it instead of cutting it short. No-op outside a listen. */
void link_audio_external_react(uint32_t hold_ms);
