/*
 * SPDX-License-Identifier: MIT
 *
 * wave_player.h: Molty's tone player (docs/AGENT-LINK.md "Waves"), beside the beat player and sharing its speaker. A
 * chord of up to WAV_VOICES voices, every one the beat player's own low drum (rhythm_player.c RHY_LOW) with a beater's
 * click on it, hit once a cycle: slow it is a rhythm, fast the hits fuse into a note. Every value that moves is glided.
 *
 * Plain C (libm only), no FreeRTOS / ESP-IDF: wave_core.c runs it on the audio task, web/web_molty.c in the module,
 * test/host/wave_test.c checks it.
 *
 *   the hit   the low drum and a 1.8 kHz click dying in 1.5 ms, rendered once into a table (WAV_HIT_LEN samples at
 *             16 kHz), band-limited under WAV_TOP. Each new hit of a voice chokes the one before (it fades over
 *             WAV_CHOKE samples, or half a cycle if that is shorter), as a drum machine's voice does when hit again.
 *   two ways  under WAV_DIRECT_HI the hit table itself, from each hit; over WAV_TABLE_LO one cycle of that same choked
 *             waveform for each semitone from 24.5 Hz to 6 kHz, cut at the harmonics under WAV_TOP (a band-limited
 *             wavetable, made by FFT: nothing aliases at 16 kHz), read by the voice's phase and blended between the
 *             two nearest semitones. Between the two they are crossfaded: the same waveform at the same phase.
 *   clock     one phase H, a turn of the clock (the chord's shared cycle), 2^32 a turn. Whole-number tuning: voice i's
 *             phase is n_i * H, exactly (the chord stands still and loops); equal tuning: each voice its own phase.
 *   level     a gain over pitch (each to a 0.45 peak or a 0.2 RMS, whichever first), held (5 ms in, 20 ms out) or
 *             plucked for a preview (5 ms in, a 0.45 s fall, strummed 18 ms a voice). A restart, a tuning switch or a
 *             pluck over a ringing one ducks the sum to silence first (about 12 ms).
 *   mix       summed, times 1.25 / voices, the beat player's soft knee, int16 at 32000 full scale.
 *   tables    wav_tables_make: the hit and the cycles, about 430 KB, PSRAM on the device, made once when a chord is
 *             first sent (about 0.4 s) and freed when nothing plays them (wave_core.c).
 *   view      wav_view: the clock, each voice's phase, pitch and level at an output sample (the one the speaker plays
 *             now), from snapshots the fill keeps; wav_trace: what the hand drew round the circle from it (the picture).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <math.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAV_RATE      16000
#define WAV_VOICES    5
#define WAV_MAX_N     48                /* a voice's whole number: its hits a turn                             */
#define WAV_TOP       7200.f            /* Hz: the band every sound stays under                                */
#define WAV_MIN_HZ    1.f
#define WAV_MAX_HZ    5760.f
#define WAV_HIT_LEN   7200
#define WAV_CHOKE     32                /* samples: a hit choking the one before                               */
#define WAV_TAB_N     1024              /* samples a cycle                                                     */
#define WAV_TAB_LO    19                /* MIDI 19, 24.5 Hz: the lowest cycle                                  */
#define WAV_TABS      97                /* ... to MIDI 115                                                     */
#define WAV_DIRECT_HI 24.5f             /* Hz: under this the hit table alone                                  */
#define WAV_TABLE_LO  32.f              /* ... over this the cycles alone                                      */
#define WAV_VIEWS     32                /* snapshots for the picture, WAV_VIEW_EVERY samples apart (256 ms)    */
#define WAV_VIEW_EVERY 128

typedef struct {
    float hit[WAV_HIT_LEN + 2], hit_peak;
    float tab[WAV_TABS][WAV_TAB_N + 1], peak[WAV_TABS];
} wav_tables_t;
/** The hit and the cycles (PSRAM on the device); NULL without memory. */
wav_tables_t *wav_tables_make(void);
void wav_tables_free(wav_tables_t *t);

/** Pitch: equal temperament, A4 = 440 Hz. */
static inline float wav_hz(float midi) { return 440.f * exp2f((midi - 69.f) / 12.f); }

enum { WAV_HOLD, WAV_PLUCK };
enum { WAV_JUST, WAV_EQUAL };

