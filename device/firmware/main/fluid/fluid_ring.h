/* fluid_ring.h — how a listen looks and moves: EVERY tuning knob in one place.
 *
 * Molty shows it is listening on the water itself. Each syllable-like rise of the voice
 * drops one touch-style impulse (fluid_sim_impulse, what a fingertip does in poll_touch) at
 * a new spot, so circular ripples spread and overlap as you speak. A quiet room gets a small
 * slow drop, so a silent listen still reads as awake. Around it a nearly invisible tinted line
 * (fluid_target_ring, a target like the creature's) marks the rim and carries ring_color.
 *
 * Units: radii and lengths in stage units (1 = the rim; at 412 px one unit = 206 px, and
 * on the 52×32 grid a cell is ≈ 0.038 wide × 0.0625 tall). Level = the VAD mic level
 * 0..1 (vad_level: the loudest moment of each ~100 ms chunk over the room's floor; room ≈ 0,
 * close speech ≈ 1). Syllables run ~4 Hz, so 10 Hz peaks do show their rises and dips.
 * Tune on the device: change a value, `make firmware`, flash, listen. */
#pragma once

/* ── the line: a whisper of the fluid just inside the rim ─────────────────────── */
#define RING_R            0.875f   /* centre line, of the radius                          */
#define RING_CORE         0.10f    /* full-ink core, half-width in CELLS along the normal  */
#define RING_SOFT         1.60f    /* soft falloff beyond the core, in cells               */
#define RING_INK          0.11f    /* target ink (the creature is ~1, the dim background ~0.1) */
#define RING_INK_LEVEL    0.03f    /* + this at full voice                                 */

/* ── the voice ─────────────────────────────────────────────────────────────── */
#define RING_ATTACK_S     0.08f    /* ink's level smoothing toward louder                  */
#define RING_DECAY_S      0.45f    /* ...toward quieter                                    */
#define RING_ONSET_MIN    0.22f    /* below this level it is the room, not a voice          */
#define RING_ONSET_RISE   0.16f    /* a syllable: the level rose this much over its recent low */
#define RING_LOW_RISE_S   0.40f    /* the recent low creeps up to the level this slowly     */
#define RING_GAP_S        0.30f    /* never two drops closer than this    */
#define RING_HOLD_S       0.55f    /* a voice with no rises (a held vowel, a clipped level)
                                    * still drops this often                                */

/* ── the drops: the same impulse as a touch (poll_touch: 1.6, radius 0.09) ────── */
#define RING_DROP_R       0.09f    /* impulse radius, stage units                           */
#define RING_DROP_S_MIN   0.50f    /* a soft syllable's height (fluid_sim_impulse units)    */
#define RING_DROP_S_MAX   1.60f    /* full voice: exactly a fingertip                       */
#define RING_DROP_REACH   0.72f    /* drops land within this radius of the centre           */
#define RING_QUIET_S      3.50f    /* quiet room: one small drop this often                 */
#define RING_QUIET_DROP_S 0.30f    /* ...this high                                          */
/* Frame budget: every ripple redraws glyph cells, and redrawn cells are what the device pays
 * for; the drop rate and height are the expensive knobs. Host bench, a blob and 40 s of
 * phrases at 4 Hz syllables, cells per frame: 592 with these values at ~2 drops/s, against 518
 * for the old breathing line with rim pings and 299 with no listen; a quiet room costs 321
 * against the old 369. Raise them in small steps and watch fps on the device. */

/* ── presence ──────────────────────────────────────────────────────────────── */
#define RING_FADE_S       0.3f     /* fade in when a listen starts / melt out when it ends */
