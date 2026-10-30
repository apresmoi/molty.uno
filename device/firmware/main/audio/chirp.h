/*
 * SPDX-License-Identifier: MIT
 *
 * chirp.h — Molty's turn acknowledgement: a short, quiet, two-note blip synthesised on the
 * device (no file, no network), played by link/link_audio.c the moment a listen that heard
 * speech ends (link_protocol.h link_ack_decide), so the human hears "got it" before any
 * agent has answered.
 *
 *   notes     G5 784 Hz (0..65 ms), then D6 1175 Hz (50..160 ms) — a rising fifth,
 *             overlapping 15 ms so it reads as one gesture, not two beeps.
 *   envelope  each note: 6 ms raised-cosine attack, exponential decay (τ 25 / 45 ms) and a
 *             raised-cosine release over its last 8 / 20 ms: no click at either edge.
 *   level     peak ≈ 0.13 FS (−17 dBFS; RMS ≈ −28 dBFS): well under speech, then the
 *             speaker's volume setting scales it like any say.
 *   length    CHIRP_SAMPLES = 160 ms at 16 kHz.
 *
 * Plain C (libm only): test/host/link_audio_pair_test.c checks length, edges, level,
 * smoothness and the two pitches.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define CHIRP_RATE     16000u
#define CHIRP_MS       160u
#define CHIRP_SAMPLES  (CHIRP_RATE / 1000u * CHIRP_MS)
#define CHIRP_PEAK     4600            /* ≈ −17 dBFS */

/* Samples [from, from + n) of the chirp into out; returns how many were written (fewer at
 * the end, 0 past it). */
size_t chirp_fill(int16_t *out, size_t from, size_t n);
