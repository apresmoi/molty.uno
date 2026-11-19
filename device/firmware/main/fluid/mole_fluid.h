/*
 * mole_fluid.h — "fluid" creature: Molty as a fluid of characters.
 *
 * Device port of device/creatures/fluid.html. One per-cell fluid (fluid_sim.h)
 * fills the round panel as braille dots; states are targets it flows toward:
 * creature (the .molty, home: boot lands here), blob (stands in while no creature is
 * loaded), thinking, text (speed reader), effect:ripple|vortex|wave, and the demo reel
 * ("autoplay", on request only). At power on the boot screen comes first: "molty.uno"
 * condenses out of the fluid, then the firmware default's creature paddles until the link is
 * up and dissolves into the chosen one (fluid_director.h "boot screen"). What is shown
 * when (the same-state guard, idle mood flashes, sleep, the reader's return) is
 * fluid_director.h. Touch → ripple, IMU tilt → slosh.
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
#include "cJSON.h"
#include "fluid_ask.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Creature vtable functions ──────────────────────────────────────────────── */
esp_err_t mole_fluid_creature_init(void);
void      mole_fluid_creature_deinit(void);
esp_err_t mole_fluid_creature_render(void);
/** The frame on the panel into the board's framebuffer (the drawer sends the panel only
 *  windows, from internal RAM): call before reading it. Waits for the next frame, up to
 *  500 ms; ESP_ERR_INVALID_STATE when the fluid is not running. */
esp_err_t mole_fluid_framebuffer_sync(void);
#ifdef MOLTY_DIAG_RING
/** Debug builds only (MOLTY_DIAG_RING=1 at build time): the listening ring as if a listen ran for ms, no audio; voice:
 *  with a made-up voice on it (cpk_voice). */
void mole_fluid_diag_ring(uint32_t ms, bool voice);
#endif
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
/**
 * The creature package `id` (docs/PACKAGES.md section 5) as the creature, hot: json (len bytes,
 * PSRAM) is taken over and built on the fluid task (checked first: cp_create). The choice
 * itself is kept by the caller (pkg_creature.h). Safe from any task.
 */
esp_err_t mole_fluid_creature_package(const char *id, char *json, size_t len);
/** What an agent is told about the creature on screen: cp_info's JSON for a package ({id, name,
 *  about, gestures, moods}), {id: "", name} for a .molty. malloc'd (the caller frees) with *gen set to
 *  its generation (it moves at each change), or NULL while it is still *gen (0: never seen) or before
 *  the first creature loaded (link_cmd_creature_frame). Safe from any task. */
char *mole_fluid_creature_info(unsigned *gen);
/** The creature package shown or on its way into id (n bytes), "" for a .molty creature. */
void mole_fluid_creature_id(char *id, size_t n);

/** A package's scene (scene/scene_player.h) on the fluid, the FD_ST_SCENE state: PLAY
 *  replaces what plays and the queue, QUEUE plays it after the queue (at once when nothing
 *  plays), CLEAR (sc NULL) drops everything; with nothing left the creature comes back.
 *  Another state taking the screen (a say's words, a mood's creature) drops the scene. Takes
 *  sc on ESP_OK (freed on the fluid task); ESP_ERR_INVALID_STATE when the queue is full;
 *  ESP_ERR_INVALID_STATE before init. Safe from any task. */
typedef enum { MOLE_SCENE_PLAY, MOLE_SCENE_QUEUE, MOLE_SCENE_CLEAR } mole_scene_op_t;
struct scene;
esp_err_t mole_fluid_scene(mole_scene_op_t op, struct scene *sc);

/** Something happened to the device (sense.c): a shake (a brief surprised flash) or a
 *  button press (wakes a sleepy creature). Either turns the screen back on. Safe from any
 *  task; a no-op before init. */
typedef enum { MOLE_FLUID_SHAKE, MOLE_FLUID_BUTTON } mole_fluid_event_t;
void mole_fluid_event(mole_fluid_event_t ev);
/** The screen off (false) or on (power.h's switch, sense.c's button). Off, the fluid stops
 *  simulating and drawing (the CPU's share of the battery) and the panel goes to sleep
 *  (molty_display_power; a board without it gets one black frame). Requests keep queueing
 *  while it is off. It comes back on by itself for something the human must see: a touch
 *  (that touch is not a tap), new words (a say, the pairing code), a question or a listen.
 *  Safe from any task; a no-op before init. */
void mole_fluid_screen(bool on);
/** The screen is on (true before init). */
bool mole_fluid_screen_on(void);
/** Power off's goodbye (fluid_director.h "fd_bye"): the screen on, "bye!" condenses, melts
 *  into a pond and the panel fades to black; then mole_fluid_goodbye_done. From here the
 *  screen stays on and every request is ignored. Safe from any task. */
void mole_fluid_goodbye(void);
bool mole_fluid_goodbye_done(void);

/** The link's health as an FD_STATUS_* code (fluid_director.h): a small mark at the
 *  screen's edge, never a state change. One caller (sense.c). */
void mole_fluid_set_status(int status);

