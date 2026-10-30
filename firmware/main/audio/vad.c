/*
 * SPDX-License-Identifier: MIT
 *
 * vad.c — end-of-speech detection for `listen`. See vad.h for the model.
 */
#include "vad.h"

#include <math.h>
#include <string.h>

#define VAD_SILENT_DB (-120.f)

static uint32_t clamp_u(uint32_t x, uint32_t lo, uint32_t hi) { return x < lo ? lo : x > hi ? hi : x; }

void vad_init(vad_t *v, const vad_cfg_t *cfg)
{
    static const vad_cfg_t def = VAD_CFG_DEFAULT;
    memset(v, 0, sizeof(*v));
    v->cfg = cfg ? *cfg : def;
    vad_cfg_t *c = &v->cfg;
    c->floor_window_ms = clamp_u(c->floor_window_ms, 100, VAD_RING_MAX * 100);
    c->calib_ms = clamp_u(c->calib_ms, VAD_FRAME_MS, c->floor_window_ms);
    if (c->off_db > c->on_db) c->off_db = c->on_db;
    v->floor_db = v->last_db = VAD_SILENT_DB;
    v->block_min = INFINITY;
}

/* minimum over the completed 100 ms blocks in the window and the block being filled */
static float floor_now(const vad_t *v)
{
    float m = v->block_min;
    for (uint32_t i = 0; i < v->ring_len; i++) if (v->ring[i] < m) m = v->ring[i];
    return m;
}

static void floor_push(vad_t *v, float db)
{
    if (db < v->block_min) v->block_min = db;
    if (++v->block_n < VAD_BLOCK_FRAMES) return;
    uint32_t cap = v->cfg.floor_window_ms / 100;
    v->ring[v->ring_pos] = v->block_min;
    v->ring_pos = (v->ring_pos + 1) % cap;
    if (v->ring_len < cap) v->ring_len++;
    v->block_n = 0;
    v->block_min = INFINITY;
}

static vad_verdict_t frame(vad_t *v, float db)
{
    const vad_cfg_t *c = &v->cfg;
    v->last_db = db;
    bool calibrated = v->t_ms >= c->calib_ms;
    if (calibrated && c->enabled) {
        v->floor_db = floor_now(v);
        /* Until the window has filled, the floor may be speech that was already
         * running when listen started: never trust it above init_floor_max_dbfs. */
        if (v->t_ms < c->floor_window_ms && v->floor_db > c->init_floor_max_dbfs) v->floor_db = c->init_floor_max_dbfs;
        bool was = v->phase == VAD_SPEECH;
        bool voiced = db > c->abs_min_dbfs && db > v->floor_db + (was ? c->off_db : c->on_db);
        if (voiced) {
            v->phase = VAD_SPEECH;
            v->speech_ms += VAD_FRAME_MS;
            v->silence_ms = 0;
        } else if (v->phase != VAD_WAITING) {
            v->phase = VAD_TRAILING;
            v->silence_ms += VAD_FRAME_MS;
        }
    }
    floor_push(v, db);
    v->t_ms += VAD_FRAME_MS;
    if (!calibrated) v->floor_db = floor_now(v);

    if (c->enabled) {
        bool spoke = v->speech_ms >= c->min_speech_ms;
        if (spoke && v->phase == VAD_TRAILING && v->silence_ms >= c->end_silence_ms && v->t_ms >= c->min_record_ms)
            return VAD_END_SILENCE;
        if (!spoke && v->phase != VAD_SPEECH && v->t_ms >= c->no_speech_ms) return VAD_END_NO_SPEECH;
    }
    if (v->t_ms >= c->max_ms) return VAD_END_MAX;
    return VAD_CONTINUE;
}

vad_verdict_t vad_feed(vad_t *v, const int16_t *pcm, size_t n)
{
    for (size_t i = 0; i < n && v->verdict == VAD_CONTINUE; i++) {
        int32_t s = pcm[i];
        v->acc += (uint64_t)(s * s);             /* ≤ 2^30 per sample, 2^39 per frame */
        if (++v->acc_n < VAD_FRAME_SAMPLES) continue;
        /* integer sum, one float log per frame: no soft-float doubles on the S3 */
        float ms = (float)v->acc * (1.f / ((float)VAD_FRAME_SAMPLES * 32768.f * 32768.f));
        float db = ms > 1e-12f ? 10.f * log10f(ms) : VAD_SILENT_DB;
        v->acc = 0; v->acc_n = 0;
        v->verdict = frame(v, db);
    }
    return v->verdict;
}

const char *vad_reason(vad_verdict_t verdict)
{
    switch (verdict) {
    case VAD_END_SILENCE:   return "silence";
    case VAD_END_NO_SPEECH: return "no_speech";
    case VAD_END_MAX:       return "max";
    default:                return NULL;
    }
}