typedef struct {
    uint8_t n, mode;                    /* its whole number; WAV_HOLD or WAV_PLUCK                             */
    int8_t semi;                        /* semitones over the root (equal tuning)                              */
    bool mute;
    uint32_t ph, lastph, dph;           /* phase (2^32 a cycle), the last sample's, a sample's step            */
    float s, sp, pg;                    /* samples since its hit, since the hit it chokes, that one's level    */
    float lf, lft, f;                   /* log2 of its pitch now and where it glides; its pitch                */
    float env, envT;
    int t, wait;                        /* a pluck: samples since it began, still to wait (the strum)          */
    float g, wt, d, tu;                 /* control rate: gain, the cycles' share, the choke, the cycles' blend */
    int tm;                             /* ... and which cycle                                                 */
} wav_voice_t;

typedef struct {
    int n;                              /* voices                                                              */
    uint8_t count[WAV_VOICES];          /* whole numbers, root first                                           */
    int8_t semi[WAV_VOICES];            /* semitones over the root                                             */
} wav_chord_t;

typedef struct { uint64_t n; uint32_t H; float fc; uint32_t ph[WAV_VOICES]; float f[WAV_VOICES], env[WAV_VOICES]; } wav_snap_t;

typedef struct {
    const wav_tables_t *t;              /* NULL: silent                                                        */
    wav_voice_t v[WAV_VOICES];
    wav_chord_t ch;
    uint8_t tuning;
    bool hold;                          /* a chord is held (playing): samples are made even when all are muted */
    bool yield;                         /* the speaker is wanted by a job: it ducks out and waits              */
    uint32_t H, dH;
    float fc, root, duck, duckT;
    struct { bool on; wav_chord_t ch; float root; uint8_t mode, tuning; bool fresh, retune; } later;   /* at silence */
    uint64_t n;                         /* samples made                                                        */
    wav_snap_t snap[WAV_VIEWS];
    int nsnap;
} wav_player_t;

void wav_init(wav_player_t *p, const wav_tables_t *t);
/** Chord ch from root (Hz, clamped), held or plucked. fresh: from its hit, after the sum ducks to silence (a pluck
 *  always is); else the voices glide to it. */
void wav_chord(wav_player_t *p, const wav_chord_t *ch, float root, int mode, int tuning, bool fresh);
/** The root's pitch, glided (30 ms), never restarted. */
void wav_set_root(wav_player_t *p, float root);
/** Voices muted, a bit each (root = bit 0): they fade out (20 ms) and in (5 ms). */
void wav_set_mute(wav_player_t *p, uint32_t mask);
/** Whole numbers or the piano's: switched at silence. */
void wav_set_tuning(wav_player_t *p, int tuning);
/** Everything fades out; nothing is held. */
void wav_silence(wav_player_t *p);
/** The speaker is wanted (it ducks out, then makes nothing), or free again. */
void wav_yield(wav_player_t *p, bool yield);
/** It makes samples: a chord held, a pluck ringing, a change waiting for silence. */
bool wav_active(const wav_player_t *p);
/** The next n samples into out (16 kHz mono): n while it sounds, fewer at the end (the rest zeroed), 0 when silent. */
size_t wav_fill(wav_player_t *p, int16_t *out, size_t n);
/** The highest root (MIDI) whose top voice stays under WAV_MAX_HZ, for ch in that tuning. */
int wav_top_midi(const wav_chord_t *ch, int tuning);

/** What the picture shows at an output sample. */
typedef struct {
    bool on;                            /* snapshots exist                                                     */
    bool hold;
    int nv;
    uint8_t n[WAV_VOICES];
    uint32_t mute;
    float H;                            /* the clock, turns 0..1                                               */
    float fc;                           /* turns a second                                                      */
    float ph[WAV_VOICES], f[WAV_VOICES], env[WAV_VOICES];
    float to[WAV_VOICES];               /* the pitch each glides to (the trace's: a glide redraws nothing)     */
    bool pluck[WAV_VOICES];
    bool just;                          /* whole-number tuning                                                 */
} wav_view_t;
void wav_view(const wav_player_t *p, uint64_t n, wav_view_t *out);

/** The outline the hand drew, as the picture shows it: npts + 1 radii round the circle from 12 o'clock clockwise
 *  (out[npts] = out[0]), the circle's own radius rc pushed out by amp times the sound (each voice's real cycle at the
 *  pitch it glides to, at the phase it had when the hand passed), fading behind the hand over persist s. held (0..1):
 *  how far the beats are a note (the afterglow holds whole from 1). Not held (a preview): each voice's cycle, still. */
void wav_trace(const wav_tables_t *t, const wav_view_t *v, float held, float rc, float amp, float persist, float *out, int npts);

#ifdef __cplusplus
}
#endif
