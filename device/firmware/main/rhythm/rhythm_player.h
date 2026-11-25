/*
 * SPDX-License-Identifier: MIT
 *
 * rhythm_player.h: Molty's beat player (docs/AGENT-LINK.md "Rhythm"). A library of patterns and a tempo, sent once;
 * from then on it loops by itself and makes the sound: each drum synthesised (no stored samples), mixed, placed at its
 * exact sample.
 *
 * Plain C (libm only), no FreeRTOS / ESP-IDF: rhythm_dev.c runs it on the audio task, web/web_molty.c in the module,
 * test/host/rhythm_test.c checks it.
 *
 *   clock     one number, u: where the player is, in the pattern's units (a step is one unless `lengths` says),
 *             counted from the phrase's start; negative during the first count-in. u(n) = u0 + (n - n0) * du at
 *             output sample n, du = bpm * per / 60 / RHY_RATE. A tempo change moves the anchor (n0, u0) to the sample
 *             it lands on and changes du: nothing restarts, nothing jumps. A hit plays at the first sample whose u
 *             reaches its onset: within one sample of the ideal time, for ever (no sum of rounding errors).
 *   steps     step s starts at the sum of the lengths before it; with swing every second step starts swing x the
 *             step before it late. A bar is one turn: L units.
 *   phrase    bars in an order (`bars`, `order`), on repeat; `next` moves the phrase one bar on at the next bar line.
 *   count-in  a bar (or two) of clicks on the beats before the pattern: at the start, after a new pick, after a pause.
 *   sound     each hit starts a voice (RHY_VOICES at once, the oldest gives way): a pitched sine dropping in pitch
 *             (low, head), a high ringing pair (rim, bell), shaped noise (clap, hat, snare), a click. Accented hits
 *             play louder. The voices are summed, then a soft knee keeps the sum under full scale: never a clipped
 *             sample.
 *   mute      a muted layer's ringing voices fade out over RHY_FADE samples (no click); its next hits are silent.
 *   pause     every voice fades the same way; resume starts with a count-in, then the bar it paused in from its start.
 *   view      rhy_view: where a sample (the one the speaker plays now) is in the pattern, for the picture.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RHY_RATE    16000
#define RHY_STEPS   32
#define RHY_LAYERS  4
#define RHY_BARS    8
#define RHY_ORDER   16
#define RHY_LIB     16
#define RHY_VOICES  8
#define RHY_FADE    80                  /* samples (5 ms): a muted or paused voice's fade                      */
#define RHY_BPM_MIN 40
#define RHY_BPM_MAX 220
#define RHY_ANCHORS 8                   /* the clock's last anchors (a tempo change, a start): rhy_view looks back */
#define RHY_BEATS   8                   /* main beats played, not yet taken (rhy_take_beat)                      */
#define RHY_ID_MAX  24
#define RHY_LABEL_MAX 12
#define RHY_LAYER_ID_MAX 16

enum { RHY_LOW, RHY_HEAD, RHY_RIM, RHY_CLAP, RHY_BELL, RHY_HAT, RHY_SNARE, RHY_CLICK, RHY_SOUNDS };
extern const char *const RHY_SOUND_NAMES[RHY_SOUNDS];

typedef struct {
    char id[RHY_LAYER_ID_MAX + 1];
    uint8_t sound;
    uint32_t rgb;                       /* its colour on the clock                                             */
    uint32_t hits, accent;              /* step bits, step 1 = bit 0                                           */
} rhy_layer_t;

typedef struct {
    char id[RHY_ID_MAX + 1], label[RHY_LABEL_MAX + 1];
    uint8_t steps, nlayers, nbars, norder, count_in;
    float per;                          /* units a beat                                                        */
    float swing;                        /* 0..0.33                                                             */
    float start[RHY_STEPS + 1];         /* where each step starts, units; start[steps] = the bar's length L     */
    uint32_t beats;                     /* step bits                                                            */
    rhy_layer_t layer[RHY_LAYERS];
    uint32_t bar[RHY_BARS][RHY_LAYERS]; /* each bar's hits a layer (nbars 0: the layers' own)                   */
    uint8_t order[RHY_ORDER];
} rhy_pattern_t;

/** Step s's onset in units from the bar's start (swing included). */
double rhy_onset(const rhy_pattern_t *pt, int s);
/** Layer l's hits in phrase bar slot `slot` (the order's slot; any slot without bars). */
uint32_t rhy_bar_hits(const rhy_pattern_t *pt, int slot, int l);

