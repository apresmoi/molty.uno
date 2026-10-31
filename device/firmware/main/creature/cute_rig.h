/*
 * cute_rig.h — the cute-kit RUNTIME on the device: a .molty spec (cute_spec.h) becomes
 * rigid low-poly parts on a small rig, animated by the mood engine, rasterised into the
 * fluid's per-cell target (ink, colour, solid features) the fluid relaxes toward.
 *
 * Port of device/creatures/js/cute/kit.js (CuteCreature: build, update, _face,
 * _limbs, MOTIONS), js/mood.js (MoodEngine) and the CreatureTarget raster of
 * js/fluid/targets.js (ortho camera fit, flat Lambert key 1.0 + ambient 0.4, one colour
 * per face, coverage / colour / luminance per cell, auto-exposure, dark covered cells as
 * solid features). Per-creature JS hooks are replaced by the declarative behaviours of
 * the .molty file. No ESP-IDF dependencies (host-testable).
 */
#pragma once

#include "cute_spec.h"
#include "fluid_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cute_rt cute_rt_t;

/** Build the creature: meshes from primitives, rig, face; normalise to height 1; fit the
 *  camera. seed drives the engine's jitter/blink randomness. NULL on OOM. */
cute_rt_t *cute_rt_create(const cute_spec_t *spec, uint32_t seed);
void       cute_rt_destroy(cute_rt_t *rt);
void       cute_rt_set_mood(cute_rt_t *rt, int mood);
int        cute_rt_mood(const cute_rt_t *rt);
/** Play a one-shot action (cute_act.h CA_*) over the current mood; a newer one replaces it
 *  (the old one fades out over 0.25 s). */
void       cute_rt_act(cute_rt_t *rt, int act);
/** The action playing, or -1. */
int        cute_rt_acting(const cute_rt_t *rt);
/* s into the action playing, 0 = none */
float      cute_rt_act_time(const cute_rt_t *rt);
/** The fake-TTS speech envelope (0..1) while speaking (mood.js SpeechEnvelope). */
float      cute_rt_env(const cute_rt_t *rt);
/** Advance the mood engine + kit animation + behaviours by dt seconds. */
void       cute_rt_update(cute_rt_t *rt, float dt);
/** Rasterise the posed creature into per-cell buffers for a W×H grid (2×4 samples/cell). */
void       cute_rt_raster(cute_rt_t *rt, int W, int H);
/** Write the fluid target from the last raster: T, tcol, tf (solid features). swell
 *  breathes the target about the centre (targets.js CreatureTarget.fill). */
void       cute_rt_fill(cute_rt_t *rt, fluid_sim_t *s, float swell);
/** Visible triangles of the last raster (the JS tri counter). */
int        cute_rt_tris(const cute_rt_t *rt);
const char *cute_rt_name(const cute_rt_t *rt);

#ifdef __cplusplus
}
#endif
