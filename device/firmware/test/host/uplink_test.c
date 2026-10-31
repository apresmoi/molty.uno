/* link_uplink.c on the host: the listen's uplink ring (order, resend, overflow, drop). */
#include "link_uplink.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int16_t store[UPLINK_SLOTS * UPLINK_SLOT_SAMPLES];
static uplink_meta_t frame(uint32_t seq, bool end) { uplink_meta_t m = {.seq = seq, .ct = seq * 300, .n = end ? 0 : UPLINK_SLOT_SAMPLES, .end = end}; return m; }
static int16_t pcm[UPLINK_SLOT_SAMPLES];
static void push(uplink_t *u, uint32_t seq, bool end) {
    for (int i = 0; i < UPLINK_SLOT_SAMPLES; i++) pcm[i] = (int16_t)seq;
    uplink_meta_t m = frame(seq, end);
    assert(uplink_push(u, &m, pcm));
}
/* send the head: its seq (and that its audio is its own) */
static uint32_t send_ok(uplink_t *u) {
    const int16_t *p; const uplink_meta_t *m = uplink_peek(u, &p);
    assert(m && u->inflight);
    if (m->n) assert(p[0] == (int16_t)m->seq && p[m->n - 1] == (int16_t)m->seq);
    uint32_t s = m->seq; uplink_sent(u); return s;
}

int main(void) {
    uplink_t u; uplink_init(&u, store);
    assert(!uplink_peek(&u, NULL) && uplink_pending(&u) == 0);
    /* in order, one at a time */
    for (uint32_t s = 0; s < 5; s++) push(&u, s, false);
    assert(send_ok(&u) == 0 && send_ok(&u) == 1);
    /* a failed send keeps the frame first: the same seq goes again, counted */
    const uplink_meta_t *m = uplink_peek(&u, NULL); assert(m->seq == 2); uplink_failed(&u);
    m = uplink_peek(&u, NULL); assert(m->seq == 2 && m->rt == 1); uplink_failed(&u);
    m = uplink_peek(&u, NULL); assert(m->seq == 2 && m->rt == 2);
    assert(send_ok(&u) == 2);
    assert(u.fail == 2 && u.sent == 3);
    assert(send_ok(&u) == 3 && send_ok(&u) == 4 && !uplink_pending(&u));
    /* overflow: the OLDEST unsent goes (not the one in flight, never the end) */
    uplink_reset(&u);
    for (uint32_t s = 0; s < UPLINK_SLOTS; s++) push(&u, s, false);
    assert(uplink_pending(&u) == UPLINK_SLOTS);
    uplink_peek(&u, NULL);                      /* seq 0 in flight */
    push(&u, UPLINK_SLOTS, false);              /* full: seq 1 goes */
    assert(u.drop == 1 && u.drop_samples == UPLINK_SLOT_SAMPLES);
    uplink_sent(&u);                            /* seq 0 went */
    assert(send_ok(&u) == 2);                   /* a gap at 1: the relay counts it lost */
    for (uint32_t s = 3; s <= UPLINK_SLOTS; s++) assert(send_ok(&u) == s);
    assert(!uplink_pending(&u));
    /* not in flight: the head itself is the oldest unsent */
    uplink_reset(&u);
    for (uint32_t s = 0; s < UPLINK_SLOTS; s++) push(&u, s, false);
    push(&u, 100, false); assert(send_ok(&u) == 1 && u.drop == 1);
    /* the end frame is never dropped by an overflow */
    uplink_reset(&u);
    push(&u, 0, true);
    for (uint32_t s = 1; s < UPLINK_SLOTS + 3; s++) push(&u, s, false);
    assert(send_ok(&u) == 0);
    /* drop_all: everything but the frame in flight (and the end frame when kept) */
    uplink_reset(&u);
    for (uint32_t s = 0; s < 5; s++) push(&u, s, false);
    push(&u, 5, true);
    uplink_peek(&u, NULL);
    uplink_drop_all(&u, true);
    assert(uplink_pending(&u) == 2 && u.drop == 4);
    uplink_failed(&u);                          /* seq 0 failed: still first */
    assert(send_ok(&u) == 0 && send_ok(&u) == 5 && !uplink_pending(&u));
    uplink_drop_all(&u, false); assert(!uplink_pending(&u));
    /* sent / failed with nothing in flight change nothing */
    uplink_sent(&u); uplink_failed(&u); assert(u.sent == 2 && u.fail == 1);
    /* a frame bigger than a slot is refused */
    uplink_meta_t big = {.seq = 9, .n = UPLINK_SLOT_SAMPLES + 1}; assert(!uplink_push(&u, &big, pcm));
    puts("UPLINK PASS: in order, resend keeps the seq (rt), overflow drops the oldest unsent (not in flight, never the end), drop_all, bounds");
    return 0;
}
