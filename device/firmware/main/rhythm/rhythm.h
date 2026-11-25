/*
 * SPDX-License-Identifier: MIT
 *
 * rhythm.h: this Molty's one beat player (rhythm_player.h) and what shares it (docs/AGENT-LINK.md "Rhythm").
 *
 *   link      rhythm.play / rhythm.set / rhythm.stop (cap "rhythm", boards with a speaker): the library and the tempo
 *             once; the player loops by itself.
 *   speaker   the audio task plays it between its jobs (rhythm_dev.c): a say, a listen or the chirp after it takes the
 *             speaker, the rhythm fades out within RHY_FADE samples and waits; after the job it starts again with a
 *             count-in at the bar it stopped in. One owner of the speaker at a time, as ever.
 *   screen    the rhythm package's clock (a scene's `beat` item) draws the pattern from rhythm_now, at the sample the
 *             speaker plays now; its wheel and its dots act here (rhythm_local_*), with no round trip. The sound lives
 *             as long as its picture: when no scene with a beat shows for RHYTHM_ORPHAN_S, it stops.
 *   buttons   while a rhythm shows: accept pauses and resumes, reject stops it and the scene goes (sense.c).
 *   events    what the human changed, once it settled (rhythm_take_event): sense.c (web: web_device.c) sends them.
 *
 * rhythm_core.c is plain C (cJSON) and builds on the device, in the web module and in the host tests; the device's lock
 * and its speaker clock come from rhythm_dev.c, the module's from web_molty.c (rhythm_lock / rhythm_unlock /
 * rhythm_heard).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "rhythm_player.h"

#define RHYTHM_ORPHAN_S 2.f             /* a beat with no clock on screen this long stops                        */
#define RHYTHM_BUZZ_LEAD 480            /* samples (30 ms): the motor is started this early, to spin up          */

/* the link's commands: NULL done (*out the answer), else an error code (invalid_args, not_playing) */
const char *rhythm_play(const cJSON *args, cJSON **out);
const char *rhythm_set(const cJSON *args, cJSON **out);
const char *rhythm_stop(void);

/** The pattern playing and where the speaker is in it now. False: no rhythm loaded. */
bool rhythm_now(rhy_view_t *v, rhy_pattern_t *pt);
/** The library's labels (the wheel's names), how many. */
#define RHYTHM_LABEL_ROOM 25            /* a label as a wheel holds it (PKG_KEY_MAX + 1) */
int  rhythm_labels(char (*out)[RHYTHM_LABEL_ROOM], int max);
/** The library's rhythm i's id ("" none). */
const char *rhythm_id(int i, char out[RHY_ID_MAX + 1]);
/** The screen's own acts. A mute, a pause and a stop are sent to the agent (rhythm_take_event); a tempo or a pick
 *  set by the wheel is sent by the wheel when the finger leaves (a `ring` event, mole_fluid.c). */
void rhythm_local_mute(int layer, float x, float y);   /* toggles that layer; x, y: the tap (0..1 of the panel) */
void rhythm_local_bpm(float bpm);
void rhythm_local_pick(int i);
void rhythm_local_pause(void);              /* toggles */
void rhythm_local_stop(void);
/** A rhythm is loaded and not stopped (the buttons are its). */
bool rhythm_active(void);
/** No clock on screen (the fluid task, each frame): it stops after RHYTHM_ORPHAN_S. */
void rhythm_orphan(bool shown, float dt);

typedef enum { RHY_EV_MUTE, RHY_EV_STATE } rhythm_ev_kind_t;
typedef struct {
    rhythm_ev_kind_t kind;
    int bpm;
    bool on;                                /* MUTE: muted                                                         */
    float x, y;                             /* MUTE: the tap                                                       */
    char id[RHY_ID_MAX + 1];                /* the rhythm (STATE) or the layer (MUTE)                              */
    char state[8];                          /* STATE: playing, paused, stopped                                     */
} rhythm_ev_t;
bool rhythm_take_event(rhythm_ev_t *e);
/** That event as the link's frame. */
cJSON *rhythm_event_frame(const rhythm_ev_t *e);

/** Its sound: n samples while it plays (rhythm_player.h rhy_fill). */
size_t rhythm_fill(int16_t *out, size_t n);
bool   rhythm_sounding(void);
/** The speaker is wanted by a job (yield: it fades and waits), or free again (it comes back with a count-in). */
void   rhythm_yield(bool yield);
/** It sounds, or waits for the speaker to come back: the audio task should play it between jobs. */
bool   rhythm_wants_speaker(void);
/** A main beat heard by sample `upto` (the buzz is on): *first its bar's first. */
bool   rhythm_take_buzz(uint64_t upto, bool *first);
uint64_t rhythm_made(void);                 /* samples made so far                                                 */

/* the host's: a lock over the player (any task), and the sample the speaker plays now */
void rhythm_lock(void);
void rhythm_unlock(void);
uint64_t rhythm_heard(void);
