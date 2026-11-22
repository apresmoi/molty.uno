/*
 * SPDX-License-Identifier: MIT
 *
 * fluid_askview.h: a question drawn over whatever the screen shows, as the approved design: the
 * recording ring's language (creature_ring.h) on the braille dots, its words in the device's font at the panel's pixels.
 *
 *   form      a touch at the centre of the real water; where its crest first lifts a cell, the question's lines and
 *             letters there condense from dust (the crest itself a pale dust while it runs); the scene behind dims and
 *             thins into dark dust but keeps moving
 *   stand     each answer a crisp thin line (a rim arc, or a row's underline) shedding dust inward, slowly twinkling
 *   answered  a gentle real wave from the chosen line; every line and letter breaks into dust where it stands (the
 *             chosen one a moment later and brighter) and the water carries it off as it fades (~0.6 s); nothing slides
 *   ended     (declined, timed out, replaced) the rim lets go a ring-shaped wave, as the recording ring's stop, then the
 *             same dust
 *   whole     where the drawer changes its picture as dust (the device), none of the above: it is that change's new
 *             picture, formed from the first frame, its answers' lines growing outward (fluid_askview_order); answered,
 *             its choice brightens on its last frame and the next change takes it away
 * The scene behind is given as crisp dots (a rigged creature's, a scene's, or the fluid dithered) and composed with the
 * question's dots into one set of crisp dots with a colour a cell (fluid_draw_opts_t.scene): the drawer refracts them
 * through the same water. The words are 1-bit masks (fluid_letter_t.mask), one a line, over cleared cells.
 *
 * Memory: one block in PSRAM (ESP), made for a question and freed when it is gone; nothing grows after.
 * Plain C, no ESP-IDF (host-testable: test/host/askview_test.c).
 */
#pragma once

#include "fluid_ask.h"
#include "fluid_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fluid_askview fluid_askview_t;

/** For the sim's grid and the question (laid out here, fluid_ask_lay). NULL on OOM. */
fluid_askview_t *fluid_askview_create(const fluid_sim_t *s, const fluid_ask_q_t *q);
/** The same in parts, for a frame that cannot take it whole (its lines' dots are a root, an angle and a power each:
 *  hundreds of ms on the device): begin lays it out, then build works out `rows` more rows of dots a call until it
 *  returns true (the view is whole; only then start it, draw it or ask its layout's hits). */
fluid_askview_t *fluid_askview_begin(const fluid_sim_t *s, const fluid_ask_q_t *q);
bool  fluid_askview_build(fluid_askview_t *v, const fluid_sim_t *s, int rows);
/** Its layout (where its words and answers are). */
const fluid_ask_layout_t *fluid_askview_layout(const fluid_askview_t *v);
void  fluid_askview_destroy(fluid_askview_t *v);
/** The question's moments on the water: it appears (a touch at the centre), it was answered with `pick` (a gentle wave
 *  from that line) or it ended unanswered (pick < 0: the rim lets go). */
void  fluid_askview_start(fluid_askview_t *v, fluid_sim_t *s);
/** ... or it comes whole as a change of picture's new picture (fluid_draw_morph_*): formed from its first frame, and
 *  goes the same way (no wave of its own; answered, its choice brightens on its last frame) */
void  fluid_askview_start_whole(fluid_askview_t *v);
/** Per cell, when a change of picture brings its grains (fluid_draw_morph_order): its answers' lines grow outward. */
const uint8_t *fluid_askview_order(const fluid_askview_t *v);
void  fluid_askview_answer(fluid_askview_t *v, fluid_sim_t *s, int pick);
/** This frame, after the sim's steps: the question's dots, colours and letters from the screen's phase and clocks
 *  (fluid_ask_screen_t) and the water; t the frame's clock (s, any origin: the twinkles). */
void  fluid_askview_frame(fluid_askview_t *v, const fluid_sim_t *s, const fluid_ask_screen_t *scr, float t);
/** The drawer's frame (fluid_draw_opts_t.frame, FLUID_FRAME): the cell rows take turns over 3 frames while it forms or
 *  breaks up (the crest, the dust: most cells change), else 2, for the question's dots and the water's own changes
 *  alike. */
uint32_t fluid_askview_draw_frame(const fluid_askview_t *v);
/** Over the scene's crisp dots (dots, in place) and its colours (col, in place: the frame's): the scene dimmed and
 *  thinned to dark dust (primary: per cell, the creature's own dots there, which keep their dots; NULL none), the
 *  question's dots over it. */
void  fluid_askview_compose(const fluid_askview_t *v, uint8_t *dots, const uint8_t *primary, fluid_rgba16_t *col);
/** The letters for the drawer (at most max): one 1-bit mask a line of text, copied to the drawer's own copy (call with
 *  the drawer idle: the masks stay as they are until the next call). Returns how many. */
int   fluid_askview_letters(fluid_askview_t *v, fluid_letter_t *out, int max);
/** The drawer's options with the question over the scene: o's dots, colours (compose) and letters (letters) as given,
 *  drawn as crisp dots alone (no shapes, dust or fine dust), the question's interlace, its letters never melting (the
 *  frame's melt is a text state's). */
void  fluid_askview_opts(const fluid_askview_t *v, fluid_draw_opts_t *o, const uint8_t *dots, const fluid_letter_t *letters, int nletters);
/** The question's own dots this frame (cell k's braille bits), for tests. */
const uint8_t *fluid_askview_dots(const fluid_askview_t *v);

#ifdef __cplusplus
}
#endif
