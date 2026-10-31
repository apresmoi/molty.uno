/* SPDX-License-Identifier: MIT — link_uplink.c: the listen's uplink ring (link_uplink.h). */
#include "link_uplink.h"
#include <string.h>

#define AT(u, i) (((u)->head + (i)) % UPLINK_SLOTS)

void uplink_init(uplink_t *u, int16_t *pcm_storage)
{
    memset(u, 0, sizeof(*u));
    u->pcm = pcm_storage;
}

void uplink_reset(uplink_t *u) { uplink_init(u, u->pcm); }

int uplink_pending(const uplink_t *u) { return u->count; }

/* remove the queued frame at ring offset i (> 0 or not in flight), counting it dropped */
static void drop_at(uplink_t *u, int i)
{
    const int at = AT(u, i);
    u->drop++; u->drop_samples += u->meta[at].n;
    for (int k = i; k < u->count - 1; k++) {           /* close the gap, order kept */
        const int to = AT(u, k), from = AT(u, k + 1);
        u->meta[to] = u->meta[from];
        if (u->pcm) memcpy(u->pcm + (size_t)to * UPLINK_SLOT_SAMPLES, u->pcm + (size_t)from * UPLINK_SLOT_SAMPLES,
                           (size_t)u->meta[from].n * 2);
    }
    u->count--;
}

bool uplink_push(uplink_t *u, const uplink_meta_t *m, const int16_t *pcm)
{
    if (m->n > UPLINK_SLOT_SAMPLES) return false;
    if (u->retained && u->count + u->retained >= UPLINK_SLOTS) u->retained--;   /* the oldest sent frame's slot */
    if (u->count == UPLINK_SLOTS) {
        int victim = -1;                     /* the oldest unsent, not in flight, not the end */
        for (int i = u->inflight ? 1 : 0; i < u->count && victim < 0; i++)
            if (!u->meta[AT(u, i)].end) victim = i;
        if (victim < 0) return false;
        drop_at(u, victim);
    }
    const int at = AT(u, u->count);
    u->meta[at] = *m;
    u->meta[at].rt = 0;
    if (m->n && pcm && u->pcm) memcpy(u->pcm + (size_t)at * UPLINK_SLOT_SAMPLES, pcm, (size_t)m->n * 2);
    u->count++;
    return true;
}

const uplink_meta_t *uplink_peek(uplink_t *u, const int16_t **pcm)
{
    if (!u->count) return NULL;
    u->inflight = true;
    if (pcm) *pcm = u->pcm ? u->pcm + (size_t)u->head * UPLINK_SLOT_SAMPLES : NULL;
    return &u->meta[u->head];
}

void uplink_sent(uplink_t *u)
{
    if (!u->count || !u->inflight) return;
    u->inflight = false;
    u->sent++;
    u->head = (u->head + 1) % UPLINK_SLOTS;
    u->count--;
    if (u->count + u->retained < UPLINK_SLOTS) u->retained++;
}

bool uplink_rewind(uplink_t *u, uint32_t seq)
{
    if (u->inflight) return false;
    if (u->count && u->meta[u->head].seq == seq) return true;
    for (int back = 1; back <= u->retained; back++) {
        const int at = (u->head - back + UPLINK_SLOTS) % UPLINK_SLOTS;
        if (u->meta[at].seq != seq) continue;
        u->head = at; u->count += back; u->retained -= back; u->sent -= (uint32_t)back;
        return true;
    }
    return !u->count && !u->retained;
}

void uplink_failed(uplink_t *u)
{
    if (!u->count || !u->inflight) return;
    u->inflight = false;
    u->fail++;
    u->meta[u->head].rt++;
}

void uplink_drop_all(uplink_t *u, bool keep_end)
{
    for (int i = u->count - 1; i >= (u->inflight ? 1 : 0); i--)
        if (!(keep_end && u->meta[AT(u, i)].end)) drop_at(u, i);
}
