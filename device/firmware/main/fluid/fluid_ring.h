/* fluid_ring.h — how a listen looks and moves: EVERY tuning knob in one place.
 *
 * A circle of the fluid (fluid_target_ring, a target like the creature's) sits fixed just inside
 * the rim, in ring_color. It stays there; the ripples bend it. Each syllable-like rise of the voice raises the water
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
#define RING_INK          0.20f    /* target ink (the creature is ~1, the dim background ~0.1) */
#define RING_INK_LEVEL    0.00f    /* + this at full voice: brighter, never wider          */

/* ── the voice ─────────────────────────────────────────────────────────────── */
#define RING_ATTACK_S     0.08f    /* ink's level smoothing toward louder                  */
#define RING_DECAY_S      0.45f    /* ...toward quieter                                    */
#define RING_ONSET_MIN    0.22f    /* below this level it is the room, not a voice          */
#define RING_ONSET_RISE   0.16f    /* a syllable: the level rose this much over its recent low */
#define RING_LOW_RISE_S   0.40f    /* the recent low creeps up to the level this slowly     */
#define RING_GAP_S        0.55f    /* never two waves closer than this    */
#define RING_HOLD_S       1.00f    /* a voice with no rises (a held vowel, a clipped level)
                                    * still sends one this often                                */

/* ── the waves: the whole circle rises at once, radially symmetric ───────────── */
#define RING_PING_IN      0.10f    /* the bump rises this far inside the line               */
#define RING_PING_WIDTH   0.04f    /* the bump's radial width, stage units: narrow = a spiky crest */
#define RING_PING_S_MIN   0.08f    /* the softest syllable's height                         */
#define RING_PING_S_MAX   0.28f    /* full voice                                            */
#define RING_QUIET_S      3.50f    /* quiet room: one faint wave this often                 */
#define RING_QUIET_PING_S 0.05f    /* ...this high                                          */
/* Frame budget: waves redraw glyph cells everywhere; height and rate are the expensive knobs.
 * On the device: 26-27 fps while spoken to (32-34 otherwise); full-voice 0.50 fell to 19. */

/* ── presence ──────────────────────────────────────────────────────────────── */
/* the voice plays the water: louder is taller and closer together, a murmur is small and far apart */
#define RING_GAP_SOFT     1.10f    /* time to the next wave after a barely-there syllable    */
#define RING_S_CURVE      1.2f     /* height follows level^this: quiet stays gentle          */
/* while listening the water only bends what it crosses, circle included: a touch in reverse */
#define RING_WAVE_C2      0.70f    /* the water's wave speed² × this: a little slower       */
#define RING_WAVE_REFR    1.6f     /* the water's refraction × this                         */
#define RING_FADE_S       0.3f     /* fade in when a listen starts / melt out when it ends */
