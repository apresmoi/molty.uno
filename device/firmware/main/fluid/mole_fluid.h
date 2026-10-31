/*
 * mole_fluid.h — "fluid" creature: Molty as a fluid of characters.
 *
 * Device port of device/creatures/fluid.html. One per-cell fluid (fluid_sim.h)
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
 *         ack: "chirp"|"thinking"|"off" (what the device does when the human finishes
 *               speaking: chirp + thinking out of the box; kept in NVS; GET reports it),
 *         shake_talk: true|false (a shake starts a listen by itself, for the agents:
 *               on out of the box; kept in NVS; GET reports it),
 *         idle_life: "lively"|"calm"|"off" (how much the creature moves by itself:
 *               fluid_director.h; lively out of the box; kept in NVS; GET reports it),
 *         act: "nod"|... (a one-shot action, cute_act.h; GET reports the one playing),
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
 * The creature (.molty, device/creatures/MOLTY-FORMAT.md) the "creature" state renders.
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
 *  — each rise of `level` (0..1, the VAD's mic level, ~10 Hz) drops a touch-style ripple
 *  in the water around the creature (in its listening mood), a quiet room a small slow one,
 *  and a faint line of the fluid marks the rim; off melts it back. Knobs: fluid_ring.h. Never shown while a question
 *  is on screen. Its colour: mole_fluid_listen_color (the listen's ring_color, 0xRRGGBB;
 *  -1 = the device default, POST /api/creature/state {ring_color: "#rrggbb"|"accent"},
 *  kept in NVS, else the palette's accent; fluid_ring_rgb). Safe from any task; a no-op
 *  before init. */
void mole_fluid_listen(bool on);
void mole_fluid_listen_level(float level);
void mole_fluid_listen_color(int32_t rgb);
/** A say's text follows its voice (link_audio.c, cap say_cues): word < 0 starts following:
 *  the text shows no word until word 0 is reached (its own clock stops); word ≥ 0 shows up to that word (the
 *  last one the speaker reached; never backwards). mole_fluid_text_cue_end: the say is
 *  over — the reader's own pace takes whatever is left, then it ends as usual. */
void mole_fluid_text_cue(int word);
void mole_fluid_text_cue_end(void);
/** A one-shot action (cute_act.h CA_*) on the creature (link_cmd.c `act`, a say's action
 *  mark): it plays once the creature is on screen and no question covers it, waiting up to
 *  3 s (fluid_director.h). Safe from any task; a no-op before init. */
void mole_fluid_act(int act);
/** A say's mood mark reached its word (link_audio.c, cap say_marks): the reader's words
 *  — if text_ver is still the loaded text, i.e. the say's own — take that mood's colour from there on (happy amber, surprised cyan, thinking violet,
 *  sleepy blue, listening green; idle / speaking the palette's), until the next text. The
 *  creature itself shows the say's last mood when the words end (link_audio_tick). */
void mole_fluid_cue_mood(int mood, unsigned text_ver);
/** The reader's text version (bumped by every text set / append): a say captures it after
 *  setting its words and hands it to mole_fluid_cue_mood, which tints only while that text
 *  is still the one loaded. */
unsigned mole_fluid_text_ver(void);
/** A say played to its end (link_audio_tick): the director may give a happy flash once the
 *  creature is back and idle (fd_turn_spoken). Safe from any task. */
/* The heap guard (diag/heap_guard.h): internal RAM is running out — the sim's hot fields move
 * to PSRAM (rebuilt on the task's next frame, same grid; and every later build too). Slower
 * frames, never a starved Wi-Fi. Any task. */
void mole_fluid_shed_internal(void);
/* how many of the sim's hot fields sit in internal RAM (the last frame's count) */
int mole_fluid_internal_fields(void);
void mole_fluid_spoke(void);
/** The listen window an agent gets when it gives none (link_audio.c, cmd listen without ms):
 *  0 = unlimited or 5000..30000 ms (link_protocol.h LINK_LISTEN_*), 30000 out of the box.
 *  The console's setting, POST /api/creature/state {listen_ms} (GET reports it), kept in NVS
 *  like ring_color. Safe from any task, before init too (read straight from NVS). */
uint32_t mole_fluid_listen_window(void);
/** The turn acknowledgement's setting (link_protocol.h link_ack_t: 0 off, 1 thinking,
 *  2 chirp — the default). POST /api/creature/state {ack: "chirp"|"thinking"|"off"} (GET
 *  reports it), kept in NVS like ring_color. Safe from any task, before init too. */
int mole_fluid_ack_mode(void);
/** Idle life (fluid_director.h FD_IDLE_*: 0 off, 1 calm, 2 lively — the default). POST
 *  /api/creature/state {idle_life} (GET reports it), NVS. The listener reactions follow it
 *  (link/listen_react.h: off = none). Safe from any task, before init too. */
int mole_fluid_idle_life(void);
/** The auto filler's delay (link_protocol.h LINK_FILLER_*: 0 = off, else 400..5000 ms, 2500 out
 *  of the box): reported in every listen's answer, the relay acts on it. POST
 *  /api/creature/state {filler_ms} (GET reports it), kept in NVS like ring_color. Safe from any
 *  task, before init too. */
uint32_t mole_fluid_filler_ms(void);
/** Shake to talk (link_protocol.h): whether a shake starts a listen by itself. POST
 *  /api/creature/state {shake_talk: bool} (GET reports it), kept in NVS like ring_color; on
 *  out of the box. Safe from any task, before init too. */
bool mole_fluid_shake_talk(void);

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
