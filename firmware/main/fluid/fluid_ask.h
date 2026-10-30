/*
 * SPDX-License-Identifier: MIT
 *
 * fluid_ask.h — the screen of a question (the link's `ask`): where its 1–5 answers sit
 * on the round screen, and which one a tap picks.
 *
 * Plain C, no FreeRTOS / ESP-IDF: link/link_ask.c picks the answer for a tap,
 * mole_fluid.c lays the labels out; test/host/ask_test.c checks both.
 *
 *   sectors   n = 1: the whole screen. n ≥ 2: n equal pie sectors, dividers at 12 o'clock
 *             and every 360/n degrees clockwise (n = 2: the left / right halves). Sector k
 *             spans [k, k+1)·360/n clockwise from 12 o'clock: a point exactly on a divider
 *             belongs to the sector clockwise of it; the centre belongs to sector 0.
 *   order     option i sits in the i-th sector in reading order of the sector centres:
 *             top to bottom by row, left to right within a row. n = 3: upper left, upper
 *             right, bottom; 4: TL TR BL BR; 5: top left, top right, lower left, lower
 *             right, bottom; 2: left, right.
 *   layout    each label on its sector's bisector at a fixed fraction of the radius, one
 *             type size for all: the largest at which every label's letter box stays in
 *             its sector (FLUID_ASK_GAP px off both dividers) and inside the disc
 *             (FLUID_ASK_RIM px off the rim), capped at FLUID_ASK_FS_MAX. Letter boxes are
 *             the reader's: 0.6 em wide, 1.05 em tall (fluid_text_set_word). 8 letters fit
 *             at 27.9 px at n = 5 on the 412 px stage, above FLUID_ASK_FS_MIN (18 px, the
 *             reader's legibility floor; ask_test.c pins it). A 240 px stage gets 16 px.
 * Coordinates: px relative to the screen's centre, y down; pixel sizes scale with the
 * stage (tuned at FLUID_STAGE_REF = 412).
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLUID_ASK_MAX        5       /* options                                       */
#define FLUID_ASK_LABEL_MAX  8       /* characters per option (the relay checks it too) */
#define FLUID_ASK_FS_MIN     18.f    /* type size floor, stage px at 412              */
#define FLUID_ASK_FS_MAX     44.f    /* type size cap                                 */
#define FLUID_ASK_RIM        10.f    /* letter boxes stay this far inside the rim     */
#define FLUID_ASK_GAP        4.f     /* ...and this far off a divider                 */

/** The clockwise sector 0..n-1 of a point (dx, dy) from the centre; 0 for n ≤ 1. */
int   fluid_ask_sector(int n, float dx, float dy);
/** sector[i] = the sector option i sits in (reading order); n clamped to 1..FLUID_ASK_MAX. */
void  fluid_ask_order(int n, int sector[FLUID_ASK_MAX]);
/** The option (0..n-1) a tap at (dx, dy) from the centre picks. */
int   fluid_ask_pick(int n, float dx, float dy);
/** Lay out n labels of lens[i] characters on a stage of `stage` px: cx/cy = the centre of
 *  option i's label (stage px from the top left), returns the type size (stage px). */
float fluid_ask_layout(int n, const int *lens, int stage, float *cx, float *cy);
/** Whether a len-letter label of type size fs centred at (x, y) (px from the centre) fits
 *  sector k of n on a disc of radius r px (rim and gap scaled by sc = stage / 412). */
bool  fluid_ask_fits(int n, int k, int len, float fs, float x, float y, float r, float sc);

/* ── The question on screen, frame by frame ─────────────────────────────────────
 * mole_fluid.c follows what the link wants (mole_fluid_ask_*) with this once per frame,
 * and tells the touch path which question a tap on the frame it drew may answer. */
enum { FLUID_ASK_NONE, FLUID_ASK_SHOW, FLUID_ASK_PICK };
#define FLUID_ASK_HOLD_S  0.8f   /* a picked answer stays this long                          */
#define FLUID_ASK_READY   0.6f   /* t·speed at which the labels are condensed (the hold ramp,
                                  * (t·speed − 0.2) / 0.4, reaches 1): readable, tappable     */
typedef struct {
    int phase, pick;             /* FLUID_ASK_*; the option picked (PICK)                     */
    unsigned seq;                /* the question it follows (mole_fluid_ask_show's seq)       */
    float t, pick_t;             /* s shown, s since the pick (the caller advances both)      */
    bool requested;              /* a state / mood request came while it was up               */
} fluid_ask_screen_t;
enum { FLUID_ASK_DID_SHOW = 1, FLUID_ASK_DID_PICK = 2, FLUID_ASK_DID_LEAVE = 4 };
/** One frame: the link wants question `seq` (`active`), answered with option `pick` (≥ 0,
 *  of n) or not yet; `requested` = an explicit state / mood request arrived this frame.
 *  Returns what changed, in this order: SHOW (a new question replaces any), PICK, LEAVE
 *  (ended unanswered, or the pick's hold is over / cut short by a request). On LEAVE, *keep
 *  = a request came while the question was up: the screen goes to it, not to the creature. */
int fluid_ask_follow(fluid_ask_screen_t *s, bool active, unsigned seq, int pick, int n, bool requested, bool *keep);
/** The question a tap on a frame of this screen may answer: shown, not yet answered, its
 *  labels condensed at this speed (cfg speed) — else 0 (nothing, another question's
 *  letters still morphing, or a pick on its way out). */
unsigned fluid_ask_tappable(const fluid_ask_screen_t *s, float speed);

#ifdef __cplusplus
}
#endif
