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
/* A bound gesture's `talk` (gesture.h): the same listen as a shake's, whatever the shake
 * setting says. Any task. False = no mic. */
bool link_audio_gesture_talk(void);
/* A recording is running (and for how long). Any task. */
bool link_audio_listening(uint32_t *age_ms);
/* A say is running with no audio received yet. Any task. */
bool link_audio_say_waiting(void);
/* Ends the running listen; what was recorded goes up, reason "tap". Any task. */
typedef enum { LINK_AUDIO_REQ_STOP = 0, LINK_AUDIO_REQ_CANCEL = 1 } link_audio_req_t;
/** The running job's name: read it BEFORE looking at the job's state, then hand it to _request. */
uint32_t link_audio_job_gen(void);
/** Stop and send (a listen) or cancel THAT job; ignored if it has already ended. */
void link_audio_request(uint32_t gen, link_audio_req_t what);
/* A bound `stop`: ends the listen (or drops it with the link down), or cuts the say. Any task. */
void link_audio_stop_any(void);
esp_err_t link_audio_say_cancel(const cJSON *args, cJSON **result);
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
