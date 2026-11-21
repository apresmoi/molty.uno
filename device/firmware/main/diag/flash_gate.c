#include "flash_gate.h"
#include <string.h>

bool fg_step(fg_hold_t *h, fg_stats_t *st, int64_t now_us, int64_t calm_us, bool force)
{
    const bool calm = calm_us >= FG_CALM_US, capped = h->held && now_us - h->since >= FG_HOLD_MAX_US;
    if (!force && !calm && !capped) {
        if (!h->held) { h->held = true; h->since = now_us; st->held++; }
        return false;
    }
    if (h->held) {
        const int64_t ms = (now_us - h->since) / 1000;
        if (ms > st->hold_max_ms) st->hold_max_ms = (uint32_t)ms;
        st->flushed++;
        if (!calm && !force) st->capped++;
        h->held = false;
    }
    if (force) st->forced++;
    return true;
}

static void batch_start(fg_batch_t *b, int64_t now_us, const char *who)
{
    memset(b, 0, sizeof(*b));
    strncpy(b->who, who ? who : "?", sizeof(b->who) - 1);
    b->first_us = now_us;
}

bool fg_batch_op(fg_batch_t *b, fg_batch_t *done, int64_t now_us, const char *who, uint32_t w, uint32_t e, uint32_t r,
                 uint32_t dur_us, bool anim)
{
    bool closed = false;
    if (b->ops && now_us - dur_us - b->last_us > FG_BATCH_GAP_US) { *done = *b; closed = true; b->ops = 0; }
    if (!b->ops) batch_start(b, now_us - dur_us, who);
    else if (strncmp(b->who, who ? who : "?", sizeof(b->who) - 1)) b->mixed = true;
    if ((w || e) && !b->wwho[0]) strncpy(b->wwho, who ? who : "?", sizeof(b->wwho) - 1);
    b->ops++; b->anim_ops += anim && (w || e);
    b->wbytes += w; b->ebytes += e; b->rbytes += r;
    if (w || e) b->busy_us += dur_us; else b->read_us += dur_us;
    b->last_us = now_us;
    return closed;
}

bool fg_batch_idle(fg_batch_t *b, fg_batch_t *done, int64_t now_us)
{
    if (!b->ops || now_us - b->last_us <= FG_BATCH_GAP_US) return false;
    *done = *b;
    b->ops = 0;
    return true;
}

bool fg_batch_worth(const fg_batch_t *b) { return b->wbytes || b->ebytes || b->read_us >= FG_READ_LOG_US; }