/** The listening ring (link_audio.c, the audio task): on while the mic records a `listen`
 *  — each rise of `level` (0..1, the VAD's mic level, ~10 Hz) sends a round wave from a fixed circle at the rim across the
 *  screen (in its listening mood), a quiet room a faint slow one; off melts it back. Knobs: fluid_ring.h. Never shown while a question
 *  is on screen. Its colour: mole_fluid_listen_color (the listen's ring_color, 0xRRGGBB;
 *  -1 = the device default, POST /api/creature/state {ring_color: "#rrggbb"|"accent"},
 *  kept in NVS, else the palette's accent; fluid_ring_rgb). Safe from any task; a no-op
 *  before init. */
/** Progress (the agent's, link `progress`; a package's download, pkg_cmd.c): a thin line on the listening ring's
 *  circle, filling clockwise from 12 o'clock to pct (0..100; < 0 = no percent: a comet circles). Over a rigged creature
 *  it is the recording ring itself (creature_ring.h: formed from a wave at the centre, its colour), in mint over the
 *  fluid. Each call keeps it up (it melts after 120 s without one); done fills it, flashes and lets it go into the
 *  water; stop lets it go as it is (a failure). Under the listening ring. Safe from any task. */
void mole_fluid_progress(float pct, bool done);
void mole_fluid_progress_stop(void);
void mole_fluid_listen(bool on);
/** The listen's mic level (0..1, the VAD's, each mic chunk): a rigged creature's listen ring ripples with it. */
void mole_fluid_listen_level(float level);
void mole_fluid_listen_color(int32_t rgb);
/** A say's text follows its voice (link_audio.c, cap say_cues): word < 0 starts following:
 *  the text shows no word until word 0 is reached (its own clock stops); word ≥ 0 shows up to that word (the
 *  last one the speaker reached; never backwards). mole_fluid_text_cue_end: the say is
 *  over — the reader's own pace takes whatever is left, then it ends as usual. */
void mole_fluid_text_cue(int word);
void mole_fluid_text_cue_end(void);
/** A one-shot gesture on the creature (link_cmd.c `act`, a say's action mark): a cute_act.h CA_* id
 *  or one of the creature's own (mole_fluid_gesture). It plays once the creature is on screen and no
 *  question covers it, waiting up to 3 s (fluid_director.h). Safe from any task; a no-op before init. */
void mole_fluid_act(int act);
/** The gesture `name` of the creature now: a shared one (cute_act.h, every creature plays them) or one
 *  of its package's own (CUTE_N_ACTS + i), its length in *dur (NULL: not wanted); -1 for a name it
 *  lacks. Safe from any task. */
int  mole_fluid_gesture(const char *name, float *dur);
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
/* The network is up (the relay's first welcome): the hot fields may take the internal RAM
 * left above the sim's reserve (rebuilt on the task's next frame). Once a boot, never after
 * a shed. Any task. */
void mole_fluid_allow_internal(void);

/** A tap at panel pixel (px, py): on a spot of the scene on screen (scene_player.h), Molty
 *  shows its reading as the subtitle, or opens the layout the spot names (its `open`). Non-zero
 *  (which spot) if it landed on one; MOLE_SPOT_BACK if it landed beside every spot of a layout
 *  a tap had opened: Molty goes back to the scene under it. Any task. */
#define MOLE_SPOT_BACK 0xFFFFFFFFu
unsigned mole_fluid_spot_tap(int px, int py);
/** That spot's id into id (size bytes; "" if it has none or another spot was tapped since), and the
 *  layout it opens into open (may be NULL; "" if it opens none). Any task. */
void mole_fluid_spot_id(unsigned spot, char *id, size_t size, char *open, size_t open_size);
/** The scene on screen changed on the device itself since the last call (a tap opened a layout, it went back,
 *  the scenes ended): what is up now, for the relay (link_evt_scene_frame). why: "open" | "back" (a tap beside
 *  it) | "end" (it ran out); pkg and layout "" when nothing is up; spot: the one that opened it. False: no change.
 *  Any one task. */
typedef struct { char pkg[33], layout[25], spot[41], why[6]; } mole_scene_evt_t;
bool mole_fluid_scene_event(mole_scene_evt_t *out);
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
/* The gesture bindings (gesture.h, packed): what tap, double tap and the buttons do.
 * /api/creature/state {gestures: {tap: "talk", ...}} and cmd gestures; GET reports them. */
uint32_t mole_fluid_gestures(void);
cJSON *mole_fluid_gestures_json(void);
esp_err_t mole_fluid_gestures_apply(const cJSON *bindings);

/** A question (link_ask.c and link_pair.c, the link task) — the screen follows what the link wants (fluid_ask.h,
 *  fluid_askview.h; device/creatures/ask-options.html):
 *  show   q (title, detail, code, 1..FLUID_ASK_MAX answers in their colours, fluid_ask_color) forms over whatever shows,
 *         from a touch at the centre of the water; the scene dims under it and keeps moving; replaces any question.
 *  pick   it was answered with option i: a gentle wave from that answer, everything breaks into dust the water
 *         carries off (~1 s).
 *  end    it ended unanswered (timeout, cancelled): the rim lets go, then the same dust.
 *  While it stands, the director's state changes wait under it; its dust goes over whatever comes next.
 *  show sets *seq (!= 0) to the question's number on screen: what mole_fluid_ask_shown reports once the panel shows it
 *  formed enough to answer. */
esp_err_t mole_fluid_ask_show(const fluid_ask_q_t *q, unsigned *seq);
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
