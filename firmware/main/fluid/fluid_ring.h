/* fluid_ring.h — the listening ring's look and motion: EVERY tuning knob in one place.
 *
 * The ring is the fluid itself (fluid_target_ring, a target like the creature's), not a
 * pixel overlay: a thin, soft line just inside the rim whose radius and ink ripple with
 * two counter-travelling waves around it — slow and faint when the room is quiet, faster
 * and deeper while the human speaks — and that "pings" the fluid's own wave field from
 * points on the ring, so sound-like ripples spread inward toward Molty with the voice.
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
#define RING_INK_RIPPLE   0.35f    /* ink rides the waves: crests ×(1+this), troughs ×(1−this) */

/* ── ripples along the ring: two waves travelling in opposite directions ──────── */
#define RING_WAVES_A      5        /* crests around the ring, wave A (its wavelength = 1/5 turn) */
#define RING_WAVES_B      3        /* crests around the ring, wave B (counter-travelling)  */
#define RING_AMP_IDLE     0.010f   /* radial ripple amplitude when quiet (≈ 2 px)          */
#define RING_AMP_LEVEL    0.030f   /* + this at full voice (≈ 6 px; 0.04 total ≈ 8 px)      */
#define RING_SPEED_IDLE   0.06f    /* turns/s the waves travel when quiet (gentle drift)   */
#define RING_SPEED_LEVEL  0.40f    /* + this at full voice (turns/s)                       */

/* ── the voice ─────────────────────────────────────────────────────────────── */
#define RING_LEVEL_GAIN   1.4f     /* mic level × this (clamped to 1): normal speech moves it fully */
#define RING_ATTACK_S     0.08f    /* level smoothing toward louder (10 Hz in → no steps)  */
#define RING_DECAY_S      0.45f    /* ...toward quieter: silence settles back to the calm line */

/* ── pings: sound-like ripples of the fluid's own surface, from points on the ring ── */
#define RING_PING_IDLE    0.25f    /* pings/s when quiet                                  */
#define RING_PING_LEVEL   1.2f     /* + pings/s at full voice                             */
#define RING_PING_S_IDLE  0.08f    /* a quiet ping's strength (fluid_sim_impulse)         */
#define RING_PING_S_LEVEL 0.22f    /* + strength at full voice                            */
#define RING_PING_RADIUS  0.05f    /* a ping's radius                                     */
/* Frame budget: every ripple of the surface redraws glyph cells across the screen, and
 * redrawn cells are what the device pays for (the old ring cost ~1.3 fps). With these
 * values the ring redraws fewer cells per frame than the old band did (−20 % over a
 * listen, −2 % while spoken to; firmware host bench). More / stronger pings or a deeper
 * RING_AMP_LEVEL cost frames — raise them in small steps. */

/* ── presence ──────────────────────────────────────────────────────────────── */
#define RING_FADE_S       0.3f     /* fade in when a listen starts / melt out when it ends */
