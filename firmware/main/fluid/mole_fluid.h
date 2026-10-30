/*
 * mole_fluid.h — "fluid" creature: Molty as a fluid of characters.
 *
 * Device port of creatures/fluid.html. One per-cell fluid (fluid_sim.h)
 * fills the round panel as braille dots; states are targets it flows toward:
 * creature (the .molty, home: boot lands here), blob (stands in while no creature is
 * loaded), thinking, text (speed reader), effect:ripple|vortex|wave, and the demo reel
 * ("autoplay", on request only). What is shown when — the same-state guard, idle mood
 * flashes, sleep, the reader's return — is fluid_director.h. Touch → ripple, IMU tilt
 * → slosh.
 *
 * State via GET/POST /api/creature/state when "fluid" is active:
 *   POST {state: "creature"|"blob"|"thinking"|"text"|"effect:ripple"|"effect:vortex"
 *               |"effect:wave"|"autoplay",
 *         hold: true (with "text": hold the last word instead of returning to the
 *               creature 1.5 s after it; the pairing code, the Improv "hi"),
 *         text, wpm, palette: "green"|"amber"|"pastel", edge_mode: "off"|"contours"|"all",
 *         grid: "52x32", speed, mic (0..1), tilt (IMU gain, 0 = off), ripple: {x,y,strength},
 *         mood: idle|listening|thinking|speaking|happy|sleepy|surprised (alone: shows the creature),
 *         hold_s: 0..3600 (with mood: seconds before idle; 0 = until the next mood;
 *                 absent: happy / surprised revert after a few s, the rest persist)}
 *   Re-requesting the shown state changes nothing; autoplay is never persisted.
 *   GET adds read-only fps, frame_ms, sim_ms, draw_ms, flush_ms, word, energy, asleep,
 *   indicator ("ok"|"wifi"|"relay"|"auth"), ...
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

/** Something happened to the device (sense.c): a shake (a brief surprised flash) or a
 *  button press (wakes a sleepy creature). Safe from any task; a no-op before init. */
typedef enum { MOLE_FLUID_SHAKE, MOLE_FLUID_BUTTON } mole_fluid_event_t;
void mole_fluid_event(mole_fluid_event_t ev);
/** The link's health as an FD_STATUS_* code (fluid_director.h): a small mark at the
 *  screen's edge, never a state change. One caller (sense.c). */
void mole_fluid_set_status(int status);

#ifdef __cplusplus
}
#endif
