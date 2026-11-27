/*
 * SPDX-License-Identifier: MIT
 *
 * wave.h: this Molty's tone player (wave_player.h) and what shares it (docs/AGENT-LINK.md "Waves"). The beat player's
 * sibling: one owner of the speaker at a time, its lock, its speaker loop (rhythm_dev.c) and its clock are the rhythm's.
 *
 *   link      wave.play / wave.set / wave.stop (cap "wave", boards with a speaker): the chord kinds once, a note, a
 *             pitch; the player holds it by itself. Playing one stops the other player (rhythm.play stops this one).
 *   faces     two. Stopped, the chooser: the rim picks the chord kind, each previewed at the pitch it last played at
 *             (middle C before any). Playing: the rim is the pitch, in fine steps with a detent on each note (and each
 *             whole hit a second), a semitone a click from C5, a whole tone from C6; turning fast, an octave a click.
 *             The note is wherever the pitch is: choosing a kind never moves it. The centre plays and stops. All of it
 *             acts here, on the device (wave_local_*), with no round trip.
 *   screen    the waves package's scope (a scene's `wave` item) draws wave_now: the clock and the voices at the sample
 *             the speaker plays now (the rhythm's delay and trim, rhythm_heard_seen), the readout, the names. The
 *             sound lives as long as its picture: no wave item on screen for WAVE_ORPHAN_S stops it, and the tables
 *             are freed once nothing plays (wave.play returns at once: they are made a piece at a time off the link
 *             task, a voice's own first, wave_build_run).
 *   events    what the human changed (wave_take_event): sense.c (web: web_device.c) sends them.
 *
 * Plain C (cJSON); the lock and the speaker's clock are rhythm.h's.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "wave_player.h"

#define WAVE_KINDS     32               /* chord kinds a library holds (the wheel's beads)                     */
#define WAVE_ID_MAX    24
#define WAVE_LABEL_MAX 24
#define WAVE_ORPHAN_S  2.f
#define WAVE_LOW_MIDI  (-36)            /* 1.02 Hz                                                             */
#define WAVE_NOTE_HZ   19.5f            /* from here the readout is hertz and a note                           */

/* the link's commands: NULL done (*out the answer), else an error code (invalid_args, not_playing, oom) */
const char *wave_play(const cJSON *args, cJSON **out);
const char *wave_set(const cJSON *args, cJSON **out);
const char *wave_stop(void);

enum { WAVE_AT_CHORDS, WAVE_AT_PLAYING };

/** What the screen shows now. */
typedef struct {
    bool on;                                /* a library is loaded and not stopped                             */
    int level;                              /* WAVE_AT_*                                                       */
    int index;                              /* the chord kind (the chooser's bead)                             */
    wav_view_t v;                           /* the player at the sample heard as this frame is seen            */
    wav_tables_t *t;                        /* its tables (freed only on the fluid task: wave_orphan)          */
    int nv;                                 /* the chord kind's voices (its dots)                              */
    uint8_t count[WAV_VOICES];              /* ... their whole numbers                                          */
    uint32_t rgb[WAV_VOICES];               /* each voice's note's colour                                      */
    char name[32];                          /* the chord kind's                                                */
    uint32_t name_rgb;
    char top[8];                            /* the note the pitch is on (or nearest), as the chord spells it   */
    uint32_t top_rgb;
    char big[32], unit[32];                 /* playing: hits a second, or hertz, and the unit with the note (and
                                             * the cents off it, between notes)                                */
    uint32_t unit_rgb;
    float tone;                             /* 0 a beat .. 1 a note (19.5 to 30 Hz)                            */
    float land;                             /* 1 on an octave's jump, down to 0                                */
} wave_screen_t;
bool wave_now(wave_screen_t *s);
/** Where the wheel is: the face (WAVE_AT_*), the chord kind, the pitch (MIDI) and the octave jump's emphasis (1 down
 *  to 0). False: nothing loaded. */
bool wave_where(int *level, int *index, float *midi, float *land);
/** Chord kind i's id ("" none). */
const char *wave_kind_id(int i, char out[WAVE_ID_MAX + 1]);
/** The chooser's labels: the chord kinds. */
int  wave_labels(char (*out)[WAVE_LABEL_MAX + 1], int max);

/** The screen's own acts. turn: clicks clockwise positive; what to feel: 0 nothing moved (an end, or a detent
 *  holding), 1 a fine step (or a chord kind), 3 it landed on a detent (a note, a whole hit a second), 2 an octave. */
enum { WAVE_TURN_NONE, WAVE_TURN_STEP, WAVE_TURN_OCTAVE, WAVE_TURN_NOTE };
int  wave_local_turn(int clicks, bool fast);
void wave_local_centre(void);
void wave_local_mute(int voice, float x, float y);
void wave_local_stop(void);
/** A library is loaded and not stopped (the buttons are its). */
bool wave_active(void);
/** Each frame on the fluid task: no wave item on screen stops it after WAVE_ORPHAN_S; stopped and silent, its tables
 *  go. dt: the frame (the landing's emphasis fades with it). */
void wave_orphan(bool shown, float dt);

typedef enum { WAVE_EV_MUTE, WAVE_EV_STATE } wave_ev_kind_t;
typedef struct {
    wave_ev_kind_t kind;
    int voice;                              /* MUTE: 1 the root                                               */
    bool on;                                /* MUTE: muted                                                    */
    float x, y;                             /* MUTE: the tap                                                  */
    char state[8];                          /* STATE: playing, paused (choosing), stopped (gone)              */
    char chord[WAVE_ID_MAX + 1], note[4];
    float hz;
} wave_ev_t;
bool wave_take_event(wave_ev_t *e);
cJSON *wave_event_frame(const wave_ev_t *e);

/* the tables are made off the link task: wave_play hands them to the host (rhythm_dev.c: a low-priority task, its stack
 * in PSRAM; the web module: at once), which runs wave_build_run and, once it ended, is reaped on the fluid task */
void wave_build_spawn(void);
void wave_build_reap(void);
void wave_build_run(void);

/* the speaker's share (rhythm_core.c, inside the rhythm's lock): samples, whether it makes them now, whether it wants
 * the speaker (it makes them, or waits for it to come back), a job's yield, and the samples made */
size_t   wave_fill_locked(int16_t *out, size_t n);
bool     wave_sounding_locked(void);
bool     wave_wants_locked(void);
void     wave_yield_locked(bool yield);
uint64_t wave_made_locked(void);

/** The readout at pitch hz from MIDI midi, the note spelt in key (0 major, 1 minor): hits a second ("6.9", "16") and
 *  "A SECOND", or from WAVE_NOTE_HZ hertz ("440", "1.05K") and "HZ A4" (between notes the nearest and its cents off:
 *  "HZ A4 +25"). */
void wave_readout(float hz, float midi, int key, char big[32], char unit[32]);
