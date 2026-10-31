/* fluid_ring.h — how a listen looks and moves: EVERY tuning knob in one place.
 *
 * A circle of the fluid (fluid_target_ring, a target like the creature's) sits fixed just inside
 * the rim, in ring_color. It never moves. Each syllable-like rise of the voice raises the water
 * all along that circle at once (fluid_ring_ping), so one round wave leaves it and runs across
 * the whole screen toward the centre, like a fingertip touching the entire rim. A quiet room
 * gets a faint, slow one, so a silent listen still reads as awake.
 *
 * Units: radii and lengths in stage units (1 = the rim; at 412 px one unit = 206 px, and
 * on the 52×32 grid a cell is ≈ 0.038 wide × 0.0625 tall). Level = the VAD mic level
 * 0..1 (vad_level: the loudest moment of each ~100 ms chunk over the room's floor; room ≈ 0,
 * close speech ≈ 1). Syllables run ~4 Hz, so 10 Hz peaks do show their rises and dips.
 * Tune on the device: change a value, `make firmware`, flash, listen. */
#pragma once

/* ── the circle: fixed radius, no breathing ─────────────────────────────────── */
#define RING_R            0.875f   /* centre line, of the radius                          */
#define RING_CORE         0.35f    /* full-ink core, half-width in CELLS along the normal  */
#define RING_SOFT         1.00f    /* soft falloff beyond the core, in cells               */
#define RING_INK          0.22f    /* target ink (the creature is ~1, the dim background ~0.1) */
#define RING_INK_LEVEL    0.06f    /* + this at full voice: brighter, never wider          */
#define RING_PIN          1.70f    /* what shows is at least ink × this (fluid_ring_pin): a wave's
                                    * trough darkens a cell, and the circle must not blink with it */

/* ── the voice ─────────────────────────────────────────────────────────────── */
#define RING_ATTACK_S     0.08f    /* ink's level smoothing toward louder                  */
#define RING_DECAY_S      0.45f    /* ...toward quieter                                    */
#define RING_ONSET_MIN    0.22f    /* below this level it is the room, not a voice          */
#define RING_ONSET_RISE   0.16f    /* a syllable: the level rose this much over its recent low */
#define RING_LOW_RISE_S   0.40f    /* the recent low creeps up to the level this slowly     */
#define RING_GAP_S        0.38f    /* never two waves closer than this    */
#define RING_HOLD_S       0.70f    /* a voice with no rises (a held vowel, a clipped level)
                                    * still sends one this often                                */

/* ── the waves: the whole circle rises at once, radially symmetric ───────────── */
#define RING_PING_IN      0.10f    /* the bump rises this far inside the line: on the line itself it
                                    * smeared the circle's ink into a wide band that flashed */
#define RING_PING_WIDTH   0.07f    /* the bump's radial width, stage units (a cell row is 0.0625) */
#define RING_PING_S_MIN   0.20f    /* a soft syllable's height (fluid_sim_impulse units)    */
#define RING_PING_S_MAX   0.55f    /* full voice                                            */
#define RING_QUIET_S      3.50f    /* quiet room: one faint wave this often                 */
#define RING_QUIET_PING_S 0.10f    /* ...this high                                          */
/* Frame budget: a wave crossing the whole screen redraws glyph cells everywhere, and redrawn
 * cells are what the device pays for; height and rate are the expensive knobs. Host bench, a
 * blob and 40 s of phrases at 4 Hz syllables, cells per frame: 884 with these values at ~1.6
 * waves/s, against 592 for interior drops and 299 with no listen; a quiet room costs 394 against
 * 321. Device fps while spoken to is unmeasured: watch it, and lower RING_PING_S_MAX first. */

/* ── presence ──────────────────────────────────────────────────────────────── */
#define RING_FADE_S       0.3f     /* fade in when a listen starts / melt out when it ends */
