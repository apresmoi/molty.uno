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
 *         ring_color: "#rrggbb"|"accent" (the listening ring's default colour, kept in NVS;
 *               GET reports it the same way),
 *         listen_ms: 0 (unlimited) | 5000..30000 (the listen window an agent gets when it
 *               gives none, kept in NVS; GET reports it),
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

/** The listening ring (link_audio.c, the audio task): on while the mic records a `listen`
 *  — a thin line of the fluid itself condenses just inside the rim around the creature
 *  (in its listening mood) and breathes: the whole circle expands and contracts, calm
 *  when quiet, quicker and deeper with `level` (0..1, the VAD's mic level, ~10 Hz,
 *  smoothed), and it sends circular ripples across the fluid from the whole rim; silence
 *  settles it back to a calm breath, off melts it back. Knobs: fluid_ring.h. Never shown while a question
 *  is on screen. Its colour: mole_fluid_listen_color (the listen's ring_color, 0xRRGGBB;
 *  -1 = the device default, POST /api/creature/state {ring_color: "#rrggbb"|"accent"},
 *  kept in NVS, else the palette's accent; fluid_ring_rgb). Safe from any task; a no-op
 *  before init. */
void mole_fluid_listen(bool on);
void mole_fluid_listen_level(float level);
void mole_fluid_listen_color(int32_t rgb);
/** The listen window an agent gets when it gives none (link_audio.c, cmd listen without ms):
 *  0 = unlimited or 5000..30000 ms (link_protocol.h LINK_LISTEN_*), 30000 out of the box.
 *  The console's setting, POST /api/creature/state {listen_ms} (GET reports it), kept in NVS
 *  like ring_color. Safe from any task, before init too (read straight from NVS). */
uint32_t mole_fluid_listen_window(void);

/** A question (link_ask.c, the link task) — the screen follows what the link wants:
 *  show   n (1..FLUID_ASK_MAX) labels condense out of the fluid in their sectors
 *         (fluid_ask.h), each in its colour (colors[i] 0xRRGGBB, -1 or colors NULL =
 *         FLUID_ASK_COLORS[i]) with a rim arc of the fluid in that colour, and faint
 *         dividers; replaces any question on screen.
 *  pick   the question was answered with option i: the others (labels and arcs) melt,
 *         the chosen one stays ~0.8 s (its arc flares), then the creature comes back.
 *  end    the question ended unanswered (timeout, cancelled): the labels melt and the
 *         fluid returns to the creature — or to the state a command asked for meanwhile.
 *  While a question is up, other state changes wait under it (the director keeps them),
 *  except that an explicit state / mood request ends a pick's hold early.
 *  show sets *seq (≠ 0) to the question's number on screen: what mole_fluid_ask_shown
 *  reports once the panel shows it readable. */
esp_err_t mole_fluid_ask_show(int n, const char *const *labels, const int32_t *colors, unsigned *seq);
void mole_fluid_ask_pick(int index);
void mole_fluid_ask_end(void);
/** The question (its show seq) a tap may answer right now: the one on the frame last flushed
 *  to the panel, its labels condensed and not yet answered (fluid_ask_tappable); 0 when the
 *  panel shows none — nothing, or another question's letters still morphing into it.
 *  Safe from any task (the touch path). */
unsigned mole_fluid_ask_shown(void);

#ifdef __cplusplus
}
#endif
