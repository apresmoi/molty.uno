/*
 * SPDX-License-Identifier: MIT
 *
 * fluid_ask.h — a question on the round screen (a package install, an agent's ask, a firmware update, a pairing code):
 * what it says, where its answers sit, which one a tap picks, and its screen's phases. The approved design is
 * device/creatures/ask-options.html ("Rim arcs" for 1..3 answers, "Underlined rows" for 4..5); fluid_askview.h draws it.
 *
 * Plain C, no FreeRTOS / ESP-IDF, no fluid: link/link_ask.c and link/link_pair.c pick the answer for a tap,
 * mole_fluid.c follows the phases; test/host/ask_test.c checks all of it.
 *
 *   header    the title (up to 40 letters, wrapped to two lines when one would be too small), a smaller detail line
 *             and a code (a pairing's digits), centred, from near the top down; the device's 5x7 font, capitals
 *   arcs      1..3 answers: arcs of the rim splitting its lower half, option 0 on the left (clockwise from 6 o'clock
 *             going right to left: the page's reading order), each label inside its arc
 *   rows      4..5 answers: rows under the header, each label underlined; a faint ring round the rim
 *   labels    up to fluid_ask_label_max(n) letters: 14 for 1..2 answers, 10 for 3, 8 for 4..5 (the relay checks the same)
 * Coordinates: glass units, -1..1 across the stage, y down; font pixel sizes in stage px at FLUID_STAGE_REF (412).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLUID_ASK_MAX        5       /* options                                             */
#define FLUID_ASK_LABEL_MAX  14      /* letters a label may have at most (1..2 answers)     */
#define FLUID_ASK_TITLE_MAX  40
#define FLUID_ASK_DETAIL_MAX 36
#define FLUID_ASK_CODE_MAX   6       /* digits                                              */
#define FLUID_ASK_ARCS_MAX   3       /* up to this many answers on rim arcs; more on rows   */
#define FLUID_ASK_PX_MIN     1.6f    /* a label's font pixel floor, stage px at 412 (11 px tall letters) */

/** The longest label n answers may have: 14 (1..2), 10 (3), 8 (4..5). */
int fluid_ask_label_max(int n);

/** Default colours by option index: green, red, amber, blue, violet (relay COLORS). */
extern const uint32_t FLUID_ASK_COLORS[FLUID_ASK_MAX];
/** Option i's colour: `given` (0xRRGGBB) when >= 0, else FLUID_ASK_COLORS[i]. */
uint32_t fluid_ask_color(int32_t given, int i);

/** What is asked. Strings may be empty (no title, detail or code); labels 1..fluid_ask_label_max(n) letters. */
typedef struct {
    char title[FLUID_ASK_TITLE_MAX + 1], detail[FLUID_ASK_DETAIL_MAX + 1], code[FLUID_ASK_CODE_MAX + 1];
    int n;
    char label[FLUID_ASK_MAX][FLUID_ASK_LABEL_MAX + 1];
    uint32_t rgb[FLUID_ASK_MAX];                /* each option's colour, 0xRRGGBB */
} fluid_ask_q_t;

/* ── The layout ───────────────────────────────────────────────────────────────── */
enum { FLUID_ASK_TITLE, FLUID_ASK_DETAIL, FLUID_ASK_CODE, FLUID_ASK_LABEL };
#define FLUID_ASK_ITEMS   (2 + 1 + 1 + FLUID_ASK_MAX)
#define FLUID_ASK_STR     (FLUID_ASK_TITLE_MAX + 1)
/** A line of text: its letters (capitals), centre (x, y), font pixel px (412 stage px), colour, role, option (-1). */
typedef struct { char str[FLUID_ASK_STR]; int len; float x, y, px; uint32_t rgb; uint8_t role; int8_t opt; } fluid_ask_item_t;
enum { FLUID_ASK_ARC, FLUID_ASK_RING, FLUID_ASK_HLINE };
/** A crisp line with dust thinning off one side: an arc of radius a from b to c (radians clockwise from 12), a ring
 *  of radius a, or a line at y = a from x = b to c. (cx, cy): where an accept's wave starts. opt -1: no option's. */
typedef struct { uint8_t kind; int8_t opt; float a, b, c, cx, cy; uint32_t rgb; } fluid_ask_target_t;
/** Where the answers are, for a tap (small: kept by the link for the question it waits on). */
typedef struct {
    int n;
    bool rows;                                  /* underlined rows (else rim arcs)                 */
    float top, rowh;                            /* rows: where the first starts, each one's height  */
} fluid_ask_hit_t;
typedef struct {
    fluid_ask_hit_t hit;
    int nitems, ntargets;
    fluid_ask_item_t item[FLUID_ASK_ITEMS];
    fluid_ask_target_t tg[FLUID_ASK_MAX + 1];
} fluid_ask_layout_t;

/** Lay the question out (n clamped to 1..FLUID_ASK_MAX; a label longer than its limit is cut to it). */
void fluid_ask_lay(const fluid_ask_q_t *q, fluid_ask_layout_t *L);
/** The option (0..n-1) a tap at (X, Y) glass units picks, -1 for none (the centre, the upper half, between rows). */
int  fluid_ask_pick(const fluid_ask_hit_t *h, float X, float Y);
/** q's answers for a tap (fluid_ask_lay's hit; the layout itself is on the stack for the call). */
fluid_ask_hit_t fluid_ask_hit(const fluid_ask_q_t *q);

/* ── The question on screen, frame by frame ─────────────────────────────────────
 * mole_fluid.c follows what the link wants (mole_fluid_ask_*) with this once per frame and tells the touch path which
 * question a tap on the frame it drew may answer. SHOW: forming where the centre tap's crest passes (FLUID_ASK_FORM_S),
 * then standing; PICK: answered, its lines and letters breaking into dust the water carries off; OUT: it ended
 * unanswered (declined, timed out, replaced), the rim letting go as the recording ring's stop, then the same dust.
 * Only SHOW owns the screen (the director's transitions wait under it); the dust goes over whatever comes next. */
enum { FLUID_ASK_NONE, FLUID_ASK_SHOW, FLUID_ASK_PICK, FLUID_ASK_OUT };
#define FLUID_ASK_FORM_S  1.6f   /* s from the tap to standing                                  */
#define FLUID_ASK_GONE_S  1.3f   /* s an answered or ended question takes to blow away         */
#define FLUID_ASK_READY   0.6f   /* t·speed from which a tap answers                            */
typedef struct {
    int phase, pick;             /* FLUID_ASK_*; the option picked (PICK)                       */
    unsigned seq;                /* the question it follows (mole_fluid_ask_show's seq)         */
    float t, out_t;              /* s shown, s since it was answered or ended (the caller advances both) */
} fluid_ask_screen_t;
enum { FLUID_ASK_DID_SHOW = 1, FLUID_ASK_DID_PICK = 2, FLUID_ASK_DID_LEAVE = 4, FLUID_ASK_DID_GONE = 8 };
/** One frame: the link wants question `seq` (`active`), answered with option `pick` (>= 0, of n) or not yet.
 *  Returns what changed: SHOW (a new question replaces any), PICK (answered), LEAVE (ended unanswered), GONE (its dust
 *  has blown away: NONE). */
int fluid_ask_follow(fluid_ask_screen_t *s, bool active, unsigned seq, int pick, int n);
/** The question a tap on a frame of this screen may answer: shown, not yet answered, formed enough at this speed
 *  (cfg speed), else 0. */
unsigned fluid_ask_tappable(const fluid_ask_screen_t *s, float speed);

#ifdef __cplusplus
}
#endif