typedef struct {
    uint8_t sound, layer;               /* layer 0xff: a count-in click                                        */
    bool fading;
    uint32_t t, len;                    /* samples since it started, its length                                */
    float gain, fade;
    float ph, ph2, f, f1, ksw, sw;      /* the tone: phases, pitch now and at its end, its drop                */
    float env, kenv, nenv, knenv;       /* the tone's and the noise's decays                                   */
    float lp, hp, hpx;                  /* the noise's filters                                                 */
} rhy_voice_t;

typedef void (*rhy_hit_fn)(void *ctx, uint64_t n, int layer, int step, bool accent);   /* layer -1: a click */

typedef struct {
    rhy_pattern_t lib[RHY_LIB];
    int nlib, cur;
    float bpm;
    bool on, paused, buzz;
    uint32_t mute;                      /* layer bits                                                          */
    uint64_t n;                         /* samples made                                                        */
    struct { uint64_t n0; double u0, du; } anc[RHY_ANCHORS];   /* anc[0] the clock now, then older             */
    double cin_end;                     /* u where the count-in ends                                           */
    int64_t ev_bar; int ev_step;        /* the next step to sound                                              */
    int shift;                          /* the phrase moved on this many bars (`next`)                         */
    bool next_req;
    bool stopping;                      /* fading out for a pause or a stop                                    */
    bool quit;                          /* ... a stop: off once faded                                          */
    rhy_voice_t v[RHY_VOICES];
    uint32_t noise;
    uint64_t beat_n[RHY_BEATS]; uint8_t beat_first[RHY_BEATS]; int nbeat;
    unsigned ver;                       /* bumped when what the picture draws changes: pick, mute, load        */
    rhy_hit_fn hit_fn; void *hit_ctx;   /* (tests: every hit, as it is placed)                                */
} rhy_player_t;

void rhy_init(rhy_player_t *p);
/** A library (copied) and which plays, at bpm (clamped), with layers `mute` silent; paused: at rest until resumed.
 *  Starts with the pattern's count-in. False: an empty library or a pick outside it. */
bool rhy_load(rhy_player_t *p, const rhy_pattern_t *lib, int n, int pick, float bpm, uint32_t mute, bool paused);
/** The tempo from the next sample on (clamped to RHY_BPM_MIN..MAX). */
void rhy_set_bpm(rhy_player_t *p, float bpm);
/** Another pattern of the library: what rings fades, then its count-in. */
bool rhy_pick(rhy_player_t *p, int i);
void rhy_set_mute(rhy_player_t *p, uint32_t mute);
/** The phrase one bar on, at the next bar line. */
void rhy_next(rhy_player_t *p);
/** Pause: the voices fade (rhy_fill returns the fade, then 0). Resume: a count-in, then the bar it paused in. */
void rhy_pause(rhy_player_t *p, bool pause);
void rhy_stop(rhy_player_t *p);
/** Playing and not paused (or still fading out): the caller should keep the speaker and fill. */
static inline bool rhy_sounding(const rhy_player_t *p) { return p->on && (!p->paused || p->stopping); }
/** The next n samples into out (16 kHz mono). Returns how many were made: n while it sounds, fewer at the end of a
 *  fade (the rest of out zeroed), 0 when it does not sound. */
size_t rhy_fill(rhy_player_t *p, int16_t *out, size_t n);
/** u at output sample n (one the speaker plays now: n <= p->n). */
double rhy_u_at(const rhy_player_t *p, uint64_t n);
/** A main beat placed at or before sample upto, not yet taken: true and *first (the bar's first). For the buzz. */
bool rhy_take_beat(rhy_player_t *p, uint64_t upto, bool *first);

/** What the picture shows at output sample n. */
typedef struct {
    bool on, paused, count_in;
    int cur;                            /* the pattern                                                         */
    int step;                           /* the step sounding, 0..; -1 during a count-in                       */
    int cin_step;                       /* the count-in's click on that step, -1 none                          */
    float since;                        /* s since that step (or count-in beat) began                          */
    int slot, norder;                   /* where in the phrase (0 without one)                                 */
    uint32_t hits[RHY_LAYERS];          /* this bar's hits a layer                                             */
    uint32_t mute;
    float bpm;
    unsigned ver;
} rhy_view_t;
void rhy_view(const rhy_player_t *p, uint64_t n, rhy_view_t *out);

#ifdef __cplusplus
}
#endif
