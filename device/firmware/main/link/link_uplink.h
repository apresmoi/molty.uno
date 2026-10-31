/*
 * SPDX-License-Identifier: MIT
 *
 * link_uplink.h — the listen's uplink ring: mic frames waiting to go up to the relay.
 *
 * Plain C, no FreeRTOS / ESP-IDF (link_audio.c locks around it; test/host/uplink_test.c
 * drives it directly). Why it exists: the mic must never wait on the network. A WebSocket
 * send can block for seconds on weak Wi-Fi (2 s per try), while the I2S DMA ring holds
 * only ~90 ms: a blocking send dropped audio silently, and a failed send ended the listen.
 * Now the audio task only pushes frames here; a sender takes the oldest, sends it, and on
 * failure keeps it and tries again later — in order, same seq. docs/AGENT-LINK.md "Turn trace".
 *
 *   push      a frame (<= UPLINK_SLOT_SAMPLES) with its seq. Full ring: the OLDEST unsent
 *             frame that is not being sent right now (and not the end frame) is dropped and
 *             counted (drop / drop_samples): its seq becomes a gap the relay reports as lost.
 *   peek      the oldest frame, marked in flight (the sender sends it outside the lock).
 *   sent      the in-flight frame went up: it leaves the ring.
 *   failed    the in-flight frame did not: it stays first, its rt (failed tries) + 1.
 *   rewind    the link dropped: frames "sent" may have died in the TCP buffers. Back to the seq
 *             the relay says it needs next; sent frames stay in the ring until overwritten.
 *   drop_all  flush deadline / cancel: every frame but the in-flight one leaves, counted
 *             (keep_end: the end frame stays, so the relay still learns the listen ended).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPLINK_SLOT_SAMPLES 4800        /* 300 ms @16 kHz: one uplink frame               */
#define UPLINK_SLOTS        66          /* 66 x 300 ms = 19.8 s: a link drop's window (15 s, link_audio.c) + what was sent just before it */
#define UPLINK_LV_MAX       4           /* level bytes per frame (3 x 100 ms + a partial) */

typedef struct {
    uint32_t seq;
    uint32_t ct;                 /* capture ms of the frame's first sample (from the mic start) */
    uint32_t ready_ms;           /* device ms (esp_timer) when the frame was complete            */
    uint16_t n;                  /* samples                                                       */
    uint16_t rt;                 /* failed sends of this frame so far                             */
    uint16_t cl;                 /* saturated samples                                             */
    uint8_t  nlv;
    uint8_t  lv[UPLINK_LV_MAX];  /* chunk levels (link_level_byte)                                */
    bool     end;                /* the listen's last frame                                       */
} uplink_meta_t;

typedef struct {
    uplink_meta_t meta[UPLINK_SLOTS];
    int16_t *pcm;                /* UPLINK_SLOTS * UPLINK_SLOT_SAMPLES, the caller's storage */
    int head, count;
    int retained;                /* frames before head that went up and are still intact (rewind) */
    bool inflight;               /* meta[head] is being sent                                 */
    uint32_t sent, fail, drop, drop_samples;
} uplink_t;

void uplink_init(uplink_t *u, int16_t *pcm_storage);
/** Forget everything, stats too (a new listen). */
void uplink_reset(uplink_t *u);
/** Queue a frame (pcm may be NULL when m->n == 0). False only when nothing could be made
 *  room for (every slot in flight / the end frame: cannot happen with > 2 slots). */
bool uplink_push(uplink_t *u, const uplink_meta_t *m, const int16_t *pcm);
/** The oldest frame (marked in flight), or NULL when empty. *pcm points into the ring:
 *  valid until sent / failed (push never drops the in-flight slot). */
const uplink_meta_t *uplink_peek(uplink_t *u, const int16_t **pcm);
void uplink_sent(uplink_t *u);
void uplink_failed(uplink_t *u);
/** Queue again from `seq` (the relay's next). False: that frame is gone (or one is in flight). */
bool uplink_rewind(uplink_t *u, uint32_t seq);
/** Drop every queued frame except the one in flight (and the end frame when keep_end). */
void uplink_drop_all(uplink_t *u, bool keep_end);
int  uplink_pending(const uplink_t *u);
