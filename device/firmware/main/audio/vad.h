/*
 * SPDX-License-Identifier: MIT
 *
 * vad.h — end-of-speech detection for `listen` (energy VAD on 16 kHz PCM16).
 *
 * Plain C, no FreeRTOS / ESP-IDF: link/link_audio.c feeds it the mic chunks as they
 * are captured; test/host/vad_test.c feeds it synthetic signals.
 *
 *   frame     20 ms (320 samples); energy = RMS in dBFS (digital silence → −120).
 *   floor     the noise floor: the minimum frame energy over the last floor_window_ms
 *             (minimum statistics). It is first valid after calib_ms: the first
 *             frames are measured and kept, then classified retroactively against
 *             that floor the moment it exists, so a short reply that starts (or even
 *             ends) inside calibration still counts as speech. It drops at once to a quieter
 *             frame and rises only as louder frames age out of the window — so a
 *             steady fan becomes the floor after floor_window_ms, while the gaps
 *             between words keep it down during speech. Until floor_window_ms the
 *             floor is capped at init_floor_max_dbfs: speech already running at the
 *             start would otherwise calibrate as the floor and never count.
 *   voiced    energy > floor + on_db, or > floor + off_db while already voiced
 *             (hysteresis: a softer tail of the same utterance stays voiced), and
 *             always > abs_min_dbfs.
 *   phases    waiting → speech (voiced) → trailing (unvoiced after speech) → speech …
 *   end       VAD_END_SILENCE   ≥ min_speech_ms voiced in total, then end_silence_ms of
 *                               continuous unvoiced frames (and ≥ min_record_ms in all);
 *             VAD_END_NO_SPEECH no_speech_ms elapsed with < min_speech_ms voiced and
 *                               nothing voiced right now;
 *             VAD_END_MAX       max_ms of audio fed (the `listen` window).
 *             With enabled = false only VAD_END_MAX fires (the old full-window record).
 *             A verdict is sticky: once ended, vad_feed keeps returning it.
 *   speech_ms only meaningful when vad_speech_known(): VAD on and calibration done.
 *             Otherwise nothing was classified and the voiced total is unknown (not 0).
 *
 * Every constant is in vad_cfg_t; VAD_CFG_DEFAULT holds the shipped values.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VAD_RATE          16000
#define VAD_FRAME_MS      20
#define VAD_FRAME_SAMPLES (VAD_RATE * VAD_FRAME_MS / 1000)
#define VAD_BLOCK_FRAMES  5                       /* the floor ring holds 100 ms block minima */
#define VAD_RING_MAX      50                      /* ≤ 5 s floor window */
#define VAD_CALIB_MAX     50                      /* ≤ 1 s calibration (frames kept for replay) */

typedef struct {
    bool     enabled;          /* false: record the full max_ms window */
    uint32_t max_ms;           /* the listen window (cap) */
    uint32_t end_silence_ms;   /* continuous silence after speech that ends the listen */
    uint32_t no_speech_ms;     /* give up when nobody spoke by then */
    uint32_t min_speech_ms;    /* voiced total before a silence may end it (clicks, taps) */
    uint32_t min_record_ms;    /* never end on silence before this */
    uint32_t calib_ms;         /* first frames: floor measurement, classified once it exists (≤ 1000) */
    uint32_t floor_window_ms;  /* minimum-statistics window of the floor (≤ 5000) */
    float    on_db;            /* above floor: unvoiced → voiced */
    float    off_db;           /* above floor: stays voiced (≤ on_db) */
    float    abs_min_dbfs;     /* never voiced below this level */
    float    init_floor_max_dbfs; /* floor ceiling before floor_window_ms has passed */
} vad_cfg_t;

/* Shipped values. Mic stream: gain 8 on the 1.46 MSM mic; ambient room noise lands
 * around −65..−50 dBFS there, close speech −35..−15 dBFS. */
#define VAD_CFG_DEFAULT { .enabled = true, .max_ms = 10000, .end_silence_ms = 700, \
    .no_speech_ms = 4000, .min_speech_ms = 160, .min_record_ms = 600, .calib_ms = 300, \
    .floor_window_ms = 3000, .on_db = 10.f, .off_db = 5.f, .abs_min_dbfs = -60.f, \
    .init_floor_max_dbfs = -50.f }

typedef enum { VAD_CONTINUE = 0, VAD_END_SILENCE, VAD_END_NO_SPEECH, VAD_END_MAX } vad_verdict_t;
typedef enum { VAD_WAITING = 0, VAD_SPEECH, VAD_TRAILING } vad_phase_t;

typedef struct {
    vad_cfg_t     cfg;
    vad_phase_t   phase;
    vad_verdict_t verdict;
    uint32_t      t_ms;          /* audio fed, in whole frames */
    uint32_t      speech_ms;     /* voiced total */
    uint32_t      silence_ms;    /* current run of unvoiced frames after speech */
    float         floor_db;
    float         last_db;
    float         peak_db;       /* loudest frame since the last vad_level()          */
    /* partial frame carried between feeds */
    uint64_t      acc;
    uint32_t      acc_n;
    /* floor: ring of 100 ms block minima */
    float         ring[VAD_RING_MAX];
    uint32_t      ring_len, ring_pos, block_n;
    float         block_min;
    /* calibration frames, replayed through the classifier once the floor exists */
    float         calib_db[VAD_CALIB_MAX];
    uint32_t      calib_n;
} vad_t;

/* cfg NULL → VAD_CFG_DEFAULT. Out-of-range values are clamped. */
void vad_init(vad_t *v, const vad_cfg_t *cfg);
/* Feed n samples (any length; partial frames carry over). Returns the verdict. */
vad_verdict_t vad_feed(vad_t *v, const int16_t *pcm, size_t n);
/* true when speech_ms is a measurement: VAD enabled and calibration over. A listen
 * that ends earlier (or runs with VAD off) has an unknown voiced total. */
bool vad_speech_known(const vad_t *v);
/* The mic level for the screen's listening ring: the loudest frame since the previous
 * call, VAD_LEVEL_FLOOR_DB above the noise floor = 0 ... + VAD_LEVEL_SPAN_DB = 1 (close
 * speech reads ~1, the room 0). Works with the VAD off too. Resets the peak. */
#define VAD_LEVEL_FLOOR_DB 6.f
#define VAD_LEVEL_SPAN_DB  24.f
float vad_level(vad_t *v);
/* "silence" / "no_speech" / "max" (NULL while VAD_CONTINUE). */
const char *vad_reason(vad_verdict_t verdict);
