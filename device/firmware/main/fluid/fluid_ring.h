/* fluid_ring.h — the listening ring's look and motion: EVERY tuning knob in one place.
 *
 * The ring is the fluid itself (fluid_target_ring, a target like the creature's), not a
 * pixel overlay: a thin, soft line just inside the rim that BREATHES — the whole circle
 * expands and contracts, slow and shallow when the room is quiet, quicker and deeper (and
 * a little further out) while the human speaks — and "pings" the fluid's own wave field
 * from the whole rim at once, so circular ripples run inward toward Molty with the voice.
 * Nothing moves around the ring: every point of it does the same thing.
 *
 * Units: radii and lengths in stage units (1 = the rim; at 412 px one unit = 206 px, and
 * on the 52×32 grid a cell is ≈ 0.038 wide × 0.0625 tall). Level = the VAD mic level
 * 0..1 (vad_level: room ≈ 0, close speech ≈ 1), arriving at ~10 Hz and smoothed per frame.
 * Tune on the device: change a value, `make firmware`, flash, listen. */
#pragma once

/* ── shape ─────────────────────────────────────────────────────────────────── */
#define RING_R            0.875f   /* centre line, of the radius                          */
#define RING_CORE         0.10f    /* full-ink core, half-width in CELLS (along the ring's
                                    * normal: a cell is 8 px wide, 13 px tall at 412 / 52×32) */
#define RING_SOFT         0.90f    /* soft falloff beyond the core, in cells: core + soft =
                                    * 1 makes an anti-aliased line ONE cell thick at every
                                    * angle (the old ring was a 2–3 cell band); thinner than
                                    * that and it breaks up where it falls between two rows */

/* ── strength (target ink 0..1; the creature is ~1, the dim background ~0.1) ──── */
#define RING_INK          0.30f    /* the calm line                                       */
#define RING_INK_LEVEL    0.16f    /* + this at full voice (brighter while spoken to)     */

/* ── breathing: the whole ring expands and contracts (no motion around it) ─────── */
#define RING_BREATH_HZ_IDLE   0.25f  /* breaths/s when quiet (calm, slow)                     */
#define RING_BREATH_HZ_LEVEL  1.6f   /* + this at full voice (a quicker pulse while spoken to) */
#define RING_BREATH_AMP_IDLE  0.008f /* radius in/out when quiet (≈ 1.6 px)                   */
#define RING_BREATH_AMP_LEVEL 0.012f /* + this at full voice (≈ 2.5 px; 0.02 in all ≈ 4 px)     */
#define RING_SWELL            0.008f /* the ring sits this much further out at full voice (≈ 1.6 px) */
#define RING_INK_BREATH       0.25f  /* ink rides the breath: out ×(1+this), in ×(1−this)     */

/* ── the voice ─────────────────────────────────────────────────────────────── */
#define RING_LEVEL_GAIN   1.4f     /* mic level × this (clamped to 1): normal speech moves it fully */
#define RING_ATTACK_S     0.08f    /* level smoothing toward louder (10 Hz in → no steps)  */
#define RING_DECAY_S      0.45f    /* ...toward quieter: silence settles back to the calm line */

/* ── pings: a ripple of the fluid's own surface from the WHOLE rim at once — radially
 *    symmetric, so a circular wave runs inward across the window toward Molty ──────── */
#define RING_PING_IDLE    0.08f    /* pings/s when quiet (one every ~12 s)                */
#define RING_PING_LEVEL   0.2f     /* + pings/s at full voice (one every ~3.5 s)          */
#define RING_PING_S_IDLE  0.03f    /* a quiet ping's height (fluid_sim_impulse units)     */
#define RING_PING_S_LEVEL 0.08f    /* + height at full voice                              */
#define RING_PING_WIDTH   0.03f    /* the rim bump's radial width, stage units            */
/* Frame budget: every ripple of the surface redraws glyph cells across the screen, and
 * redrawn cells are what the device pays for. A rim ping moves cells ALL round the circle
 * (a point ping did not), so pings are the expensive knob: each extra 0.1 ping/s at full
 * voice ≈ +15–20 redrawn cells/frame while spoken to. With these values the ring redraws
 * no more cells than the rotating version did (host bench: 292 vs 295 per frame over a
 * listen, 374 vs 377 while spoken to). Raise pings or the breath in small steps. */

/* ── presence ──────────────────────────────────────────────────────────────── */
#define RING_FADE_S       0.3f     /* fade in when a listen starts / melt out when it ends */
