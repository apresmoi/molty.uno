/*
 * link_ask.h — the link's `ask` command: a question answered by a tap on the screen.
 *
 * The link task owns the question (command, cancellation, timeout, answer); the touch
 * path (sense_touch, on the fluid task) only drops a tap into a one-slot mailbox; the
 * screen (mole_fluid_ask_*) follows. Device events (docs/AGENT-LINK.md), only while the
 * link is up, never rate-limited (one per question):
 *   {"t":"evt","name":"answer","data":{"job":J,"index":I,"label":L}}            a tap
 *   {"t":"evt","name":"answer","data":{"job":J,"index":null,"reason":"timeout"}}   no tap in time
 *   {"t":"evt","name":"answer","data":{"job":J,"index":null,"reason":"cancelled"}} a command that
 *       changes the screen (say, show, mood, state, listen, another ask), a pairing, or
 *       the link going down (no event then: it is down)
 */
#pragma once
#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"

/* args {job, options, timeout_ms} (link_ask_args) → result {job} at once. "unsupported"
 * without touch; a pending question is cancelled first. */
esp_err_t link_ask_cmd(const cJSON *args, cJSON **result);
/* Something else takes the screen: a pending question ends "cancelled" (link task). */
void link_ask_cancel(void);
/* Link task, every loop: a tap that answers, else the timeout. */
void link_ask_tick(void);
/* Touch path, on a touch-down edge at (dx, dy) px from the screen's centre: true when a
 * question is pending — the tap is its answer, not a `tap` event (the first tap wins). */
bool link_ask_tap(float dx, float dy);
bool link_ask_pending(void);
