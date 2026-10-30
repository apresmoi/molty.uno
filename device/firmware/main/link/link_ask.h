/*
 * link_ask.h — the link's `ask` command: a question answered by a tap on the screen.
 *
 * The link task owns the question (command, cancellation, timeout, answer); the touch
 * path (sense_touch, on the fluid task) only drops a tap into a one-slot mailbox; the
 * screen (mole_fluid_ask_*) follows. Device events (docs/AGENT-LINK.md), only while the
 * link is up, never rate-limited (one per question):
 *   {"t":"evt","name":"answer","data":{"job":J,"index":I,"label":L}}            a tap
 *   {"t":"evt","name":"answer","data":{"job":J,"index":null,"reason":"timeout"}}   no tap in time
 *   {"t":"evt","name":"answer","data":{"job":J,"index":null,"reason":"cancelled"}} an accepted
 *       command that changes the screen (say, show, mood, state, listen, another ask; a
 *       refused one leaves the question up), a pairing, or the link going down (no event
 *       then: it is down)
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

/* args {job, options, colors?, timeout_ms} (link_ask_args) → result {job} at once. "unsupported"
 * without touch; a pending question is cancelled first. */
esp_err_t link_ask_cmd(const cJSON *args, cJSON **result);
/* A question the firmware itself asks (self_update.c's "update" / "later"): the same
 * screen, tap, timeout and cancellation as `ask`, but its end goes to done(index, reason)
 * on the link task — index ≥ 0 the option tapped, else reason "timeout" | "cancelled"
 * (also when the link goes down) — instead of an `answer` event. A pending question is
 * cancelled first (its owner hears "cancelled"). Link task; labels as link_ask_args
 * accepts them (the caller's own constants); colors[i] 0xRRGGBB (NULL, or -1 for one
 * option: the screen's default by index). */
typedef void (*link_ask_done_t)(int index, const char *reason);
esp_err_t link_ask_open(const char *job, const char *const *labels, const int32_t *colors, int n, unsigned timeout_ms, link_ask_done_t done);
/* Something else takes the screen: a pending question ends "cancelled" (link task). */
void link_ask_cancel(void);
/* Link task, every loop: a tap that answers, else the timeout. */
void link_ask_tick(void);
/* Touch path, on a touch-down edge at (dx, dy) px from the screen's centre: true when a
 * question is pending — the tap is not a `tap` event. It answers that question (the first
 * such tap wins) only when the panel shows it (mole_fluid_ask_shown): a tap on a frame
 * still showing a replaced question, or its letters morphing into the new one, answers
 * nothing. Its capture time decides against the deadline: at or before it, it wins over
 * the timeout. */
bool link_ask_tap(float dx, float dy);
bool link_ask_pending(void);
