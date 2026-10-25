/*
 * mole_fluid.h — "fluid" creature: Molty as a fluid of characters.
 *
 * Target board: Waveshare ESP32-S3-Touch-LCD-1.46 (412x412 round LCD)
 * FQBN: esp32:esp32:esp32s3:FlashSize=16M,PSRAM=enabled,CDCOnBoot=cdc
 *
 * Device port of creatures/fluid.html. One per-cell fluid (fluid_sim.h)
 * fills the round panel as braille dots; states are targets it flows toward:
 * blob, thinking, text (speed reader), effect:ripple|vortex|wave, and an autoplay
 * show reel cycling through them. Touch → ripple, IMU tilt → slosh.
 *
 * State via GET/POST /api/creature/state when "fluid" is active:
 *   POST {state: "blob"|"thinking"|"text"|"effect:ripple"|"effect:vortex"|"effect:wave"
 *               |"autoplay"|"creature"(only once a target source is installed),
 *         text, wpm, palette: "green"|"amber"|"pastel", edge_mode: "off"|"contours"|"all",
 *         grid: "52x32", speed, mic (0..1), tilt (IMU gain, 0 = off), ripple: {x,y,strength},
 *         mood: idle|listening|thinking|speaking|happy|sleepy|surprised (alone: shows the creature)}
 *   GET adds read-only fps, frame_ms, sim_ms, draw_ms, flush_ms, word, energy, ...
 * POST /api/reader/text and /api/reader/append feed the speed reader.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Creature vtable functions ──────────────────────────────────────────────── */
esp_err_t mole_fluid_creature_init(void);
void      mole_fluid_creature_deinit(void);
esp_err_t mole_fluid_creature_render(void);
esp_err_t mole_fluid_creature_state_get_json(char **out);
esp_err_t mole_fluid_creature_state_set_json(const char *body);

/** Replace the speed-reader text (also restarts the text state if it is showing). */
esp_err_t mole_fluid_set_text(const char *text, int len);
/** Append streamed words (POST /api/reader/append): the text state keeps its place and
 *  resumes if it was holding the last word. Excess beyond 4 KB is dropped. */
esp_err_t mole_fluid_append_text(const char *chunk, int len);

/**
 * The creature (.molty, creatures/MOLTY-FORMAT.md) the "creature" state renders.
 * put: validate (magic, version, length, CRC, bounds), store atomically as
 * /spiffs/creature.molty and hot-reload it; *err names the failed check.
 * get: the stored file, else the firmware default (duck); caller frees *out with
 * heap_caps_free. Both are safe from the httpd task.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
esp_err_t mole_fluid_creature_file_put(const uint8_t *buf, size_t len, const char **err);
esp_err_t mole_fluid_creature_file_get(uint8_t **out, size_t *len, bool *is_default);

#ifdef __cplusplus
}
#endif
