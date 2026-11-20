/*
 * mole_fluid.c — "fluid" creature: Molty as a fluid of characters.
 *
 * Port of device/creatures/js/fluid/main.js (wiring) on top of fluid_sim/target/draw.c; what
 * is shown when (states, moods, idle life, the demo reel) is fluid_director.c. Over it:
 * a question (the link's ask, fluid_ask.h: drawn over whatever shows, which dims and keeps moving under it,
 * fluid_askview.h; the director's transitions wait while it stands) and rings of the fluid itself (the listening
 * ring while a listen records, fluid_target_ring / fluid_ring.h). Threading:
 *   - fluid_task (core 1, prio 2) OWNS the sim, targets, renderer, the director and the
 *     display while active: touch poll, IMU read, director, fixed 60 Hz steps, draw, flush.
 *   - the board (molty_board.h) sizes everything: the stage is the display's largest
 *     centred square, the default grid keeps the 1.46's cell size in px; no touch / IMU
 *     simply means no ripples from a finger / no tilt.
 *   - the httpd task only touches s_cfg / pending requests / s_stats under s_mutex
 *     (held for microseconds), so a slow frame never stalls the single-threaded
 *     HTTP server and a POST never waits on a frame.
 *
 * FRAME BUDGET on the 1.46 (52×32 = 1664 cells, ~1280 inside the disc; estimates for the S3 @
 * 160 MHz with PSRAM-resident fields, before on-device measurement):
 *   target fill + background floor + ψ noise   ~2.5 ms   (3 noise3/cell)
 *   2 × sim step (60 Hz fixed step @ 30 fps)   ~5 ms
 *   view + braille draw (dirty cells only)     ~4 ms
 *   flush (dirty row band, ≤ full 412² QSPI)   ≤ 9 ms
 *   total                                      ~20 ms → paced at 3 ticks = 33.3 fps
 * Measured live: GET /api/creature/state → sim_ms / draw_ms / flush_ms / frame_ms / fps.
 * If frame_ms > 30, drop to grid "44x27" (≈30 % fewer cells).
 */
#include "mole_fluid.h"
#include "fluid_sim.h"
#include "fluid_director.h"
#include "fluid_ask.h"
#include "fluid_askview.h"
#include "scene_player.h"
#include "fluid_fine.h"
#include "cute_rig.h"
#include "cute_act.h"
#include "creature_pkg.h"
#include "pkg_creature.h"
#include "glyph_rsvp.h"
#include "molty_board.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <math.h>
#include <unistd.h>
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "mole_diag.h"
#include "esp_private/esp_clk.h"
#include "cJSON.h"
#include "gesture.h"
#include "nvs.h"
#include "link_protocol.h"   /* link_color_parse */

#include "glyph_font.h"   /* gc_font5x7, GC_RAMP_ASCII */
#include "fluid_ring.h"   /* the listening ring's knobs */

static const char *TAG = "mole_fluid";

/* ── Constants ──────────────────────────────────────────────────────────────── */

#define FLUID_TEXT_MAX     4096
#define FLUID_MAX_TOKENS   512
#define FLUID_STEP         (1.f / 60.f)
#define FLUID_FRAME_US     30000      /* 33.3 fps cap                            */
#define FLUID_SLEEP_US     50000      /* asleep (slow breathing): 20 fps, less CPU */
#define FLUID_TOUCH_US     16000      /* touch poll ≤ 60 Hz (shared I2C bus)    */
#define FLUID_TILT_ACCEL   25.f       /* cells/s² of flow bias per g of tilt    */
#define FLUID_TILT_LEAN    0.12f      /* stage units a target leans per g       */
#define FLUID_IMU_SX       (1.f)      /* device-frame → screen: dashboard maps  */
#define FLUID_IMU_SY       (-1.f)     /* dot_dx = +ax, dot_dy = -ay             */
#define FLUID_RIPPLE_Q     4

static const char *DEFAULT_TEXT =
    "hello, i am molty. i breathe in ascii and i speak in ascii. touch the water.";
/* the boot screen's word (fluid_director.h "boot screen"): condenses out of the fluid at power on */
#define BOOT_WORD "molty.uno"
/* ...and the goodbye's (fd_bye): before the power goes off */
#define BYE_WORD "bye!"

/* ── States (the director decides which, fluid_director.h) ─────────────────── */

enum { ST_BLOB = FD_ST_BLOB, ST_THINKING = FD_ST_THINKING, ST_TEXT = FD_ST_TEXT,
       ST_FX = FD_ST_FX, ST_CREATURE = FD_ST_CREATURE, ST_SCENE = FD_ST_SCENE };

/* relax rates per kind of target (text must condense within one word's time) */
static const float RATE_UP[FD_ST_COUNT]   = { 1.6f, 1.6f, 7.f, 1.8f, 2.6f, 2.6f };
static const float RATE_DOWN[FD_ST_COUNT] = { 2.0f, 2.0f, 2.4f, 2.2f, 3.4f, 3.4f };

/* ── Shared config / requests (httpd ↔ task, under s_mutex) ─────────────────── */

typedef struct {
    int   palette, edge_mode, grid_w, grid_h, wpm;
    float speed, mic, tilt;
} fluid_cfg_t;

typedef struct {
    float fps, frame_ms, pre_ms, sim_ms, wait_ms, draw_ms, flush_ms, energy, err;
    float panel_last;                          /* the drawer's last frame, ms (not averaged)                  */
    float prof_ms[FP_N];                       /* per-phase EMA, ms per frame */
    int   mood, ctris, status, act;
    char  scene_pkg[PKG_ID_MAX + 1], scene_layout[PKG_KEY_MAX + 1];   /* "" = none */
    float scene_left;                          /* s until it ends                  */
    bool  working;
    bool  internal, autoplay, asleep;
    int   n_internal, n_blk;
    int   state, fx, reel, word_idx, nwords, inside, grid_w, grid_h, cells_drawn;
    int   crisp_dots;                          /* a rigged creature: its dots handed to the drawer */
    int   share;                               /* ... and the share of its rows' work on core 1, percent */
    float send_n, send_kpx, blit_ms;           /* the panel's windows a frame, their kpx, ms in the blits (EMA) */
    float blit_part[3];                        /* ... of which: the board's mirror copy, waiting for the panel, commands (EMA) */
    char  word[40];
} fluid_stats_t;

static SemaphoreHandle_t s_mutex = NULL;
static SemaphoreHandle_t s_done  = NULL;        /* given by the sim task on exit    */
static SemaphoreHandle_t s_wdone = NULL;        /* given by the drawer on exit      */
static SemaphoreHandle_t s_go    = NULL;        /* snapshot ready → drawer          */
static SemaphoreHandle_t s_idle  = NULL;        /* drawer free → snapshot writable  */
static TaskHandle_t      s_task  = NULL;
static TaskHandle_t      s_dtask = NULL;
static volatile bool     s_run   = false;

/* grid 0×0 = not resolved yet: alloc_bufs() sets the panel's default (52×32 on the 1.46) */
static fluid_cfg_t   s_cfg = { FLUID_PAL_PASTEL, FLUID_EDGE_ALL, 0, 0, 300, 1.f, 0.f, 1.f };
/* display geometry, fixed once known (alloc_bufs): fb_w×fb_h framebuffer, square stage */
static int s_fb_w, s_fb_h, s_stage, s_ox, s_oy, s_grid_w0, s_grid_h0, s_touch_jump;
static fluid_stats_t s_stats;
static char   *s_text     = NULL;               /* PSRAM, FLUID_TEXT_MAX+1          */
static int     s_text_len = 0;
static int     s_text_ver = 0;
static bool    s_text_appended = false;         /* last change(s) only appended     */
static int     s_req_state = -1, s_req_fx = 0;  /* pending explicit state           */
static bool    s_req_implied = false;          /* ...or the creature a mood alone implies */
static bool    s_req_text_hold = false;         /* …"text" holding its last word    */
static bool    s_req_reel  = false;             /* pending autoplay restart         */
static bool    s_req_activity = false;          /* a state / mood / text change came */
static bool    s_req_shake = false, s_req_user = false;   /* sense.c: shake, button  */
static int     s_status_in = FD_STATUS_OK;      /* link status (mole_fluid_set_status) */
/* the question the link wants on screen (mole_fluid_ask_*): seq = which one; pick = the
 * answer once it came (the task holds it, then leaves by itself) */
typedef struct {
    unsigned seq; bool active; int pick;
    fluid_ask_q_t q;                            /* what is asked (fluid_ask.h) */
} ask_want_t;
static ask_want_t s_ask_want;
static bool    s_listen_in = false;             /* link_audio: a listen is recording   */
static float   s_voice_in = 0.f;                /* ... and the mic's level (0..1)       */
static bool    s_follow_in = false;             /* link_audio: the reader follows a say's voice */
static int     s_follow_word_in = 0;            /* ...the last word the speaker reached  */
static int32_t s_ring_in = -1;                  /* ...its ring colour (-1 = the default) */
/* the device's default ring colour (NVS "fluid"/"ring", the console's setting; -1 = the
 * palette's accent), loaded once by alloc_bufs */
static int32_t s_ring_dflt = -1;
static bool    s_ring_dflt_loaded = false;
/* the listen window an agent gets when it gives none (NVS "fluid"/"listen", the console's
 * setting; link_protocol.h LINK_LISTEN_*), loaded on first use */
static uint32_t s_listen_dflt = LINK_LISTEN_DEFAULT_MS;
static bool     s_listen_dflt_loaded = false;
static uint32_t listen_setting_load(void);       /* NVS → the setting (or the default) */
/* the turn acknowledgement (NVS "fluid"/"ack", the console's setting; link_ack_t), loaded on
 * first use */
static int      s_ack_dflt = LINK_ACK_DEFAULT;
static bool     s_ack_dflt_loaded = false;
static int      ack_setting_load(void);
/* the auto filler's delay (NVS "fluid"/"filler", the console's setting; LINK_FILLER_*), loaded
 * on first use */
static uint32_t s_filler_ms = LINK_FILLER_DEFAULT_MS;
static bool     s_filler_loaded = false;
static uint32_t filler_setting_load(void);
/* shake to talk (NVS "fluid"/"shake_talk", the console's setting; 1 = on, the default) */
static bool     s_shake_talk = true;
static bool     s_shake_talk_loaded = false;
static bool     shake_talk_load(void);
/* the gesture bindings (NVS "fluid"/"gestures", gesture.h; loaded once) */
static uint32_t s_gestures;
static bool     s_gestures_loaded = false;
static uint32_t gestures_load(void);
/* idle life (NVS "fluid"/"idle_life", the console's setting; FD_IDLE_*, lively by default) */
static int      s_idle_life = FD_IDLE_DEFAULT;
static bool     s_idle_life_loaded = false;
static int      idle_life_load(void);
static int      s_req_act = -1;                 /* an agent's action (act / a say's mark)  */
static bool     s_req_spoke = false;            /* a say played to its end                 */
/* scenes (mole_fluid_scene): ops wait here for the task, which owns the player */
typedef struct { mole_scene_op_t op; scene_t *sc; } scene_op_t;
static scene_op_t s_scene_ops[SCENE_QUEUE_MAX + 2];
static int        s_scene_nops, s_scene_nq;      /* pending ops; the player's queue (task's copy) */
static bool     s_req_regrid = false;           /* rebuild the grid: hot fields moved (shed / allow internal) */
static bool     s_shed = false;                 /* the heap guard shed them: internal RAM stays off this boot */
/* a say's mood marks tint its words from the cued word on (mole_fluid_cue_mood):
 * 0 = the palette's colours, else FLUID_RGB | 0xRRGGBB */
static uint32_t s_tint_in = 0;
static float   s_ripq[FLUID_RIPPLE_Q][3];
static int     s_ripn = 0;

/* the creature: a creature package (s_cpk, docs/PACKAGES.md section 5) or a .molty (s_cute).
 * The file is handed over under s_mutex and built by the task. */
static uint8_t *s_cute_blob = NULL;  static size_t s_cute_len = 0;   /* pending load (PSRAM) */
static bool     s_cute_pkg = false;                 /* the pending file is a creature package */
static char     s_cr_id[PKG_ID_MAX + 1] = "";       /* the package shown or on its way; "" = .molty */
static bool     s_booted = false;                   /* the boot screen played: once per power on */
static int      s_req_mood = -1;
static bool     s_req_work = false;             /* mood `working` (fd_agent_work; s_req_hold_s) */
static float    s_req_hold_s = -1.f;                /* with s_req_mood: < 0 = its default */
static char     s_cute_name[40] = "", s_cute_err[40] = "";
static char    *s_cr_info;            /* the creature as the agent is told (cp_info), PSRAM; NULL before a load */
static unsigned s_cr_gen;             /* moves when s_cr_info does */
static struct { char name[25]; float dur; } s_own[ME_OWN_MAX];   /* the creature's own gestures (a package's), ids CUTE_N_ACTS + i */
static int      s_nown;
/* a gesture by name: a shared one (cute_act.h) or the creature's own; -1 for one it lacks (s_mutex held) */
static int gesture_of(const char *name) {
    const int a = cute_act_lookup(name);
    for (int i = 0; a < 0 && name && i < s_nown; i++) if (!strcmp(s_own[i].name, name)) return CUTE_N_ACTS + i;
    return a;
}
static const char *gesture_name(int a) { return a >= CUTE_N_ACTS && a < CUTE_N_ACTS + s_nown ? s_own[a - CUTE_N_ACTS].name : a >= 0 ? cute_act_name(a) : ""; }
static cute_rt_t *s_cute = NULL;                    /* task-owned                        */
static cpk_t    *s_cpk = NULL;                      /* task-owned                        */
static int       s_crisp_dots = -1;                  /* its dots last handed to the drawer (-1: not crisp) */
static bool      s_crisp = false;                    /* a rigged creature shows: its own dots, crisp (creature_dots.h) */
static int      s_cute_tris = 0;

/* the creature on screen, whichever kind (task only) */
static bool  cr_loaded(void) { return s_cpk || s_cute; }
static void  cr_set_mood(int m) { if (s_cpk) me_set_mood(cpk_engine(s_cpk), m); else if (s_cute) cute_rt_set_mood(s_cute, m); }
static int   cr_mood(void) { return s_cpk ? me_mood(cpk_engine(s_cpk)) : s_cute ? cute_rt_mood(s_cute) : -1; }
static void  cr_act(int a) { if (s_cpk) me_act(cpk_engine(s_cpk), a); else if (s_cute) cute_rt_act(s_cute, a); }
static float cr_act_dur(int a) { return s_cpk ? me_act_dur(cpk_engine(s_cpk), a) : cute_act_dur(a); }
static int   cr_acting(void) { return s_cpk ? me_acting(cpk_engine(s_cpk)) : s_cute ? cute_rt_acting(s_cute) : -1; }
static float cr_act_time(void) { return s_cpk ? me_act_time(cpk_engine(s_cpk)) : cute_rt_act_time(s_cute); }
static float cr_env(void) { return s_cpk ? me_env(cpk_engine(s_cpk)) : cute_rt_env(s_cute); }

/* ── Task-owned state ───────────────────────────────────────────────────────── */

typedef struct { int16_t off, len, orp, core; uint8_t para; } ftok_t;

static fluid_sim_t  *s_sim  = NULL;
static fluid_text_t *s_tx   = NULL;
static fluid_draw_t *s_draw = NULL;            /* drawer-owned while it runs        */
static fluid_frame_t *s_frame = NULL;           /* snapshot handed to the drawer     */
static fluid_draw_opts_t s_dopts;
static fluid_letter_t s_dletters[FLUID_MAX_LETTERS];
static float s_melt = 0.f, s_shimmer = 0.12f;
static uint32_t s_frame_no = 0;
static char   *s_ttext  = NULL;                 /* task copy of the text (PSRAM)    */
static ftok_t *s_toks   = NULL;                 /* PSRAM, FLUID_MAX_TOKENS           */
static int     s_ntok   = 0, s_seen_ver = -1;

/* the question on screen, over whatever the director shows (its transitions wait while the question stands) */
static struct {
    fluid_ask_screen_t scr;                     /* phase, pick, seq (the ask_want_t it follows), t, out_t (fluid_ask.h) */
    fluid_ask_hit_t hit;                        /* where its answers are, for a tap                                    */
    fluid_askview_t *v;                         /* how it looks (PSRAM, made for it)                                    */
    fluid_askview_t *making;                    /* ... while its dots are worked out, a few rows a frame (ASK_BUILD_ROWS) */
    fluid_askview_t *old;                       /* one the drawer may still read: freed with the drawer idle            */
} s_ask;
/* the listening ring (fluid_target_arc) */
static bool  s_listen = false;                  /* task copy of s_listen_in */
static float s_voice = 0.f;                     /* task copy of s_voice_in */
#define FLUID_CRISP_LISTEN_RGB 0xFF2C3E           /* a rigged creature's listen ring (ring-options.html)       */
#define FLUID_CRISP_PROG_RGB   0x35E07C           /* ... and its progress line: green, apart from the listen  */
static int32_t s_ring_rgb = -1, s_ring_def = -1;   /* task copies of s_ring_in / s_ring_dflt */
static fluid_ring_t s_rg;                       /* the listening ring (fluid_ring.h)     */
#ifdef MOLTY_DIAG_RING
static volatile int64_t s_diag_ring_until;      /* a debug build's ring without a listen (mole_fluid_diag_ring) */
static volatile bool s_diag_voice;               /* ... with a voice made up (syllables ~4 Hz in phrases, pauses) */
void mole_fluid_diag_ring(uint32_t ms, bool voice) { s_diag_voice = voice; s_diag_ring_until = esp_timer_get_time() + (int64_t)ms * 1000; }
static float diag_voice(float t) {
    const float syl = 0.5f + 0.5f * sinf(t * 6.2831853f * 4.f), phrase = sinf(t * 6.2831853f * 0.35f);
    return phrase > 0.f ? fminf(1.f, (0.35f + 0.65f * syl) * (0.6f + 0.4f * phrase)) : 0.f;
}
#endif
static fluid_progress_t s_pg;                   /* the agent's progress on the same line */
static _Atomic int32_t s_prog_in = -1;          /* mole_fluid_progress: pct × 10, < 0 = no percent */
static atomic_uint     s_prog_seq;              /* bumped by each progress                         */
static atomic_bool     s_prog_end;              /* its done                                        */
static atomic_bool     s_prog_stop;             /* ... or it stopped short (a failed download)     */
#define PROG_TTL_S     120.f                    /* no news this long: the ring melts (as `working`) */
static float s_floor_w;                         /* the floor's weight, eased (p.floor)             */
static scene_player_t s_sp;                     /* the scene on screen + its queue        */
static fluid_overlay_t s_dov;                   /* the drawer's overlay (none drawn now)  */
static unsigned s_dask;                         /* ...the question a tap on it may answer */
/* the question a tap may answer on the frame last flushed to the panel (0 none): written
 * by the drawer after each flush, read by the touch path (mole_fluid_ask_shown) */
static atomic_uint s_glass_ask;

static fluid_director_t s_dir;                 /* states, moods, idle life, status  */
static int   s_cur = ST_BLOB, s_cur_fx = 0;     /* the state the sim is drawing      */
static float s_fx_state = 0.f;
static float s_tnow = 0.f, s_acc = 0.f, s_breath_ph = 0.f;
static float s_melt_t = 1e9f, s_word_until = 0.f;
static int   s_word = 0;
static bool  s_follow = false;                  /* task copies of s_follow_in / s_follow_word_in */
static int   s_follow_word = 0;                 /* -1: following, no word reached yet */
static bool  s_text_hidden = false;             /* following a say: the text state is blank until its first cue */
static uint32_t s_tint = 0;                     /* task copy of s_tint_in: the reader's words' colour */
static volatile int s_dstatus = FD_STATUS_OK;   /* indicator riding with the snapshot */
static fluid_letter_t s_overlay[FLUID_MAX_LETTERS];
static int   s_noverlay = 0;
static float s_gx = 0.f, s_gy = 0.f;            /* smoothed screen-space gravity     */
static bool  s_touch_down = false;
static float s_tpx = 0.f, s_tpy = 0.f;
static int   s_last_tx = 0, s_last_ty = 0;
static volatile bool s_full_flush = true;
static atomic_bool s_screen_want = true;        /* mole_fluid_screen                    */
static atomic_bool s_screen = true;             /* on the glass now (the task switches it) */
static bool fluid_quiet(void);
static atomic_bool s_bye = false, s_bye_done = false;   /* mole_fluid_goodbye; it ended   */
static float    s_bye_lvl = 1.f;                    /* the panel's level the goodbye set */
static bool     s_word_dots = false;                /* the boot word's dots (s_tx->dots) are drawn */
static uint8_t *s_sdots, *s_sdots_d;                /* a scene's dots: the task's, the drawer's (PSRAM) */
/* a scene in high definition: its shapes and the dots it keeps (scene_shapes_t), the task's
 * and the drawer's (PSRAM); n -1: too many shapes, it shows on the dots */
static fluid_shape_t *s_shp, *s_dshp;
static uint8_t *s_kdots, *s_kdots_d;
/* its fine dust (fluid_fine.h): built on this task a few ms a frame, drawn by the drawer; the cells a tapped spot's
 * drawing covers (the task's, the drawer's: the water never breaks them); the drawing at full growth it gathers round */
static fluid_fine_t  *s_fine;
static uint8_t       *s_still, *s_still_d;
static fluid_shape_t *s_fshp;
#define FINE_BUILD_US 6000                          /* the dust's build, at most this a frame (the sim's core) ...  */
#define FINE_AHEAD_US 20000                         /* ... and while its scene is still coming (a change of picture).
                                                     * (It was 20000: a step of the build runs past its budget by
                                                     * up to ~9 ms, and the sim's frame then ran past 33.) */
static unsigned s_cap_word, s_dcap_word;            /* the caption's word: a new number each word, 0 none */
static float    s_cap_box[4];                       /* its backdrop for the whole reading: the union of its words' boxes */
static bool     s_cap_box_on;
static unsigned s_rd_ser;                           /* counts the readings (read_spot) */
static float    s_cap_t0;                           /* when it came                                       */
static int      s_nshp, s_dnshp;
static scene_hd_t s_hd = { 0, false, 1.f };        /* a scene in high definition (scene_hd_tick) */
static int      s_dnletters;                        /* the drawer's letters (s_dletters), how many */
static bool     s_sub = false;                      /* a say over a scene: its words as a subtitle */
static int      s_sub_word = -1;                    /* the word the voice is on (-1: none yet) */
static float    s_sub_until = 0.f;                  /* the last word stays this long after the say */
/* a tapped spot's reading: its words as the subtitle, at reading pace (PSRAM) */
#define SPOT_READ_WPM  220
#define SPOT_READ_HOLD 1.f                          /* s the last word stays */
typedef struct { char text[PKG_SAY_MAX + 1]; ftok_t toks[PKG_SAY_MAX / 2 + 1]; int n, word; float next; } reading_t;
static reading_t *s_rd;
static bool       s_rd_on = false;
static scene_grow_t s_grow = { -1, 0.f, 0.f };     /* the tapped spot's drawing, grown (scene_grow_t) */
static unsigned     s_grow_gen;                     /* the scene it was tapped on */
/* the scene's spots for sense.c's taps (mole_fluid_spot_*): a copy of the scene on screen,
 * made when it starts if it has any, under s_mutex (PSRAM) */
static scene_t   *s_spot_sc;
static bool       s_spot_on = false;
static unsigned   s_spot_gen = 0;                   /* the player's gen it was copied at (0: none) */
static float      s_spot_t = 0.f;                   /* the scene's clock, for spots' on / off */
static atomic_uint s_spot_req;                      /* a tap on a spot: gen << 8 | item + 1, 0 none; MOLE_SPOT_BACK */
static bool       s_spot_back;                      /* the scene on screen was opened by a tap: a tap beside its spots goes back */
static _Atomic uint32_t s_tap_at;                   /* ... where the finger was, panel px: x << 16 | y (the change of picture it asks for shows there first) */
static bool       s_focus;                          /* ... and that tap asked for one (scene_open, scene_back): told to the drawer at the handoff */
static unsigned   s_tapped;                         /* the last spot tapped, its id and the layout it opens: kept apart from */
static char       s_tapped_id[PKG_STR_MAX + 1], s_tapped_open[PKG_KEY_MAX + 1];   /* its scene, which the tap may replace */
static mole_scene_evt_t s_scene_evt;                /* the screen changed on the device itself (mole_fluid_scene_event), s_mutex */
static bool       s_scene_evt_on;
#define SPOT_GEN(g) ((g) & 0xffffffu)
static bool  s_touch_mute = false;              /* the touch that woke the screen, until it lifts */
#define FLUID_OFF_POLL_MS  50                   /* screen off: touch and wake checks       */
/* the drawer's window buffer (fluid_draw_send): internal DMA-capable RAM, which the panel's
 * DMA reads as is. The frame never goes through the PSRAM framebuffer: drawing and
 * copying it there every frame took PSRAM bandwidth the sim on core 1 waits for. */
#define FLUID_WIN_ROWS 14                       /* a 412 px panel's cell row in one window */
static uint16_t *s_win = NULL, *s_win2 = NULL;   /* s_win2: a second window, the panel taking one while the other is
                                                 * drawn (molty_display_blit_async); FLUID_WIN2_PX each, internal DMA */
#define FLUID_WIN2_PX 4096                      /* 8 KB each: a full 466 px row band in two halves of 8 rows */
static int       s_win_px = 0;
static volatile bool s_fb_want = false;         /* /api/framebuffer: draw the frame there too */
static SemaphoreHandle_t s_fb_done = NULL;
static int   s_cpu_mhz = 160;

/* ── Text: tokenise + schedule (same rules as mole_reader.c / reader.js) ───── */

static inline bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

/* txt's words into toks (at most max): the count */
static int tokenize_into(const char *txt, ftok_t *toks, int max) {
    int n = 0, len = (int)strlen(txt), i = 0;
    while (i < len && n < max) {
        int nl = 0;
        while (i < len && is_ws(txt[i])) { if (txt[i] == '\n') nl++; i++; }
        if (nl >= 2 && n > 0) toks[n - 1].para = 1;
        if (i >= len) break;
        int st = i;
        while (i < len && !is_ws(txt[i])) i++;
        int tl = i - st;
        if (tl > FLUID_WORD_LETTERS) tl = FLUID_WORD_LETTERS;   /* display cap */
        gc_rsvp_orp_t orp = gc_rsvp_compute_orp(txt + st, tl);
        ftok_t *t = &toks[n++];
        t->off = (int16_t)st; t->len = (int16_t)tl; t->orp = (int16_t)orp.focal; t->core = (int16_t)orp.core_len; t->para = 0;
    }
    return n;
}

static void tokenize(void) {
    s_ntok = tokenize_into(s_ttext, s_toks, FLUID_MAX_TOKENS);
    /* A lone token is a static label (e.g. the pairing code), not a sequence:
     * centre it and drop the ORP accent letter (orp = -1, see fluid_text_set_word). */
    if (s_ntok == 1) s_toks[0].orp = -1;
}

static float word_delay_s(const char *txt, const ftok_t *t, int wpm) {
    float base = 60000.f / (float)(wpm < 1 ? 1 : wpm);
    float lf = t->core > 6 ? fminf(2.f, 1.f + 0.05f * (t->core - 6)) : 1.f;
    const char *w = txt + t->off;
    int b = t->len - 1;
    while (b > 0 && (w[b] == '"' || w[b] == '\'' || w[b] == ')' || w[b] == ']')) b--;
    char last = b >= 0 ? w[b] : 0;
    float pf = t->para ? 2.5f : (last == '.' || last == '!' || last == '?') ? 2.f
             : (last == ',' || last == ';' || last == ':') ? 1.5f : 1.f;
    float d = base * lf * pf;
    return (d < 40.f ? 40.f : d) / 1000.f;
}

/* the reader's letters in the say's mood colour (0: the palette's own) */
static void tint_letters(void) {
    for (int i = 0; i < s_tx->nletters; i++) s_tx->letters[i].rgb = s_tint;
}

/* the subtitle: the word the voice (or the reading) is on, as the reader shows it */
static scene_caption_t caption(const char *txt, const ftok_t *toks, int ntok, int word) {
    if (word < 0 || word >= ntok) return (scene_caption_t){ txt, 0, -1 };
    const ftok_t *t = &toks[word];
    return (scene_caption_t){ txt + t->off, t->len < FLUID_WORD_LETTERS ? t->len : FLUID_WORD_LETTERS, t->orp };
}

/* a say over the scene, else a tapped spot's reading */
static scene_caption_t scene_caption(void) {
    if (s_sub) return caption(s_ttext, s_toks, s_ntok, s_sub_word);
    if (s_rd_on) return caption(s_rd->text, s_rd->toks, s_rd->n, s_rd->word);
    return (scene_caption_t){ NULL, 0, -1 };
}

/* the scene on screen changed on the device itself: what is up now, for the relay (mole_fluid_scene_event) */
static void scene_event(const char *why, const char *spot) {
    mole_scene_evt_t e = { 0 };
    snprintf(e.pkg, sizeof(e.pkg), "%s", s_sp.cur ? s_sp.cur->pkg_id : "");
    snprintf(e.layout, sizeof(e.layout), "%s", s_sp.cur ? s_sp.cur->layout : "");
    snprintf(e.spot, sizeof(e.spot), "%s", spot ? spot : "");
    snprintf(e.why, sizeof(e.why), "%s", why);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_scene_evt = e; s_scene_evt_on = true;
    xSemaphoreGive(s_mutex);
    mole_diag_event(false, "scene.%s %s%s%s", why, e.layout[0] ? e.layout : "-", spot ? " by " : "", e.spot);
}

/* a listen's voice touches the water just inside it (a rigged creature's ring: 0.35 at 18 a second): gentler here, so
 * the dust by the rim stirs and the drawing does not break */
#define HD_VOICE_S    0.12f
#define HD_VOICE_R    0.05f
#define HD_VOICE_RATE 12.f
#define SCENE_TURN_S  20.f                          /* a scene's end waits this long after a listen for its answer  */
#define SCENE_END_ROOM_S 0.1f                       /* held so, a scene stops this short of its end                    */
#define SCENE_THINK_S 120.f                         /* ... and this long, at most, while the agent thinks or works    */
/* a tap landed on spot item n of sc: its reading, from its first word */
static void read_text(const char *text, int len);
static void read_spot(const scene_t *sc, int n) { read_text(sc->say + sc->items[n].str, sc->items[n].nstr); }
/* a reading (len characters of text, at most PKG_SAY_MAX), from its first word */
static void read_text(const char *text, int len) {
    len = len > PKG_SAY_MAX ? PKG_SAY_MAX : len;
    memcpy(s_rd->text, text, (size_t)len);
    s_rd->text[len] = '\0';
    s_rd->n = tokenize_into(s_rd->text, s_rd->toks, PKG_SAY_MAX / 2 + 1);
    s_rd->word = 0;
    s_rd_ser++;
    s_rd->next = s_tnow + word_delay_s(s_rd->text, &s_rd->toks[0], SPOT_READ_WPM);
    s_rd_on = s_rd->n > 0;
}

static void show_word(int idx) {
    const ftok_t *t = &s_toks[idx];
    fluid_text_set_word(s_tx, s_sim, s_ttext + t->off, t->len, t->orp);
    if (s_tint) tint_letters();
}

static cJSON *gestures_json(uint32_t packed) {
    cJSON *o = cJSON_CreateObject();
    for (int i = 0; o && i < GESTURE_IN_COUNT; i++)
        cJSON_AddStringToObject(o, GESTURE_INPUT_NAMES[i], GESTURE_ACTION_NAMES[gesture_binds_get(packed, (gesture_input_t)i)]);
    return o;
}

/* ── State machine (task) ───────────────────────────────────────────────────── */

static void ripple(float x, float y, float strength, float radius) {
    fluid_sim_impulse(s_sim, x, y, strength, radius);
}

/* Enter a state on the sim: the ripple + swirl that melt the old shape, the new relax
 * rates. Only for transitions the director hands out (fd_take) and a grid rebuild:
 * re-entering the shown state is exactly what melts the creature. */
static void enter_state(int st, int fx, const fluid_cfg_t *cfg) {
    int from = s_cur;
    if (s_cur == ST_FX) fluid_effect_exit(s_sim, s_cur_fx);
    if (st == ST_CREATURE && !cr_loaded()) st = ST_BLOB;   /* no creature loaded */
    s_sim->edge_w = st == ST_CREATURE ? 1.f : 0.f;      /* solid eyes once settled */
    s_word_dots = st == ST_TEXT ? fd_boot_word(&s_dir) : s_word_dots && s_cur == ST_TEXT;   /* kept through its melt */
    if (st == ST_TEXT && fd_boot_word(&s_dir)) {   /* the boot screen's word, in the fluid's own dots */
        fluid_text_set_glyphs(s_tx, s_sim, BOOT_WORD);
        s_word = 0; s_word_until = 1e9f; s_text_hidden = false;
    } else if (st == ST_TEXT && fd_bye_word(&s_dir)) {   /* the goodbye: one static label, held */
        const int n = (int)strlen(BYE_WORD);
        fluid_text_set_sentence(s_tx, n / 2.f);
        fluid_text_set_word(s_tx, s_sim, BYE_WORD, n, -1);
        s_word = 0; s_word_until = 1e9f; s_text_hidden = false;
    } else if (st == ST_TEXT) {
        if (s_ntok == 0) { strcpy(s_ttext, "..."); tokenize(); }
        float ext = 1.f;
        for (int i = 0; i < s_ntok; i++) {
            float a = s_toks[i].orp < 0 ? s_toks[i].len / 2.f : s_toks[i].orp + 0.5f, b = s_toks[i].len - a;
            if (a > ext) ext = a;
            if (b > ext) ext = b;
        }
        fluid_text_set_sentence(s_tx, ext);
        s_word = 0;
        /* following a say whose voice has not reached word 0 yet: blank until it does (the
         * word lands with its voice, not ~0.3 s before it) */
        s_text_hidden = s_follow && s_follow_word < 0;
        if (s_text_hidden) fluid_text_set_word(s_tx, s_sim, "", 0, 0);
        else show_word(0);
        s_word_until = s_tnow + 0.55f + word_delay_s(s_ttext, &s_toks[0], cfg->wpm);   /* lead-in */
    }
    /* the transition: the old shape breaks up in eddies + a ring from its centre */
    float cx, cy;
    fluid_sim_centroid(s_sim, &cx, &cy);
    if (st == ST_SCENE && s_fine) { /* on fine dust the change is the dust's own (fluid_morph_t): still water under it */ }
    else {
        if (s_sim->swirl_kick < 3.f) s_sim->swirl_kick = 3.f;
        if (st != ST_TEXT) ripple(cx, cy, 0.7f, 0.2f);
        else ripple(0.f, 0.f, 0.5f, 0.25f);
    }
    s_cur = st; s_cur_fx = fx; s_fx_state = 0.f;
    if (st != ST_TEXT) memset(s_sim->bgm, 255, (size_t)s_sim->N);
    s_sim->p.relax_up = RATE_UP[st]; s_sim->p.relax_down = RATE_DOWN[st];
    if (from == ST_TEXT && st != ST_TEXT) s_melt_t = 0.f;
    if (st == ST_FX) fluid_effect_enter(s_sim, fx);
}

/* the director's pending transition, if any, then its mood on the creature (moods blend
 * inside the creature: its mood engine fades between them and ignores the same mood) */
static void direct(const fluid_cfg_t *cfg) {
    int st, fx;
    /* a standing question owns the screen: the director's transitions wait under it */
    if (s_ask.scr.phase != FLUID_ASK_SHOW && fd_take(&s_dir, &st, &fx)) enter_state(st, fx, cfg);
    cr_set_mood(fd_mood(&s_dir));
}

/* ── A question (the link's ask): drawn over the screen (fluid_askview.h) ─────── */

#define ASK_BUILD_ROWS 24                       /* a question's dots: this many of their 4 x H rows a frame (~5 ms) */
/* follow what the link wants (once per frame, before the director's transitions) */
static void ask_follow(const ask_want_t *w) {
    const int did = fluid_ask_follow(&s_ask.scr, w->active, w->seq, w->pick, w->q.n);
    if (did & FLUID_ASK_DID_SHOW) {                        /* a new one: its look is made over the next frames (a frame
                                                            * cannot take it whole: ~0.3 s of roots and angles) */
        if (s_ask.v) { fluid_askview_destroy(s_ask.old); s_ask.old = s_ask.v; s_ask.v = NULL; }
        fluid_askview_destroy(s_ask.making);
        s_ask.making = fluid_askview_begin(s_sim, &w->q);
        if (!s_ask.making) mole_diag_event(true, "fluid.ask no memory for the question's look");
    }
    if (did & FLUID_ASK_DID_GONE) { fluid_askview_destroy(s_ask.making); s_ask.making = NULL; }
    if (s_ask.making) {                                    /* ... its clock waits; made, the touch at the centre */
        s_ask.scr.t = 0.f;
        if (fluid_askview_build(s_ask.making, s_sim, ASK_BUILD_ROWS)) {
            s_ask.v = s_ask.making; s_ask.making = NULL;
            s_ask.hit = fluid_askview_layout(s_ask.v)->hit; fluid_askview_start(s_ask.v, s_sim);
        }
    }
    if (s_ask.v && (did & (FLUID_ASK_DID_PICK | FLUID_ASK_DID_LEAVE))) fluid_askview_answer(s_ask.v, s_sim, did & FLUID_ASK_DID_PICK ? s_ask.scr.pick : -1);
    if ((did & FLUID_ASK_DID_GONE) && s_ask.v) { fluid_askview_destroy(s_ask.old); s_ask.old = s_ask.v; s_ask.v = NULL; }
}

/* with the drawer idle: the question over this frame's scene (its dots as crisp dots, dimmed under it), its letters */
static void ask_compose(void) {
    fluid_askview_t *v = s_ask.v;
    fluid_askview_destroy(s_ask.old); s_ask.old = NULL;   /* nothing the drawer reads points into it now */
    if (!v || s_ask.scr.phase == FLUID_ASK_NONE) return;
    const int N = s_sim->N;
    const uint8_t *own = NULL;
    if (s_crisp && s_cpk) own = cpk_cells_creature(s_cpk);   /* a rigged creature: its cells copied already */
    else if (s_cur == ST_SCENE && s_sp.cur) {                  /* a scene: its dots (copied already) in its own colours */
        for (int k = 0; k < N; k++) {
            const uint32_t c = s_sim->tcol[k];
            s_frame->col[k] = (fluid_rgba16_t){ (uint16_t)((c & 255) * 257), (uint16_t)((c >> 8 & 255) * 257), (uint16_t)((c >> 16 & 255) * 257), 0 };
        }
    } else {                                                   /* the fluid: its ink dithered onto the dots */
        for (int j = 0; j < s_sim->H; j++) for (int i = 0; i < s_sim->W; i++) {
            const int k = j * s_sim->W + i;
            float sub[8];
            for (int q = 0; q < 8; q++) sub[q] = s_frame->view[k] * (1.f / FQ_UNIT);
            s_sdots_d[k] = s_sim->mask[k] ? fluid_braille_dither(sub, i, j, 0.f, 0) : 0;
        }
    }
    fluid_askview_compose(v, s_sdots_d, own, s_frame->col);
    s_dnletters = fluid_askview_letters(v, s_dletters, FLUID_MAX_LETTERS);
    fluid_askview_opts(v, &s_dopts, s_sdots_d, s_dletters, s_dnletters);   /* never the state's melt: its words stay */
}

/* ── (Re)build for a grid ───────────────────────────────────────────────────── */

static void free_grid(void) {
    fluid_fine_destroy(s_fine); s_fine = NULL;
    fluid_frame_destroy(s_frame); s_frame = NULL;
    fluid_draw_destroy(s_draw); s_draw = NULL;
    fluid_text_destroy(s_tx);   s_tx = NULL;
    fluid_sim_destroy(s_sim);   s_sim = NULL;
}

/* caller must own the drawer (s_idle taken, or the drawer not running) */
static esp_err_t build_grid(int W, int H) {
    free_grid();
    s_sim = fluid_sim_create(W, H, s_stage);
    if (s_sim) s_tx = fluid_text_create(s_sim);
    if (s_tx) s_draw = fluid_draw_create(W, H, s_fb_w, s_fb_h);
    if (s_draw) s_frame = fluid_frame_create(W, H);
    if (!s_frame) { free_grid(); return ESP_ERR_NO_MEM; }
    s_fine = fluid_fine_create(W, H, s_fb_w, s_fb_h);    /* NULL: scenes keep the braille haze */
    s_noverlay = s_dnletters = 0;
    s_hd = (scene_hd_t){ 0, false, 1.f };             /* nothing held from the old grid */
    s_full_flush = true;
    s_frame_no = 0;
    return ESP_OK;
}

/* ── Input ──────────────────────────────────────────────────────────────────── */

static int64_t s_touch_t0;            /* sim: tap seen, not yet handed to the drawer */
/* one bump of a tapped spot's wave, at (x, y) glass units, y up (scene_outline) */
static void outline_bump(void *ctx, float x, float y) { (void)ctx; ripple(x, -y, SCENE_OUTLINE_WAVE, SCENE_OUTLINE_R); }
static volatile int64_t s_dtouch;     /* drawer: tap time riding with the current frame */
/* the touch panel's last read (the drawer polls it, off the sim's core: ~1 ms of I2C and
 * waits a poll); s_mutex */
typedef struct { int n; uint16_t x, y; int64_t t; unsigned seq; } touch_read_t;
static touch_read_t s_tp;
static void poll_touch(int n, const uint16_t *xs, const uint16_t *ys, int64_t t) {
    if (n <= 0) { s_touch_down = false; return; }
    float x = (float)(xs[0] - s_ox) / (float)s_stage * 2.f - 1.f, y = (float)(ys[0] - s_oy) / (float)s_stage * 2.f - 1.f;
    if (!s_touch_down) {                                    /* tap = ripple */
        s_touch_down = true; s_tpx = x; s_tpy = y; s_last_tx = xs[0]; s_last_ty = ys[0];
        /* on a scene's spot a wave leaves the outline of what it grows (that stays still); the finger's ripple too (on fine
         * dust it carries the dust over the whole glass), unless the spot opens a layout (the picture changes on still water) */
        const int spot = s_cur == ST_SCENE && s_sp.cur ? scene_spot_at(s_sp.cur, s_sp.t, x, -y) : -1;
        /* a tap on a question's answer: its own wave goes from there (fluid_askview_answer), not a finger's */
        const bool answer = s_ask.scr.phase == FLUID_ASK_SHOW && s_ask.scr.t >= FLUID_ASK_READY && s_ask.v && fluid_ask_pick(&s_ask.hit, x, y) >= 0;
        if (spot >= 0 && fluid_fine_dust_kind_get() == FINE_DUST_SPECKS) scene_outline(s_sp.cur, spot, outline_bump, NULL);
        if (!answer && (spot < 0 || !s_sp.cur->items[spot].nopen)) ripple(x, y, 1.6f, 0.09f);
        fd_user(&s_dir);                                    /* wakes a sleepy creature */
        if (s_cur == ST_CREATURE && s_cpk) cpk_come(s_cpk, NULL);   /* a rigged one comes to the front, attending */
        if (!s_touch_t0) s_touch_t0 = t;
        return;
    }
    /* the SPD2010 sometimes reports a garbage coordinate: a finger can't move >140 px
     * (of 412) in one ~16 ms poll, so re-baseline instead of stirring */
    if (abs((int)xs[0] - s_last_tx) > s_touch_jump || abs((int)ys[0] - s_last_ty) > s_touch_jump) {
        s_tpx = x; s_tpy = y; s_last_tx = xs[0]; s_last_ty = ys[0];
        return;
    }
    s_last_tx = xs[0]; s_last_ty = ys[0];
    float dx = x - s_tpx, dy = y - s_tpy;                   /* drag = trail + push */
    if (dx * dx + dy * dy < 0.03f * 0.03f) return;
    fluid_sim_push(s_sim, x, y, dx * s_sim->W * 6.f, dy * s_sim->W * 6.f, 0.16f);
    ripple(x, y, 0.5f, 0.06f);
    s_tpx = x; s_tpy = y;
}

static void read_tilt(float gain) {
    molty_imu_sample_t im;
    if (!molty_imu_get(&im)) im.ax = im.ay = 0.f;          /* no IMU: no tilt */
    float gx = FLUID_IMU_SX * im.ax, gy = FLUID_IMU_SY * im.ay;
    if (!isfinite(gx) || !isfinite(gy) || gain == 0.f) gx = gy = 0.f;
    if (gx > 1.f) gx = 1.f;
    if (gx < -1.f) gx = -1.f;
    if (gy > 1.f) gy = 1.f;
    if (gy < -1.f) gy = -1.f;
    s_gx += (gx * gain - s_gx) * 0.15f;
    s_gy += (gy * gain - s_gy) * 0.15f;
    s_sim->p.tilt_x = s_gx * FLUID_TILT_ACCEL;
    s_sim->p.tilt_y = s_gy * FLUID_TILT_ACCEL;
}

static inline float frand(void) { return (esp_random() & 0xFFFFFF) / 16777216.f; }

/* ── Frame (core 1: sim) ─────────────────────────────────────────────────────
 * PIPELINE: core 1 simulates frame N+1 while the drawer on core 0 rasterises and
 * flushes frame N from a snapshot (fluid_frame_t). Frame period = max(sim, draw +
 * flush) instead of their sum. Handshake: s_idle (drawer free → snapshot may be
 * overwritten) / s_go (snapshot ready). Measured before the split: sim 34 ms, draw
 * 17 ms, flush 9 ms per frame at 52×32. */

static void sim_frame(float rdt, const fluid_cfg_t *cfg) {
    fluid_sim_t *s = s_sim;
    s->p.speed_mul = cfg->speed;
    s_tnow += rdt;

    /* breathing: a slow swell of the whole field */
    s_breath_ph += rdt / 4.f;
    if (s_breath_ph > 1000.f) s_breath_ph -= 1000.f;
    float breath = 0.5f - 0.5f * cosf(s_breath_ph * 6.2831853f);
    float swell = 1.f + 0.05f * (breath * 2.f - 1.f);

    /* director: the reel, the reader's return, mood holds + flashes, idle life, sleep,
     * status. A question, a listen or a say owns the screen (no idle life); a question's
     * labels cover the creature (no action plays under them). */
    const bool covered = s_ask.scr.phase == FLUID_ASK_SHOW;
    {   /* a scene that runs out: the one under it (a tap had opened it), the next in the queue, or none: said to the relay */
        const unsigned gen0 = s_sp.gen;
        /* its end waits through a turn: a listen, a say over it, and between them the wait for the answer
         * (SCENE_TURN_S after the listen) and the agent thinking or at work, SCENE_THINK_S at most (a mood left on
         * never keeps a scene up) */
        static float turn_until, thought;
        if (s_listen) turn_until = s_tnow + SCENE_TURN_S;
        else if (s_follow || s_sub) turn_until = 0.f;
        const bool voice = s_listen || s_follow || s_sub;
        const bool think = s_pg.on > 0.f || fd_working(&s_dir) || fd_mood(&s_dir) == CM_THINKING || s_tnow < turn_until;
        thought = !think ? 0.f : voice ? thought : thought + rdt;
        /* (its clock runs on meanwhile, so what it lights in turn still does: only its end waits; and so while a
         * reading plays over it. While the picture changes into it, the dust reshaping, its clock waits too.) */
        const bool turn = voice || (think && thought < SCENE_THINK_S) || s_rd_on;
        scene_tick(&s_sp, s_cur != ST_SCENE ? rdt : fluid_draw_morphing(s_draw) ? 0.f : turn ? fminf(rdt, fmaxf(0.f, scene_room(&s_sp) - SCENE_END_ROOM_S)) : rdt);
        if (s_sp.gen != gen0) scene_event("end", NULL);
        /* a layout a tap opened, up whole: the tapped spot's reading plays over it */
        char say[PKG_SAY_MAX + 1];
        if (s_cur == ST_SCENE && s_sp.back && s_sp.say[0] && !fluid_draw_morphing(s_draw) && s_sp.t > 0.f) {
            const int n = scene_open_say(&s_sp, say, sizeof(say));
            if (n > 0 && !s_sub) read_text(say, n);        /* (a say already over it has the subtitle: no reading after) */
        }
    }
    /* a scene after a scene (play over one, or the queue's next): it flows in; from another
     * state, entering the scene state did that this frame (its clock is still 0) */
    if (scene_take_start(&s_sp) && s_cur == ST_SCENE && s_dir.state_t > 0.f) {
        fd_replay(&s_dir);
        if (!s_fine) ripple(0.f, 0.f, 0.6f, 0.2f);         /* (on fine dust the change is the dust's own: fluid_morph_t) */
    }
    fd_set_scene(&s_dir, scene_playing(&s_sp));
    fd_set_busy(&s_dir, s_listen || s_follow, covered);
    if (atomic_load(&s_bye) && !s_dir.bye) fd_bye(&s_dir);
    fd_tick(&s_dir, rdt, s_cur == ST_TEXT && s_word_until > 1e8f && !s_follow);
    direct(cfg);
    if (scene_playing(&s_sp) && !fd_scene_wanted(&s_dir)) scene_clear(&s_sp);   /* another state took the screen */
    {   /* an action to start: an agent's, or idle life's (never a sound) */
        int a = fd_take_act(&s_dir);
        if (a >= 0) cr_act(a);
    }

    const bool asking = s_ask.scr.phase == FLUID_ASK_SHOW;
    if (s_ask.scr.phase != FLUID_ASK_NONE) {               /* the question's clocks */
        s_ask.scr.t += rdt;
        if (s_ask.scr.phase != FLUID_ASK_SHOW) s_ask.scr.out_t += rdt;
    }
    if (s_sub) {   /* the subtitle follows the voice; its last word stays a moment, then goes */
        if (s_follow) { if (s_follow_word >= 0) s_sub_word = s_follow_word < s_ntok ? s_follow_word : s_ntok - 1; s_sub_until = s_tnow + 1.5f; }
        else if (s_tnow >= s_sub_until || s_cur != ST_SCENE) { s_sub = false; s_sub_word = -1; }
    }
    {   /* a tapped spot: its reading, word by word, until the voice or another state takes over */
        const unsigned spot = atomic_exchange(&s_spot_req, 0);
        if (spot == MOLE_SPOT_BACK) {                      /* a tap beside an opened layout's spots: back under it */
            if (s_cur == ST_SCENE && scene_back(&s_sp)) { s_rd_on = false; s_focus = true; scene_event("back", NULL); }
        } else if (spot && s_cur == ST_SCENE && s_sp.cur && spot >> 8 == SPOT_GEN(s_sp.gen) && (int)(spot & 255) <= s_sp.cur->nitems) {
            const int n = (int)(spot & 255) - 1;
            const char *err = NULL;
            scene_t *up = scene_open_build(s_sp.cur, n, &err);   /* the spot opens a layout: that plays, over this one */
            if (up) {
                char id[PKG_STR_MAX + 1];
                snprintf(id, sizeof(id), "%.*s", (int)s_sp.cur->items[n].nsid, s_sp.cur->say + s_sp.cur->items[n].sid);
                scene_open(&s_sp, up, n);
                s_focus = true;
                s_rd_on = false;
                scene_event("open", id);
            } else {
                if (err) mole_diag_event(false, "scene.open %s", err);
                read_spot(s_sp.cur, n);
                scene_grow_tap(&s_grow, n);                /* its drawing grows with the tap's ripple */
                s_grow_gen = s_sp.gen;
            }
        }
        if (s_rd_on && (s_sub || s_cur != ST_SCENE)) s_rd_on = false;
        else if (s_rd_on && s_tnow >= s_rd->next) {
            if (s_rd->word + 1 < s_rd->n) s_rd->next = s_tnow + word_delay_s(s_rd->text, &s_rd->toks[++s_rd->word], SPOT_READ_WPM);
            else if (s_tnow >= s_rd->next + SPOT_READ_HOLD) s_rd_on = false;
        }
        /* held grown while its reading (or the agent's say over the scene) plays */
        if (s_grow.spot >= 0 && (s_cur != ST_SCENE || !s_sp.cur || s_sp.gen != s_grow_gen)) s_grow = (scene_grow_t){ -1, 0.f, 0.f };
        scene_grow_tick(&s_grow, s_rd_on || s_sub, rdt);
        if (s_sp.cur) scene_grow_apply(&s_grow, s_sp.cur);
    }
    /* speed reader (paused under a question: the labels own s_tx) */
    if (!asking && s_cur == ST_TEXT && s_word < s_ntok && s_follow) {
        /* following a say: each word when the speaker reaches it (link_audio's cues),
         * never on the reader's own clock; once the say is over, the clock takes the rest */
        const int t = s_follow_word < s_ntok ? s_follow_word : s_ntok - 1;
        if (t >= 0 && (t > s_word || s_text_hidden)) {
            if (t > s_word) s_word = t;
            s_text_hidden = false;
            show_word(s_word);
            ripple(0.f, 0.f, 0.45f + 0.6f * 0.3f, 0.12f);
        }
        s_word_until = s_tnow + word_delay_s(s_ttext, &s_toks[s_word], cfg->wpm);
    } else if (!asking && s_cur == ST_TEXT && s_word < s_ntok && s_text_hidden) {
        /* the say ended (or failed) before word 0 was cued: the reader's own pace from word 0 */
        s_text_hidden = false;
        show_word(s_word);
        s_word_until = s_tnow + word_delay_s(s_ttext, &s_toks[s_word], cfg->wpm);
    } else if (!asking && s_cur == ST_TEXT && s_word < s_ntok && s_tnow >= s_word_until) {
        s_word++;
        if (s_word < s_ntok) {
            show_word(s_word);
            s_word_until = s_tnow + word_delay_s(s_ttext, &s_toks[s_word], cfg->wpm);
            ripple(0.f, 0.f, 0.45f + 0.6f * 0.3f, 0.12f);  /* a soft ring leaves the word */
        } else {
            s_word = s_ntok - 1; s_word_until = 1e9f;      /* hold the last word (the director
                                                            * moves on, or holds it) */
        }
    }
    s_melt = 0.f;
    if (s_cur == ST_TEXT) {
        s_noverlay = s_tx->nletters;
        memcpy(s_overlay, s_tx->letters, sizeof(fluid_letter_t) * (size_t)s_noverlay);
    } else if (s_cur != ST_SCENE) {                        /* a scene's letters: scene_fill below */
        s_melt_t += rdt; s_melt = s_melt_t;
        if (s_melt_t > 0.8f) s_noverlay = 0;               /* condensed letters melt away */
    }

    /* hold: crystallise the shape once the flow had time to arrive */
    s->p.hold = fd_solidity(&s_dir, cfg->speed);
    s->p.tint_rate = 3.f + 9.f * s->p.hold;

    /* input before the targets so the lean follows the hand of the moment */
    read_tilt(cfg->tilt);
    float ox = s_gx * FLUID_TILT_LEAN, oy = s_gy * FLUID_TILT_LEAN;

    /* a listen on the water, never over a question: a fixed circle at the rim, and each rise of
     * the voice sends a round wave from it across the screen (fluid_ring.h). The circle is a
     * target of the fluid, around whatever the state shows. */
#ifdef MOLTY_DIAG_RING
    fluid_ring_tick(&s_rg, rdt, (s_listen || esp_timer_get_time() < s_diag_ring_until) && !asking);
#else
    fluid_ring_tick(&s_rg, rdt, s_listen && !asking);
#endif
    {   /* the agent's progress: on from its first word until done, or PROG_TTL_S without news */
        static unsigned seen; static float at = -1.f;
        const unsigned q = atomic_load(&s_prog_seq);
        if (q != seen) { seen = q; at = s_tnow; }
        const bool done = atomic_exchange(&s_prog_end, false);
        if (done || atomic_exchange(&s_prog_stop, false)) at = -1.f;   /* done flashes first; a stop just lets go */
        const int32_t pv = atomic_load(&s_prog_in);
        fluid_progress_tick(&s_pg, rdt, at >= 0.f && s_tnow - at < PROG_TTL_S, pv < 0 ? -1.f : pv * 0.001f, done);
    }
    /* the floor: the dim fluid on a ground plane under a creature, flat behind anything else */
    s_floor_w += ((s_cur == ST_CREATURE ? 1.f : 0.f) - s_floor_w) * fminf(1.f, 2.f * rdt);
    s->p.floor = s_floor_w;
    const float acc = cfg->palette == FLUID_PAL_PASTEL ? 0.f : 1.f;   /* mono palettes: the accent */

    switch (s_cur) {   /* under a question too: it keeps moving */
    case ST_TEXT:     fluid_text_fill(s_tx, s, s_tnow); break;
    case ST_FX:       fluid_target_effect(s, s_cur_fx, s_tnow, swell, ox, oy); break;
    case ST_THINKING: fluid_target_blob(s, s_tnow * 1.4f, swell * 0.85f, ox, oy); break;
    case ST_SCENE: {
        const scene_caption_t cap = scene_caption();
        scene_shapes_t hd = { s_shp, 0, s_kdots, s_still };
        const int64_t fill_t0 = esp_timer_get_time();
        scene_fill(s, s_sp.cur, s_sp.t, &cap, s_overlay, &s_noverlay, s_sdots, &hd);
        {   /* the rings over it: a listen's (red, or the colour given), the agent's progress (green), a comet while it thinks */
            const bool thinking = !s_listen && (fd_working(&s_dir) || fd_mood(&s_dir) == CM_THINKING);
            hd.n = scene_rings(s_shp, hd.n, s->stage, &s_rg, &s_pg, thinking,
                               (uint32_t)(s_ring_rgb >= 0 ? s_ring_rgb : s_ring_def >= 0 ? s_ring_def : FLUID_CRISP_LISTEN_RGB), FLUID_CRISP_PROG_RGB);
            /* the voice on a listen's ring: small touches of the water just inside it, more the louder */
            static float due;
#ifdef MOLTY_DIAG_RING
            const float voice = s_diag_voice && esp_timer_get_time() < s_diag_ring_until ? diag_voice(s_tnow) : s_voice;
#else
            const float voice = s_voice;
#endif
            if (s_rg.on >= 1.f && voice > 0.08f) {
                for (due += voice * HD_VOICE_RATE * rdt; due >= 1.f; due -= 1.f) {
                    const float a = frand() * 6.2831853f;
                    ripple((SCENE_RING_R - 0.05f) * cosf(a), (SCENE_RING_R - 0.05f) * sinf(a), HD_VOICE_S * (0.5f + 0.5f * voice), HD_VOICE_R);
                }
            } else due = 0.f;
        }
        s_nshp = hd.n;
        {   /* the caption's backdrop: one box for the whole reading (or say), so the dark does not move with each word */
            static const char *box_txt; static int box_n; static unsigned box_ser;
            const char *txt = s_sub ? s_ttext : s_rd_on ? s_rd->text : NULL;
            const ftok_t *toks = s_sub ? s_toks : s_rd->toks;
            const int nt = s_sub ? s_ntok : s_rd->n;
            const unsigned ser = s_sub ? (unsigned)s_seen_ver : s_rd_ser;
            s_cap_box_on = cap.n > 0 && txt && s_sp.cur;
            if (s_cap_box_on && (txt != box_txt || nt != box_n || ser != box_ser)) {
                box_txt = txt; box_n = nt; box_ser = ser;
                for (int q = 0; q < nt; q++) {
                    float b[4];
                    const scene_caption_t c = caption(txt, toks, nt, q);
                    scene_caption_box(&c, s_sp.cur->caption_y, s->stage, b);
                    if (!q) { memcpy(s_cap_box, b, sizeof(b)); continue; }
                    s_cap_box[0] = fminf(s_cap_box[0], b[0]); s_cap_box[1] = fmaxf(s_cap_box[1], b[1]);
                    s_cap_box[2] = fminf(s_cap_box[2], b[2]); s_cap_box[3] = fmaxf(s_cap_box[3], b[3]);
                }
            }
            if (!s_cap_box_on) box_txt = NULL;
        }
        /* its fine dust: for the drawing at full growth (the calm one kept), built a few ms a frame */
        if (s_fine && hd.n >= 0) {
            static uint32_t wanted;                        /* the shapes are painted again only when they change */
            const uint32_t k0 = scene_full_key(s, s_sp.cur, s_sp.t);
            if (k0 != wanted || (s_fine->live < 0 && s_fine->ready < 0 && s_fine->build < 0)) {
                uint32_t key;
                const int nf = scene_full_shapes(s, s_sp.cur, s_sp.t, s_fshp, &key);
                /* (not now: the scene's calm set is still being made; asked again next frame) */
                if (nf >= 0 && fluid_fine_want(s_fine, s_sp.cur->serial, key, !(s_sp.cur->focus >= 0 && s_sp.cur->glow > 0.f), s_fshp, nf, cfg->palette)) wanted = k0;
            }
            /* ... in what the frame's fill left of its budget (a frame that repainted its items builds nothing) */
            const int mph = fluid_draw_morphing(s_draw);   /* (the scene is not on the glass yet: the drawer has room) */
            const bool ahead = s_hd.out || s_hd.dust >= 1.f || mph;
            const int left = (ahead ? FINE_AHEAD_US : FINE_BUILD_US) - (int)(esp_timer_get_time() - fill_t0);
            const bool was = s_fine->levels_ok, built = s_fine->build >= 0;
            if (left > 500) fluid_fine_work(s_fine, left);
            if (!was && s_fine->levels_ok) {               /* ready to swap in: how long it took, for the log */
                int calls;
                const int us = fluid_fine_built(s_fine, &calls);
                mole_diag_event(false, "fine.%s %dms in %d frames%s", built ? "build" : "known", us / 1000, calls, ahead ? " (ahead)" : "");
            }
        }
        /* a new word: a gentle wave under its letters; the reading's end: under the last one's, as it breaks up */
        static const char *cap_was;
        static float (*cap_pts)[2];                    /* the word's letters' centres (PSRAM) */
        static int ncap_pts;
        if (!cap_pts) cap_pts = heap_caps_calloc(FLUID_MAX_LETTERS, sizeof(float) * 2, MALLOC_CAP_SPIRAM);
        if (cap_pts && ((cap.n > 0 && cap.str != cap_was) || (cap.n <= 0 && cap_was))) {
            if (cap.n > 0) {
                ncap_pts = 0;
                for (int q = 0; q < s_noverlay && ncap_pts < FLUID_MAX_LETTERS; q++) {
                    const fluid_letter_t *L = &s_overlay[q];
                    if (L->mask || L->rgb || L->ch == ' ') continue;   /* the subtitle's letters: the palette's */
                    cap_pts[ncap_pts][0] = (L->x + L->w * 0.5f) / s->stage * 2.f - 1.f;
                    cap_pts[ncap_pts++][1] = (L->y + L->h * 0.5f) / s->stage * 2.f - 1.f;
                }
            }
            for (int q = 0; q < ncap_pts; q++) ripple(cap_pts[q][0], cap_pts[q][1], SCENE_CAPTION_RIPPLE, SCENE_CAPTION_RIPPLE_R);
            if (cap.n <= 0) ncap_pts = 0;
            s_cap_word = cap.n > 0 ? s_cap_word % 0xFFFFu + 1u : 0;
            s_cap_t0 = s_tnow;
        }
        cap_was = cap.n > 0 ? cap.str : NULL;
        break;
    }
    case ST_CREATURE:
        if (cr_loaded()) {
            if (s_cpk) { cpk_set_work(s_cpk, fd_working(&s_dir)); cpk_frame(s_cpk, s, rdt, swell); }    /* engine + player + body + ambient + fill */
            else cute_rt_update(s_cute, rdt);              /* mood engine + kit + behaviours */
            {   /* paddle's strokes splash the real water, either side of the creature */
                static float act_t0;
                const float at = cr_act_time();
                const int stroke = cute_act_splash(cr_acting(), at < act_t0 ? 0.f : act_t0, at);
                act_t0 = at;
                if (stroke) ripple(stroke & 1 ? -0.6f : 0.6f, 0.5f, 1.2f, 0.09f);
            }
            if (s_cute) { cute_rt_raster(s_cute, s->W, s->H); cute_rt_fill(s_cute, s, swell); }
            s_cute_tris = s_cpk ? cpk_tris(s_cpk) : cute_rt_tris(s_cute);
        } else {
            fluid_target_blob(s, s_tnow, swell, ox, oy);   /* creature unloaded while showing */
        }
        break;
    default:          fluid_target_blob(s, s_tnow, swell, ox, oy); break;
    }
    fluid_sim_floor_track(s, rdt, s_cur == ST_CREATURE && cr_loaded());   /* its feet, before the rings join T */
    /* under the listening ring (it wins); over a rigged creature the same thin line, in green */
    const bool over_hd = s_cur == ST_SCENE && s_hd.gen != 0 && s_nshp >= 0;   /* (there they are shapes of the scene: scene_rings) */
    if (!over_hd) fluid_target_progress(s, &s_pg, s_crisp ? FLUID_CRISP_PROG_RGB : PROG_COLOR, acc);
    if (!s_crisp && !over_hd) fluid_target_ring(s, &s_rg, fluid_ring_rgb(s_ring_rgb, s_ring_def, cfg->palette), acc);   /* a rigged
                                                                 * creature's is given to it whole (cpk_listen) */
    /* the always-on dim fluid; its slow noise (and the eddies' ψ) refresh on
     * alternating frames — both drift ≤ 0.012 lattice units per frame */
    /* a scene in high definition shows the surface (its dust) and the colours, never the ink: the
     * ink holds still under it (no background, eddies, flow or view) and moves on after */
    s_crisp = s_cur == ST_CREATURE && s_cpk && cpk_crisp(s_cpk);   /* a question's arcs and letters show over it too */
    /* a scene in high definition (a listen's ring and the agent's progress are shapes over it: ring_shapes), but on the
     * dots while a question is on the water, dimmed under it (before `still`: a scene's first frame is one already,
     * with no step of the whole fluid) */
    {   /* on fine dust a scene comes and goes by the drawer's change of picture (fluid_draw_morph_look): it is whole here
         * at once; on the dots it breaks into dust and forms from it (scene_hd_tick) */
        const bool scene = s_cur == ST_SCENE && s_sp.cur, plain = s_ask.scr.phase != FLUID_ASK_NONE;
        static bool fine_was;
        const bool fine = scene && s_fine && s_nshp >= 0;
        if (fine || (fine_was && !scene)) s_hd = fine && !plain ? (scene_hd_t){ s_sp.gen, false, 0.f } : (scene_hd_t){ 0, false, 1.f };
        else scene_hd_tick(&s_hd, scene ? s_sp.gen : 0, plain, rdt);
        fine_was = fine;
    }
    const bool still = (s_hd.gen != 0 && s_cur == ST_SCENE && !asking) || s_crisp;
    s->p.still = still;
    if (!still) fluid_sim_background(s, s_tnow, breath, (s_frame_no & 1) == 0 || s_frame_no < 2);
    if (s_cur == ST_FX && !asking) fluid_effect_tick(s, s_cur_fx, rdt, &s_fx_state);

    /* surface drivers (main.js): thinking = the motor's fine standing shimmer; the creature's
     * speech envelope rings the surface from its mouth; mic adds shimmer + random drops */
    int cmood = (s_cur == ST_CREATURE && cr_loaded() && !asking) ? cr_mood() : -1;
    bool thinking = s_cur == ST_THINKING || cmood == CM_THINKING;
    float level = cmood == CM_SPEAKING ? cr_env() : 0.f;
    s->osc.x = 0.f; s->osc.y = cmood == CM_SPEAKING ? 0.12f : 0.f;
    s->osc.amp = (cmood == CM_SPEAKING ? 1.2f : thinking ? 0.25f : 0.5f) * level;
    s->osc.freq = 2.5f + 6.f * 0.4f;
    s->vibe.amp = (thinking ? 0.9f : 0.f) + cfg->mic * 1.4f;
    s->vibe.freq = 7.f + 9.f * 0.3f;
    if (cfg->mic > 0.f && frand() < cfg->mic * 0.35f * rdt * 60.f)
        ripple(frand() * 1.6f - 0.8f, frand() * 1.6f - 0.8f, 0.4f + cfg->mic, 0.06f);

    /* fixed-step physics */
    if (!still && ((s_frame_no & 1) == 1 || s_frame_no < 2)) fluid_sim_swirl_field(s);
    s_acc += rdt;
    int n = 0;
    while (s_acc >= FLUID_STEP && n < 4) { fluid_sim_step(s, FLUID_STEP); s_acc -= FLUID_STEP; n++; }
    if (n == 4) s_acc = 0.f;
    fluid_sim_color(s, n * FLUID_STEP);                    /* colour rides the same flow, once per frame */
    if (s_crisp) {                                         /* a rigged creature: each cell its dots' colour, and the rings */
        /* the listen ring red (ring-options.html), unless the agent or the settings gave it one */
        cpk_listen(s_cpk, s_ring_rgb >= 0 ? s_ring_rgb : s_ring_def >= 0 ? s_ring_def : FLUID_CRISP_LISTEN_RGB, s_rg.ink);
#ifdef MOLTY_DIAG_RING
        cpk_voice(s_cpk, s_diag_voice && esp_timer_get_time() < s_diag_ring_until ? diag_voice(s_tnow) : s_voice);
#else
        cpk_voice(s_cpk, s_voice);
#endif
        FLUID_PROF_BEGIN(cpaint);
        cpk_paint(s_cpk, s);
        FLUID_PROF_END(FP_C_FILL, cpaint);                 /* prof_ms.c_fill: a rigged creature's paint and rings */
    }
    if (!still) fluid_sim_view(s);
    s_shimmer = 0.12f + (thinking ? 0.35f : 0.f) + cfg->mic * 0.4f;

    /* the question over it all, from the water as it is now */
    if (s_ask.v) fluid_askview_frame(s_ask.v, s, &s_ask.scr, s_tnow);
    s_frame_no++;
}

/* ── Creature (.molty): parse + build on the task; file I/O for the HTTP API ──── */

static esp_err_t alloc_bufs(void);

#define CUTE_PATH     "/spiffs/creature.molty"
#define CUTE_TMP_PATH "/spiffs/creature.molty.tmp"
/* firmware default (device/creatures/molty/duck.molty, EMBED_FILES in main/CMakeLists.txt) */
extern const uint8_t _binary_default_molty_start[];
extern const uint8_t _binary_default_molty_end[];

/* A creature is made (its package parsed, its clips or rig built: about a second for the cat) on a task of its own
 * under the screen's, so no frame waits for it, and handed to the sim whole: the sim swaps it in (adopt_creature). */
typedef struct {
    uint8_t *blob; size_t len; bool pkg;            /* what to make it from (kept until it is on screen) */
    fluid_sim_t grid; void *grid_mem;               /* the sim's grid as it was then: its size, cell centres and mask (copies) */
    cute_spec_t *spec; cute_rt_t *rt; cpk_t *k;     /* made: a .molty's, or a package's         */
    const char *err; const char *name;
    int ms;
    atomic_bool done;
} cr_job_t;
#define CR_LOAD_PRIO  1                             /* below the fluid's tasks (2)              */
#define CR_LOAD_STACK 8192                          /* PSRAM: no flash ops run on it            */
static cr_job_t s_cr_job;
static TaskHandle_t s_cr_task;

static void make_creature(cr_job_t *j) {
    const int64_t t0 = esp_timer_get_time();
    j->err = "oom"; j->name = ""; j->spec = NULL; j->rt = NULL; j->k = NULL;
    if (j->pkg) {
        j->k = cpk_create((const char *)j->blob, j->len, esp_random(), &j->err);
        if (j->k) j->name = cp_name(cpk_player(j->k));
        if (j->k && j->grid_mem) cpk_prepare(j->k, &j->grid);   /* its dots and rings too: its first frame makes nothing */
    } else if ((j->spec = heap_caps_malloc(sizeof(cute_spec_t), MALLOC_CAP_SPIRAM)) && cute_spec_parse(j->blob, j->len, j->spec, &j->err) == 0) {
        j->rt = cute_rt_create(j->spec, esp_random());
        if (!j->rt) j->err = "oom";
        else j->name = j->spec->name;
    }
    j->ms = (int)((esp_timer_get_time() - t0) / 1000);
}
static void cr_load_task(void *arg) {
    cr_job_t *j = arg;
    make_creature(j);
    atomic_store(&j->done, true);
    vTaskSuspend(NULL);                             /* the sim task deletes this one */
}

/* a job for blob (taken), with the sim's grid as it is now (the sim task) */
static void cr_job_begin(cr_job_t *j, uint8_t *blob, size_t len, bool pkg) {
    memset(j, 0, sizeof(*j));
    j->blob = blob; j->len = len; j->pkg = pkg;
    const int W = s_sim->W, H = s_sim->H, N = W * H;
    /* (only with no creature on screen yet, a boot: with one, its dots' strips hold the internal RAM the new one's would
     * want, and those would land in PSRAM for good: then its first frame makes them, once the old one is gone) */
    float *g = cr_loaded() ? NULL : heap_caps_malloc(sizeof(float) * (size_t)(W + H) + (size_t)N, MALLOC_CAP_SPIRAM);
    if (!g) return;                                 /* (no room: its first frame makes its dots and rings, as before) */
    j->grid_mem = g;
    j->grid.W = W; j->grid.H = H; j->grid.N = N; j->grid.stage = s_sim->stage;
    j->grid.xs = g; j->grid.ys = g + W; j->grid.mask = (uint8_t *)(g + W + H);
    memcpy(j->grid.xs, s_sim->xs, sizeof(float) * (size_t)W); memcpy(j->grid.ys, s_sim->ys, sizeof(float) * (size_t)H);
    memcpy(j->grid.mask, s_sim->mask, (size_t)N);
}

/* the creature made by make_creature becomes the one on screen (the sim task) */
static void adopt_creature(cr_job_t *j) {
    if (j->k && j->grid_mem && (j->grid.W != s_sim->W || j->grid.H != s_sim->H)) {   /* the grid changed meanwhile: made again for it */
        cpk_destroy(j->k);
        heap_caps_free(j->grid_mem);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (!s_cute_blob) { s_cute_blob = j->blob; s_cute_len = j->len; s_cute_pkg = j->pkg; j->blob = NULL; }
        xSemaphoreGive(s_mutex);
        heap_caps_free(j->blob);
        return;
    }
    heap_caps_free(j->blob); heap_caps_free(j->grid_mem); j->blob = NULL; j->grid_mem = NULL;
    const char *err = j->err, *name = j->name;
    cute_spec_t *spec = j->spec;
    cute_rt_t *rt = j->rt;
    cpk_t *k = j->k;
    const size_t len = j->len;
    const bool pkg = j->pkg;
    mole_diag_event(false, "creature.made %dms %s", j->ms, rt || k ? name : err);
    if (rt || k) {
        const int mood = cr_loaded() ? cr_mood() : CM_IDLE;
        cute_rt_destroy(s_cute); cpk_destroy(s_cpk);
        s_cute = rt; s_cpk = k;
        cr_set_mood(mood);
        ESP_LOGI(TAG, "creature '%s' loaded (%u B, %s)", name, (unsigned)len, pkg ? "package" : ".molty");
    } else {
        ESP_LOGW(TAG, "creature load failed: %s", err);
    }
    /* what the agent is told: the package's own info; a .molty only has its name */
    char *info = NULL;
    if (rt || k) {
        cJSON *o = k ? NULL : cJSON_CreateObject();
        if (o) { cJSON_AddStringToObject(o, "id", ""); cJSON_AddStringToObject(o, "name", name); }
        char *mo = o ? cJSON_PrintUnformatted(o) : NULL;
        const char *src = k ? cp_info(cpk_player(k)) : mo;
        if (src && (info = heap_caps_malloc(strlen(src) + 1, MALLOC_CAP_SPIRAM))) strcpy(info, src);
        cJSON_free(mo); cJSON_Delete(o);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (rt || k) { strlcpy(s_cute_name, name, sizeof(s_cute_name)); s_cute_err[0] = '\0'; }
    else strlcpy(s_cute_err, err, sizeof(s_cute_err));
    if (info) { heap_caps_free(s_cr_info); s_cr_info = info; s_cr_gen++; }
    if (rt || k) {                                     /* its own gestures, for the link and the HTTP API to find by name */
        s_nown = k ? cp_own_gestures(cpk_player(k)) : 0;
        if (s_nown > ME_OWN_MAX) s_nown = ME_OWN_MAX;
        for (int i = 0; i < s_nown; i++) {
            strlcpy(s_own[i].name, cp_gesture_name(cpk_player(k), CUTE_N_ACTS + i), sizeof(s_own[i].name));
            s_own[i].dur = cp_gesture_dur(cpk_player(k), CUTE_N_ACTS + i);
        }
        s_req_act = -1;                                /* an id meant for the creature that was */
    }
    xSemaphoreGive(s_mutex);
    if (rt || k) fd_set_own_work(&s_dir, k != NULL);
    heap_caps_free(spec);
}

/* read a whole small file into PSRAM; NULL if absent */
static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *b = heap_caps_malloc(CUTE_MAX_FILE, MALLOC_CAP_SPIRAM);
    size_t n = b ? fread(b, 1, CUTE_MAX_FILE, f) : 0;
    fclose(f);
    if (!n) { heap_caps_free(b); return NULL; }
    *len = n;
    return b;
}

/* the firmware default .molty, copied into PSRAM */
static uint8_t *default_bytes(size_t *len) {
    const size_t n = (size_t)(_binary_default_molty_end - _binary_default_molty_start);
    uint8_t *b = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (b) { memcpy(b, _binary_default_molty_start, n); *len = n; }
    return b;
}

/* the uploaded .molty (or the tmp left by a crash between remove and rename), else the
 * firmware default; the CRC check in cute_spec_parse makes a torn file harmless */
static uint8_t *creature_bytes(size_t *len, bool *is_default) {
    uint8_t *b = read_file(CUTE_PATH, len);
    if (!b) b = read_file(CUTE_TMP_PATH, len);
    *is_default = b == NULL;
    return b ? b : default_bytes(len);
}

/* the creature a boot shows: the one chosen last; with none chosen yet, an uploaded .molty
 * (a Molty from before creature packages keeps its creature), else the first built-in one */
static uint8_t *boot_creature(size_t *len, bool *pkg, char id[PKG_ID_MAX + 1]) {
    struct stat st;
    const bool chosen = pkgc_chosen(id);
    if (!chosen && (stat(CUTE_PATH, &st) == 0 || stat(CUTE_TMP_PATH, &st) == 0)) id[0] = 0;
    else if (!id[0] || !pkg_id_ok(id)) strlcpy(id, PKGC_BUILTINS[0].id, PKG_ID_MAX + 1);
    if (id[0]) {
        uint8_t *b = (uint8_t *)pkgc_bytes(id, len);
        if (!b && strcmp(id, PKGC_BUILTINS[0].id)) { strlcpy(id, PKGC_BUILTINS[0].id, PKG_ID_MAX + 1); b = (uint8_t *)pkgc_bytes(id, len); }   /* removed since */
        if (b) { *pkg = true; return b; }
    }
    bool dflt;
    *pkg = false; id[0] = 0;
    return creature_bytes(len, &dflt);
}

static void queue_creature(uint8_t *blob, size_t len, bool pkg, const char *id) {   /* takes ownership */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_cute_blob) heap_caps_free(s_cute_blob);
    s_cute_blob = blob; s_cute_len = len; s_cute_pkg = pkg;
    strlcpy(s_cr_id, id, sizeof(s_cr_id));
    xSemaphoreGive(s_mutex);
}

esp_err_t mole_fluid_creature_file_put(const uint8_t *buf, size_t len, const char **err) {
    if (alloc_bufs() != ESP_OK) { if (err) *err = "oom"; return ESP_ERR_NO_MEM; }
    cute_spec_t *spec = heap_caps_malloc(sizeof(cute_spec_t), MALLOC_CAP_SPIRAM);
    if (!spec) { if (err) *err = "oom"; return ESP_ERR_NO_MEM; }
    int rc = cute_spec_parse(buf, len, spec, err);
    heap_caps_free(spec);
    if (rc != 0) return ESP_ERR_INVALID_ARG;
    /* write tmp → fsync → verify size → remove old → rename (SPIFFS rename refuses an
     * existing destination; the loader also accepts the tmp, so no window loses it) */
    remove(CUTE_TMP_PATH);
    FILE *f = fopen(CUTE_TMP_PATH, "wb");
    if (!f) { if (err) *err = "spiffs_open"; return ESP_FAIL; }
    size_t w = fwrite(buf, 1, len, f);
    fflush(f); fsync(fileno(f)); fclose(f);
    struct stat st;
    if (w != len || stat(CUTE_TMP_PATH, &st) != 0 || (size_t)st.st_size != len) { remove(CUTE_TMP_PATH); if (err) *err = "spiffs_write"; return ESP_FAIL; }
    remove(CUTE_PATH);
    if (rename(CUTE_TMP_PATH, CUTE_PATH) != 0) { if (err) *err = "spiffs_rename"; return ESP_FAIL; }
    uint8_t *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (copy) { memcpy(copy, buf, len); queue_creature(copy, len, false, ""); }   /* hot reload */
    pkgc_choose("");                                                          /* the upload is the choice now */
    if (err) *err = "ok";
    return ESP_OK;
}

esp_err_t mole_fluid_creature_package(const char *id, char *json, size_t len) {
    if (alloc_bufs() != ESP_OK) { heap_caps_free(json); return ESP_ERR_NO_MEM; }
    queue_creature((uint8_t *)json, len, true, id);
    return ESP_OK;
}

char *mole_fluid_creature_info(unsigned *gen) {
    if (!s_mutex) return NULL;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    char *out = s_cr_info && s_cr_gen != *gen ? malloc(strlen(s_cr_info) + 1) : NULL;
    if (out) { strcpy(out, s_cr_info); *gen = s_cr_gen; }
    xSemaphoreGive(s_mutex);
    return out;
}

void mole_fluid_creature_id(char *id, size_t n) {
    id[0] = 0;
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(id, s_cr_id, n);
    xSemaphoreGive(s_mutex);
}

esp_err_t mole_fluid_creature_file_get(uint8_t **out, size_t *len, bool *is_default) {
    bool d = false;
    *out = creature_bytes(len, &d);
    if (is_default) *is_default = d;
    return *out ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ── Drawer task (core 0): what changed in the latest snapshot, to the panel ──── */

static int s_send_n, s_send_px; static int64_t s_blit_us;   /* this frame's windows to the panel (the drawer) */
static int send_win(void *ctx, int x, int y, int w, int h, const uint16_t *px) {
    (void)ctx;
    const int64_t t = esp_timer_get_time();
    const int e = (s_win2 ? molty_display_blit_async(x, y, w, h, px) : molty_display_blit(x, y, w, h, px)) == ESP_OK ? 0 : 1;
    s_blit_us += esp_timer_get_time() - t; s_send_n++; s_send_px += w * h;
    return e;
}

esp_err_t mole_fluid_framebuffer_sync(void) {
    if (!s_run || !s_fb_done) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_fb_done, 0);
    s_fb_want = true;
    return xSemaphoreTake(s_fb_done, pdMS_TO_TICKS(500)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static uint32_t s_last_wait_us;                     /* the drawer's wait for its last window's transfer, this frame */
static volatile uint32_t s_sim_work_us;             /* the sim's last frame's own work (sim_frame, the dust's build in it) */
static void draw_task(void *arg) {
    (void)arg;
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK) ESP_LOGW(TAG, "drawer: esp_task_wdt_add failed (%s)", esp_err_to_name(wdt_err));
    float dms = 0.f, lms = 0.f, pema[2] = { 0.f, 0.f };
    /* director status → mark at the screen's edge (fluid_draw_mark) */
    static const int MARK[FD_STATUS_COUNT] = { FLUID_MARK_NONE, FLUID_MARK_ARC, FLUID_MARK_DOT, FLUID_MARK_ALERT };
    int mark_drawn = FLUID_MARK_NONE, pal_drawn = -1;
    int64_t last_touch = 0;
    fluid_overlay_t ov_drawn = { 0 };                   /* the overlay on screen */
    while (s_run) {
        esp_task_wdt_reset();
        if (xSemaphoreTake(s_go, pdMS_TO_TICKS(200)) != pdTRUE) continue;
        if (!s_run) { xSemaphoreGive(s_idle); break; }
        int64_t t0 = esp_timer_get_time();
        int64_t touch_t = s_dtouch; s_dtouch = 0;
        if (t0 - last_touch >= FLUID_TOUCH_US) {        /* for the sim's next frame */
            uint16_t xs[MOLTY_TOUCH_MAX_POINTS] = { 0 }, ys[MOLTY_TOUCH_MAX_POINTS] = { 0 };
            const int n = molty_touch_poll(xs, ys, MOLTY_TOUCH_MAX_POINTS);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_tp = (touch_read_t){ n, xs[0], ys[0], t0, s_tp.seq + 1 };
            xSemaphoreGive(s_mutex);
            last_touch = t0;
        }
        uint16_t *fb = molty_display_framebuffer();   /* NULL: the display is down */
        int nc = 0;
        int ds = s_dstatus, mark = ds > 0 && ds < FD_STATUS_COUNT ? MARK[ds] : FLUID_MARK_NONE;
        /* the mark appears, goes or changes colour: one full repaint (cells under it are not dirty) */
        bool full = s_full_flush || mark != mark_drawn || s_dopts.palette != pal_drawn;
        {   /* crisp dots, and a scene on fine dust, go on whole row pairs: the panel's mirror need not follow them (its copy
             * is a third of the send); back to windows with odd edges, one full repaint first makes it whole again */
            static bool mirror_off;
            const bool crisp = s_dopts.scene && s_dopts.dots && (!s_dopts.shapes || s_dopts.fine);
            if (crisp != mirror_off) { molty_display_blit_mirror(!crisp); mirror_off = crisp; if (!crisp) full = true; }
        }
        const fluid_overlay_t ov = s_dov;
        const unsigned ask_seq = s_dask;
        int64_t t1 = t0;
        if (fb && s_draw && s_frame) {
            if (full) s_draw->force = true;
            /* overlays blend over the pixel under them: redraw the cells under the last
             * one (restores what it covered) and the next one, then draw it on top */
            else { fluid_draw_overlay_dirty(s_draw, &ov_drawn); fluid_draw_overlay_dirty(s_draw, &ov); }
            nc = fluid_draw_cells(s_draw, s_frame, &s_dopts);
            t1 = esp_timer_get_time();
            bool sent = fluid_draw_send2(s_draw, &s_dopts, &ov, mark, full, s_win, s_win2, s_win_px, send_win, NULL) >= 0;
            if (s_win2) { const int64_t tw = esp_timer_get_time(); sent &= molty_display_blit_wait() == ESP_OK; s_last_wait_us = (uint32_t)(esp_timer_get_time() - tw); s_blit_us += s_last_wait_us; }
            s_full_flush = !sent;                       /* a window that failed: repaint all */
            mark_drawn = mark; pal_drawn = s_dopts.palette; ov_drawn = ov;
            if (s_fb_want && !fluid_draw_morphing(s_draw)) {   /* the same frame into the framebuffer (a change of picture keeps the old one there: after it) */
                const fluid_rect_t t = { fb, s_fb_w, 0, 0, s_fb_w, s_fb_h };
                fluid_draw_rect(s_draw, &s_dopts, &t, true);
                fluid_draw_overlay(s_draw, &t, s_dopts.palette, &ov);
                fluid_draw_mark(s_draw, &t, s_dopts.palette, mark);
                s_fb_want = false;
                xSemaphoreGive(s_fb_done);
            }
            atomic_store(&s_glass_ask, ask_seq);        /* on the panel now */
        }
        int64_t t2 = esp_timer_get_time();
        mole_diag_panel_frame((uint32_t)(t1 - t0), (uint32_t)(t2 - t1));
        {   /* each animation's cadence (/api/log), once it has rested 10 frames: the frames its cells changed (f) and those
             * whose changes were all sent (s: must be f), its pixels a frame (the change's own for the wave), the windows,
             * the frame-to-frame intervals of this core (pan) and of the sim's clock (sim), p50 / p95 / worst ms, and its
             * worst frame's cells + send. The wave's also its length and its windows a frame. */
            static const char *const NAME[FLUID_ANIM_N] = { "wave", "grow", "word", "water" };
            enum { BINS = 64 };                         /* intervals in 1 ms bins (the last: 63 and more) */
            static struct { uint32_t f, s, rest, px, pxmax, win, worst, worst_c; uint16_t pan[BINS], sim[BINS]; int64_t began; } A[FLUID_ANIM_N];
            /* the wave's frame in parts (sums and worst, us): drawing + composing its windows, handing them to the panel
             * (each waits for the transfer before), the last transfer's wait, the cells, the sim's own work; its pixels worked
             * out one by one (kpx: composed and drawn into the kept glass) */
            enum { P_CMP, P_PUSH, P_WAIT, P_CELLS, P_SIM, P_N };
            static uint32_t wp[P_N], wpm[P_N], wband;
            static int64_t last_t0;
            static float last_sim;
            const uint32_t pan_ms = last_t0 ? (uint32_t)((t0 - last_t0 + 500) / 1000) : 0, sim_ms = (uint32_t)((s_dopts.time - last_sim) * 1000.f + 0.5f);
            last_t0 = t0; last_sim = s_dopts.time;
            const uint8_t on = s_draw ? s_draw->anim_on : 0, late = s_draw ? s_draw->anim_late : 0;
            for (int q = 0; q < FLUID_ANIM_N; q++) {
                if (on >> q & 1) {
                    if (!A[q].f++) A[q].began = t0;
                    A[q].s += !(late >> q & 1) && !s_full_flush;
                    A[q].rest = 0;
                    const uint32_t px = q == FLUID_ANIM_WAVE ? (uint32_t)s_draw->m.px : (uint32_t)s_send_px;
                    A[q].px += px; A[q].pxmax = px > A[q].pxmax ? px : A[q].pxmax;
                    A[q].win += q == FLUID_ANIM_WAVE ? (uint32_t)s_draw->m.wins : (uint32_t)s_send_n;
                    if (A[q].f > 1) { A[q].pan[pan_ms < BINS ? pan_ms : BINS - 1]++; A[q].sim[sim_ms < BINS ? sim_ms : BINS - 1]++; }
                    if ((uint32_t)(t2 - t0) > A[q].worst) { A[q].worst = (uint32_t)(t2 - t0); A[q].worst_c = (uint32_t)(t1 - t0); }
                    if (q == FLUID_ANIM_WAVE) {
                        const uint32_t v[P_N] = { (uint32_t)s_draw->m.cmp_us, (uint32_t)s_draw->m.push_us, s_last_wait_us, (uint32_t)(t1 - t0), s_sim_work_us };
                        for (int p = 0; p < P_N; p++) { wp[p] += v[p]; wpm[p] = v[p] > wpm[p] ? v[p] : wpm[p]; }
                        wband += (uint32_t)(s_draw->m.comp + s_draw->m.paint_comp);
                    }
                } else if (A[q].f && ++A[q].rest >= 10) {
                    uint32_t pc[2][3];                  /* p50, p95, worst of each core */
                    for (int c = 0; c < 2; c++) {
                        const uint16_t *h = c ? A[q].sim : A[q].pan;
                        uint32_t n = 0, acc = 0;
                        for (int b = 0; b < BINS; b++) n += h[b];
                        pc[c][0] = pc[c][1] = pc[c][2] = 0;
                        for (int b = 0; b < BINS; b++) {
                            if (!h[b]) continue;
                            acc += h[b];
                            if (!pc[c][0] && acc * 2 >= n) pc[c][0] = (uint32_t)b;
                            if (!pc[c][1] && acc * 20 >= n * 19) pc[c][1] = (uint32_t)b;
                            pc[c][2] = (uint32_t)b;
                        }
                    }
                    mole_diag_event(false, "anim %s %dms f=%u s=%u kpx=%.1f/%.1f win=%.1f pan=%u/%u/%u sim=%u/%u/%u worst=%.1f(%.1f+%.1f)",
                                    NAME[q], (int)((t0 - A[q].began) / 1000), (unsigned)A[q].f, (unsigned)A[q].s, A[q].px / 1000.f / A[q].f, A[q].pxmax / 1000.f,
                                    (float)A[q].win / A[q].f, (unsigned)pc[0][0], (unsigned)pc[0][1], (unsigned)pc[0][2], (unsigned)pc[1][0], (unsigned)pc[1][1],
                                    (unsigned)pc[1][2], A[q].worst / 1000.f, A[q].worst_c / 1000.f, (A[q].worst - A[q].worst_c) / 1000.f);
                    if (q == FLUID_ANIM_WAVE) {
                        const float n = (float)A[q].f;
                        mole_diag_event(false, "anim wave.parts cmp=%.1f/%.1f push=%.1f/%.1f wait=%.1f/%.1f cells=%.1f/%.1f simw=%.1f/%.1f comp=%.1f",
                                        wp[P_CMP] / 1000.f / n, wpm[P_CMP] / 1000.f, wp[P_PUSH] / 1000.f / n, wpm[P_PUSH] / 1000.f, wp[P_WAIT] / 1000.f / n,
                                        wpm[P_WAIT] / 1000.f, wp[P_CELLS] / 1000.f / n, wpm[P_CELLS] / 1000.f, wp[P_SIM] / 1000.f / n, wpm[P_SIM] / 1000.f, wband / 1000.f / n);
                        memset(wp, 0, sizeof(wp)); memset(wpm, 0, sizeof(wpm)); wband = 0;
                    }
                    memset(&A[q], 0, sizeof(A[q]));
                }
            }
        }
        {   /* the fine dust's own cost on this core, every ~3 s while a scene shows it (/api/log): the means, the worst
             * frame's cells + send (and its two parts), frames over 33 ms, the windows and pixels a frame, the specks
             * worked out a frame */
            static uint32_t cells_us, send_us, worst_us, worst_c, worst_s, worst_f, worst_px, wins, kpx, slow, pf0;
            fluid_fine_t *ff = s_dopts.fine;
            if (ff) {
                const uint32_t c = (uint32_t)(t1 - t0), sn = (uint32_t)(t2 - t1), fr = ff->prof_frame - pf0;   /* (fr: the dust's own part of the cells) */
                pf0 = ff->prof_frame;
                cells_us += c; send_us += sn; wins += (uint32_t)s_send_n; kpx += (uint32_t)s_send_px;
                if (c + sn > worst_us) { worst_us = c + sn; worst_c = c; worst_s = sn; worst_f = fr; worst_px = (uint32_t)s_send_px; }
                slow += c + sn > 33000;
                if (ff->prof_n >= 100) {
                    const float n = (float)ff->prof_n;
                    /* (wf, wkpx: the worst frame's dust part of its cells, ms, and the pixels it sent) */
                    mole_diag_event(false, "fine f=%.1f r=%.1f c=%.1f s=%.1f worst=%.1f(%.1f+%.1f) wf=%.1f wkpx=%.1f over=%u win=%.0f kpx=%.1f ev=%.0f sp=%d n=%u",
                                    ff->prof_frame / 1000.f / n, ff->prof_rect / 1000.f / n, cells_us / 1000.f / n, send_us / 1000.f / n,
                                    worst_us / 1000.f, worst_c / 1000.f, worst_s / 1000.f, worst_f / 1000.f, worst_px / 1000.f, (unsigned)slow, wins / n, kpx / 1000.f / n,
                                    ff->prof_eval / n, fluid_fine_specks(ff), (unsigned)ff->prof_n);
                    ff->prof_frame = ff->prof_rect = ff->prof_n = ff->prof_eval = 0; pf0 = 0;
                    cells_us = send_us = worst_us = worst_c = worst_s = wins = kpx = slow = 0;
                }
            }
        }
        if (touch_t) mole_diag_touch_latency((uint32_t)(t2 - touch_t));
        dms = dms * 0.9f + (t1 - t0) / 1000.f * 0.1f;
        lms = lms * 0.9f + (t2 - t1) / 1000.f * 0.1f;
        pema[0] = pema[0] * 0.9f + (float)fluid_prof[FP_DSAMPLE] / (float)s_cpu_mhz / 1000.f * 0.1f;
        pema[1] = pema[1] * 0.9f + (float)fluid_prof[FP_DRASTER] / (float)s_cpu_mhz / 1000.f * 0.1f;
        fluid_prof[FP_DSAMPLE] = fluid_prof[FP_DRASTER] = 0;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_stats.draw_ms = dms; s_stats.flush_ms = lms; s_stats.cells_drawn = nc; s_stats.panel_last = (t2 - t0) / 1000.f;
        s_stats.send_n = s_stats.send_n * 0.9f + s_send_n * 0.1f; s_stats.send_kpx = s_stats.send_kpx * 0.9f + s_send_px / 1000.f * 0.1f;
        s_stats.blit_ms = s_stats.blit_ms * 0.9f + s_blit_us / 1000.f * 0.1f;
        s_send_n = s_send_px = 0; s_blit_us = 0;
        {   uint32_t bp[3] = { 0, 0, 0 }; molty_display_blit_prof(bp);
            for (int q = 0; q < 3; q++) s_stats.blit_part[q] = s_stats.blit_part[q] * 0.9f + bp[q] / 1000.f * 0.1f; }
        s_stats.prof_ms[FP_DSAMPLE] = pema[0]; s_stats.prof_ms[FP_DRASTER] = pema[1];
        xSemaphoreGive(s_mutex);
        xSemaphoreGive(s_idle);
    }
    esp_task_wdt_delete(NULL);
    xSemaphoreGive(s_wdone);
    vTaskDelete(NULL);
}

/* ── Sim task (core 1) ───────────────────────────────────────────────────────── */

static void pace_wake(void *task) { xTaskNotifyGive((TaskHandle_t)task); }

/* The panel off / on, with the drawer idle (it never sends a window to a sleeping panel). */
static void screen_switch(bool on) {
    xSemaphoreTake(s_idle, portMAX_DELAY);
    esp_err_t e = molty_display_power(on);
    if (!on && e == ESP_ERR_NOT_SUPPORTED) {            /* no panel switch: a black frame */
        uint16_t *fb = molty_display_framebuffer();
        if (fb) { memset(fb, 0, (size_t)s_fb_w * s_fb_h * sizeof(uint16_t)); molty_display_flush(); }
    }
    s_full_flush = true;
    atomic_store(&s_screen, on);
    xSemaphoreGive(s_idle);
    mole_diag_event(false, "fluid.screen %s (%s)", on ? "on" : "off", e == ESP_OK ? "panel" : esp_err_to_name(e));
}

/* Screen off: something the human must see is waiting (a request the task has not taken yet). */
static bool screen_wanted(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool w = s_text_ver != s_seen_ver || (s_ask_want.active && s_ask_want.seq != s_ask.scr.seq) || (s_listen_in && !s_listen);
    xSemaphoreGive(s_mutex);
    return w;
}

static void fluid_task(void *arg) {
    (void)arg;
    /* TWDT: re-created on every select → self-subscribe; delete before exit */
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK) ESP_LOGW(TAG, "esp_task_wdt_add failed (%s)", esp_err_to_name(wdt_err));

    int64_t prev = esp_timer_get_time();
    unsigned tp_seen = 0;
    float fps = 0.f, pms = 0.f, sms = 0.f, wms = 0.f, fms = 0.f;
    /* paced on the microsecond clock: the 100 Hz tick rounds every overrun up to the next
     * 10 ms, which made 31 ms frames come every 37 to 50 ms */
    esp_timer_handle_t pace = NULL;
    const esp_timer_create_args_t pace_args = { .callback = pace_wake, .arg = xTaskGetCurrentTaskHandle(), .name = "fluid_pace" };
    ESP_ERROR_CHECK(esp_timer_create(&pace_args, &pace));
    int64_t next = esp_timer_get_time();
    bool overran = false;

    while (s_run) {
        esp_task_wdt_reset();
        if (!atomic_load(&s_screen)) {                  /* off: no sim, no drawing */
            uint16_t xs[MOLTY_TOUCH_MAX_POINTS], ys[MOLTY_TOUCH_MAX_POINTS];
            const bool touched = molty_touch_poll(xs, ys, MOLTY_TOUCH_MAX_POINTS) > 0;
            if (!touched && !screen_wanted() && !atomic_load(&s_screen_want)) { vTaskDelay(pdMS_TO_TICKS(FLUID_OFF_POLL_MS)); continue; }
            s_touch_mute = touched;
            atomic_store(&s_screen_want, true);
            screen_switch(true);
            prev = next = esp_timer_get_time();         /* no catching up on the time it was off */
        } else if (!atomic_load(&s_screen_want)) { screen_switch(false); continue; }
        int64_t now = esp_timer_get_time();
        float rdt = (float)(now - prev) / 1e6f;
        uint32_t raw_dt_us = (uint32_t)(now - prev);
        prev = now;
        if (rdt > 0.1f) rdt = 0.1f;


        /* pull config + requests */
        fluid_cfg_t cfg;
        int req = -1, req_fx = 0; bool req_reel = false, req_hold = false;
        float rip[FLUID_RIPPLE_Q][3]; int nrip = 0;
        bool text_changed = false, text_append = false;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        cfg = s_cfg;
        req = s_req_state; req_fx = s_req_fx; req_hold = s_req_text_hold; s_req_state = -1;
        const bool req_implied = s_req_implied; s_req_implied = false;
        req_reel = s_req_reel; s_req_reel = false;
        nrip = s_ripn; memcpy(rip, s_ripq, sizeof(rip)); s_ripn = 0;
        /* (a creature being made: the next one waits its turn) */
        uint8_t *cblob = s_cr_task ? NULL : s_cute_blob; size_t clen = s_cute_len; const bool cpkg = s_cute_pkg;
        if (cblob) s_cute_blob = NULL;
        int req_mood = s_req_mood; s_req_mood = -1;
        bool req_work = s_req_work; s_req_work = false;
        float req_hold_s = s_req_hold_s;
        int req_act = s_req_act; s_req_act = -1;
        bool spoke = s_req_spoke; s_req_spoke = false;
        scene_op_t sops[SCENE_QUEUE_MAX + 2];
        const int nsops = s_scene_nops;
        memcpy(sops, s_scene_ops, sizeof(scene_op_t) * (size_t)nsops); s_scene_nops = 0;
        bool regrid = s_req_regrid; s_req_regrid = false;
        const touch_read_t tp = s_tp;
        if (!s_idle_life_loaded) { s_idle_life = idle_life_load(); s_idle_life_loaded = true; }
        int idle_life = s_idle_life;
        uint32_t tint = s_tint_in;
        bool activity = s_req_activity, shake = s_req_shake, user = s_req_user;
        s_req_activity = s_req_shake = s_req_user = false;
        int status = s_status_in;
        ask_want_t want = s_ask_want;
        s_listen = s_listen_in; s_voice = s_listen_in ? s_voice_in : 0.f; s_ring_rgb = s_ring_in; s_ring_def = s_ring_dflt;
        s_follow = s_follow_in; s_follow_word = s_follow_word_in;
        if (s_text_ver != s_seen_ver) {
            memcpy(s_ttext, s_text, (size_t)s_text_len + 1);
            s_seen_ver = s_text_ver; text_changed = true;
            text_append = s_text_appended; s_text_appended = false;
        }
        xSemaphoreGive(s_mutex);

        /* hot fields moved: the same grid again, rebuilt into PSRAM (the heap guard: internal
         * RAM back to Wi-Fi / TLS / the LCD) or into what internal RAM the network left */
        const int was_n = s_sim ? s_sim->n_internal : 0;
        if (!s_sim || s_sim->W != cfg.grid_w || s_sim->H != cfg.grid_h || regrid) {
            xSemaphoreTake(s_idle, portMAX_DELAY);          /* own the drawer's buffers */
            if (build_grid(cfg.grid_w, cfg.grid_h) != ESP_OK) {
                ESP_LOGE(TAG, "grid %dx%d alloc failed — back to %dx%d", cfg.grid_w, cfg.grid_h, s_grid_w0, s_grid_h0);
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_cfg.grid_w = s_grid_w0; s_cfg.grid_h = s_grid_h0;
                xSemaphoreGive(s_mutex);
                if (build_grid(s_grid_w0, s_grid_h0) != ESP_OK) { xSemaphoreGive(s_idle); vTaskDelay(pdMS_TO_TICKS(200)); continue; }
            }
            xSemaphoreGive(s_idle);
            if (regrid)
                mole_diag_event(was_n > 0, "fluid.%s hot fields internal %d -> %d, int_free=%u", was_n > 0 ? "shed" : "internal",
                                was_n, s_sim ? s_sim->n_internal : -1, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            enter_state(s_cur, s_cur_fx, &cfg);         /* re-enter on the new grid */
            if (s_ask.v) {                              /* ...and the question over it, on the new grid (the drawer is idle) */
                fluid_askview_destroy(s_ask.v); fluid_askview_destroy(s_ask.old); fluid_askview_destroy(s_ask.making); s_ask.v = s_ask.old = s_ask.making = NULL;
                if (s_ask.scr.phase == FLUID_ASK_SHOW && want.seq == s_ask.scr.seq) s_ask.v = fluid_askview_create(s_sim, &want.q);
                if (!s_ask.v) s_ask.scr.phase = FLUID_ASK_NONE;   /* one leaving goes at once */
            }
        }
        if (text_changed) {
            tokenize();
            if (s_cur == ST_TEXT && req < 0) {
                if (!text_append) { req = ST_TEXT; req_fx = 0; req_hold = s_dir.text_hold; }   /* new text: start over */
                else if (s_word_until > 1e8f && s_word + 1 < s_ntok)    /* streamed words arrived */
                    s_word_until = s_tnow;                              /* while holding the last */
            }
        }
        if (s_cr_task && atomic_load(&s_cr_job.done)) {    /* a creature was made: it is the one on screen now */
            vTaskDeleteWithCaps(s_cr_task); s_cr_task = NULL;
            adopt_creature(&s_cr_job);
            fd_set_creature(&s_dir, cr_loaded());
        }
        if (cblob) {                                       /* a creature to make: on its own task, under this one */
            cr_job_begin(&s_cr_job, cblob, clen, cpkg);
            if (xTaskCreatePinnedToCoreWithCaps(cr_load_task, "cr_load", CR_LOAD_STACK, &s_cr_job, CR_LOAD_PRIO, &s_cr_task, tskNO_AFFINITY,
                                                MALLOC_CAP_SPIRAM) != pdPASS) {
                s_cr_task = NULL;                          /* no memory for the task: made here, as before */
                make_creature(&s_cr_job);
                adopt_creature(&s_cr_job);
                fd_set_creature(&s_dir, cr_loaded());
            }
        }
        /* the director: state first (it may end the reel), then the mood over it */
        if (req_reel) fd_autoplay(&s_dir);
        /* a drawing up stays to its end: a say's words become its subtitle, and a mood alone
         * (speaking, listening) moods the voice, never takes the screen back to the creature */
        if (s_cur == ST_SCENE && scene_playing(&s_sp)) {
            if (req == ST_TEXT && s_follow) { s_sub = true; s_sub_word = -1; req = -1; }
            else if (req == ST_CREATURE && req_implied) req = -1;
        }
        if (req >= 0) fd_request_state(&s_dir, req, req_fx, req_hold);
        for (int q = 0; q < nsops; q++) {                   /* scenes, in the order they came */
            if (sops[q].op == MOLE_SCENE_PLAY) scene_play(&s_sp, sops[q].sc);
            else if (sops[q].op == MOLE_SCENE_CLEAR) scene_clear(&s_sp);
            else if (!scene_queue(&s_sp, sops[q].sc)) scene_free(sops[q].sc);
        }
        if (nsops && scene_playing(&s_sp)) { fd_request_state(&s_dir, ST_SCENE, 0, false); fd_activity(&s_dir); }
        if (req_work) fd_agent_work(&s_dir, req_hold_s);
        else if (req_mood >= 0) fd_agent_mood(&s_dir, req_mood, req_hold_s);
        else if (activity) fd_activity(&s_dir);
        if (user) fd_user(&s_dir);
        if (shake) { fd_shake(&s_dir); if (s_cur == ST_CREATURE && s_cpk) cpk_come(s_cpk, "shaken"); }   /* its shaken, then it comes */
        fd_set_idle_life(&s_dir, idle_life);
        if (req_act >= 0) fd_agent_act(&s_dir, req_act, cr_act_dur(req_act));
        if (spoke) fd_turn_spoken(&s_dir);
        if (tint != s_tint) {                      /* a say's mood mark: its words from now on */
            s_tint = tint;
            if (s_cur == ST_TEXT && s_ask.scr.phase == FLUID_ASK_NONE) tint_letters();
        }
        fd_set_status(&s_dir, status);
        ask_follow(&want);
        direct(&cfg);
        for (int q = 0; q < nrip; q++) ripple(rip[q][0], rip[q][1], rip[q][2], 0.09f);
        if (tp.seq != tp_seen) {
            if (s_touch_mute && tp.n == 0) s_touch_mute = false;
            if (!s_touch_mute) poll_touch(tp.n, &tp.x, &tp.y, tp.t);
            tp_seen = tp.seq;
        }

        int64_t t0 = esp_timer_get_time();
        sim_frame(rdt, &cfg);
        int64_t t1 = esp_timer_get_time();
        s_sim_work_us = (uint32_t)(t1 - t0);
        /* hand the frame to the drawer: wait until it finished the previous one */
        xSemaphoreTake(s_idle, portMAX_DELAY);
        if (s_dir.bye) {   /* the goodbye's fade: the panel's level, with the drawer idle */
            const float l = fd_bye_level(&s_dir);
            if (s_bye_lvl - l >= 0.02f || (l <= 0.f && s_bye_lvl > 0.f)) {
                s_bye_lvl = l;
                molty_display_level((uint8_t)(255.f * l * l + 0.5f));
            }
            if (fd_bye_done(&s_dir)) atomic_store(&s_bye_done, true);
        }
        int64_t t2 = esp_timer_get_time();
        /* The whole picture changing (the boot word to the creature, the creature to a scene and back, a scene to a
         * scene, a question coming and going): dust on a wave from the finger, from this frame. A scene's glass, the
         * creature's and a question's are kept by the drawer as it sends them (FLUID_LOOK_KEEP) in the panel's mirror
         * (unused while their windows go on whole row pairs); the fluid's own looks (words, the blob) by the mirror
         * itself (FLUID_LOOK_GLASS): a change goes from them, never into them. A scene's dust built since comes in while
         * nothing of the change's new picture is drawn yet; till it has, the cloud waits (`ready`). */
        const bool fine_look = s_cur == ST_SCENE && s_sp.cur && s_fine && s_nshp >= 0 && s_hd.gen != 0;
        const bool asking = s_ask.v && s_ask.scr.phase != FLUID_ASK_NONE;   /* (ask_compose draws it over the frame) */
        const uint32_t look = asking ? FLUID_LOOK_KEEP | 0x20000000u | (s_ask.scr.seq & 0x1FFFFFFFu)
                            : fine_look ? FLUID_LOOK_KEEP | (s_sp.cur->serial & 0x1FFFFFFFu) : s_crisp ? FLUID_LOOK_KEEP | 1u : FLUID_LOOK_GLASS;
        const bool ready = !fine_look || asking || !fluid_fine_specks_kind(fluid_fine_dust_kind_get()) || fluid_fine_shown(s_fine, s_sp.cur->serial);
        if (s_focus) {                                     /* a tap asked for it: it starts where the finger was */
            const uint32_t at = atomic_load(&s_tap_at);
            fluid_draw_morph_focus(s_draw, (float)(at >> 16), (float)(at & 0xFFFFu));
            s_focus = false;
        }
        fluid_draw_morph_look(s_draw, look, molty_display_framebuffer(), ready);
        {
            fluid_sim_snapshot(s_sim, s_frame, cfg.edge_mode == FLUID_EDGE_OFF);
            s_dopts = (fluid_draw_opts_t){
                .palette = cfg.palette, .edge_mode = cfg.edge_mode,
                .shimmer = s_shimmer, .time = s_tnow, .melt = s_melt,
                .letters = s_dletters, .nletters = s_hd.out ? s_dnletters : s_noverlay,
                .ramp = GC_RAMP_ASCII, .font = gc_font5x7,
                /* the boot word, and its dots while it melts (each goes as the ink leaves it) */
                .dots = s_cur == ST_SCENE || s_hd.out || s_crisp ? s_sdots_d : s_word_dots && (s_cur == ST_TEXT || s_melt_t < 0.8f) ? s_tx->dots : NULL,
                .scene = s_hd.gen != 0 || s_crisp, .dust = scene_hd_dust(&s_hd), .frame = s_crisp && s_cpk ? cpk_draw_frame(s_cpk) : 0,
            };
            /* the drawer is idle (s_idle); a scene breaking into dust keeps what it showed */
            if (s_crisp) {   /* a rigged creature's cells, as a scene's dots */
                const uint8_t *cd; const uint32_t *cc;
                cpk_cells(s_cpk, &cd, &cc);
                if (cd) memcpy(s_sdots_d, cd, (size_t)s_sim->N); else memset(s_sdots_d, 0, (size_t)s_sim->N);
                int n = 0;
                for (int q = 0; q < s_sim->N; q++) n += __builtin_popcount(s_sdots_d[q]);
                s_crisp_dots = n;
            } else if (s_cur == ST_SCENE && !s_hd.out) {
                memcpy(s_sdots_d, s_sdots, (size_t)s_sim->N);
                memcpy(s_kdots_d, s_kdots, (size_t)s_sim->N);
                if (s_nshp > 0) memcpy(s_dshp, s_shp, sizeof(fluid_shape_t) * (size_t)s_nshp);
                s_dnshp = s_nshp;
            }
            if (s_hd.gen && s_dnshp >= 0) { s_dopts.shapes = s_dshp; s_dopts.nshapes = s_dnshp; s_dopts.kdots = s_kdots_d; }
            if (s_fine) {   /* the fine dust: a set built since swaps in (the drawer is idle); none once no scene shows */
                if (s_hd.gen && s_dnshp >= 0) {
                    if (s_cur == ST_SCENE && !s_hd.out) {      /* (a scene breaking up keeps the dust it showed) */
                        /* a set built since swaps in: any time but while a change of picture draws its new one */
                        if (!fluid_draw_morphing(s_draw) || fluid_draw_morph_open(s_draw)) fluid_fine_swap(s_fine);
                        fluid_fine_caption_box(s_fine, s_cap_box_on ? s_cap_box : NULL);
                        fluid_fine_rim(s_fine, s_rg.on > 0.f);
                        memcpy(s_still_d, s_still, (size_t)s_sim->N); s_dcap_word = s_cap_word;
                    }
                    s_dopts.fine = s_fine; s_dopts.still = s_still_d; s_dopts.cap_word = s_dcap_word;
                    s_dopts.cap_unformed = s_dcap_word && FINE_REVEAL_S > 0.f ? fmaxf(0.f, 1.f - (s_tnow - s_cap_t0) / (FINE_REVEAL_S + 1e-6f)) : 0.f;
                } else if (!s_hd.gen && !s_hd.out) fluid_fine_clear(s_fine);
            }
            {   /* a scene's symbols point into its package: the drawer keeps it until its next frame */
                static pkg_t *held;
                pkg_t *now = s_hd.out ? held : s_cur == ST_SCENE && s_sp.cur ? s_sp.cur->pkg : NULL;
                if (now) pkg_ref(now);
                if (held) pkg_unref(held);
                held = now;
            }
            if (!s_hd.out) { memcpy(s_dletters, s_overlay, sizeof(fluid_letter_t) * (size_t)s_noverlay); s_dnletters = s_noverlay; }
        }
        if (s_touch_t0) { s_dtouch = s_touch_t0; s_touch_t0 = 0; }
        s_dstatus = fd_status(&s_dir);
        ask_compose();
        s_dask = fluid_ask_tappable(&s_ask.scr, cfg.speed);
        xSemaphoreGive(s_go);
        int64_t t3 = esp_timer_get_time();

        mole_diag_sim_frame(raw_dt_us, (uint32_t)(t1 - t0));
        /* stats (EMA) */
        fps = fps > 0.f ? fps * 0.93f + (1.f / fmaxf(rdt, 1e-3f)) * 0.07f : 30.f;
        pms = pms * 0.9f + (t0 - now) / 1000.f * 0.1f;
        sms = sms * 0.9f + (t1 - t0) / 1000.f * 0.1f;
        wms = wms * 0.9f + (t2 - t1) / 1000.f * 0.1f;
        fms = fms * 0.9f + (t3 - t0) / 1000.f * 0.1f;
        static float prof_ema[FP_N];
        for (int q = 0; q < FP_DSAMPLE; q++) {
            prof_ema[q] = prof_ema[q] * 0.9f + (float)fluid_prof[q] / (float)s_cpu_mhz / 1000.f * 0.1f;
            fluid_prof[q] = 0;
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        memcpy(s_stats.prof_ms, prof_ema, sizeof(float) * FP_DSAMPLE);
        s_stats.fps = fps; s_stats.frame_ms = fms; s_stats.pre_ms = pms; s_stats.sim_ms = sms; s_stats.wait_ms = wms;
        s_stats.energy = s_sim->energy; s_stats.err = s_sim->err;
        s_stats.state = s_cur; s_stats.fx = s_cur_fx; s_stats.reel = s_dir.autoplay ? s_dir.reel : -1;
        s_stats.autoplay = s_dir.autoplay; s_stats.asleep = fd_asleep(&s_dir); s_stats.status = fd_status(&s_dir);
        s_stats.mood = cr_mood(); s_stats.ctris = s_cute_tris;
        s_stats.act = cr_acting();
        s_stats.working = fd_working(&s_dir);
        s_scene_nq = s_sp.nq;
        {   /* the scene's spots, for taps: copied once per scene, and only if it has any */
            const scene_t *sc = s_cur == ST_SCENE ? s_sp.cur : NULL;
            if (!sc) { s_spot_on = false; s_spot_gen = 0; }
            else if (s_spot_gen != SPOT_GEN(s_sp.gen)) {
                s_spot_gen = SPOT_GEN(s_sp.gen);
                s_spot_on = false;
                for (int q = 0; q < sc->nitems && !s_spot_on; q++) s_spot_on = sc->items[q].k == PK_SPOT;
                if (s_spot_on) memcpy(s_spot_sc, sc, sizeof(scene_t));
            }
            s_spot_t = s_sp.t;
            s_spot_back = sc && s_sp.back;
        }
        snprintf(s_stats.scene_pkg, sizeof(s_stats.scene_pkg), "%s", s_sp.cur ? s_sp.cur->pkg_id : "");
        snprintf(s_stats.scene_layout, sizeof(s_stats.scene_layout), "%s", s_sp.cur ? s_sp.cur->layout : "");
        s_stats.scene_left = s_sp.cur ? scene_end(s_sp.cur) - s_sp.t : 0.f;
        s_stats.crisp_dots = s_crisp ? s_crisp_dots : -1;
        if (s_crisp && s_cpk) {    /* the creature's rows between the cores: more here while the drawer (core 0) is the slower */
            static float share = 55.f;
            /* the drawer (core 0) slower: more here; this core waiting on the other's band (k_wait): more here too;
             * this core slower and not waiting: less. k_wait has a floor of ~0.5 ms with no real wait (the band's
             * wake-ups across the cores), so its thresholds sit above that: at 0.5 / 1 ms its noise pushed the share
             * up 3 a frame and kept it from coming down, pinned at 80 while the band's core sat idle (walking 29 ms). */
            /* d from the last frames, not the stats' slow averages: a ring's wave loads the drawer within a frame or two and
             * is gone in a second (averaged, the share arrived after it) */
            static float df;
            df = df * 0.5f + (s_stats.panel_last - (t1 - t0) / 1000.f) * 0.5f;
            const float d = df, kw = prof_ema[FP_K_WAIT];
            const float step = fminf(5.f, fabsf(d) * 0.5f);   /* faster the further apart (the ring's wave comes and goes in a second) */
            share += d > 1.f ? step : kw > 1.5f ? 1.f : d < -1.f && kw < 1.f ? -step : 0.f;
            share = share < 30.f ? 30.f : share > 90.f ? 90.f : share;
            cpk_set_share(s_cpk, (int)share); s_stats.share = (int)share;
        }
        s_stats.inside = s_sim->inside; s_stats.grid_w = s_sim->W; s_stats.grid_h = s_sim->H;
        s_stats.internal = s_sim->internal;
        s_stats.n_internal = s_sim->n_internal; s_stats.n_blk = s_sim->n_blk;
        s_stats.word_idx = s_word; s_stats.nwords = s_ntok;
        s_stats.word[0] = '\0';
        if (s_cur == ST_TEXT && s_word < s_ntok) {
            int wl = s_toks[s_word].len; if (wl > 39) wl = 39;
            memcpy(s_stats.word, s_ttext + s_toks[s_word].off, (size_t)wl); s_stats.word[wl] = '\0';
        }
        xSemaphoreGive(s_mutex);

        /* pace at 33.3 fps. An overrun starts the next frame at once, but never twice in
         * a row: lower-priority core-1 work still gets 1 ms every other frame. */
        next += fd_asleep(&s_dir) ? FLUID_SLEEP_US : FLUID_FRAME_US;
        int64_t wait = next - esp_timer_get_time();
        if (wait <= 0) { next = esp_timer_get_time(); wait = overran ? 1000 : 0; overran = !overran; }
        else overran = false;
        if (wait > 0 && esp_timer_start_once(pace, (uint64_t)wait) == ESP_OK) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
    esp_timer_stop(pace);
    esp_timer_delete(pace);
    esp_task_wdt_delete(NULL);
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

/* ── Creature vtable ────────────────────────────────────────────────────────── */

static esp_err_t alloc_bufs(void) {
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_done) s_done = xSemaphoreCreateBinary();
    if (!s_wdone) s_wdone = xSemaphoreCreateBinary();
    if (!s_go) s_go = xSemaphoreCreateBinary();
    if (!s_idle) s_idle = xSemaphoreCreateBinary();
    if (!s_fb_done) s_fb_done = xSemaphoreCreateBinary();
    if (!s_mutex || !s_done || !s_wdone || !s_go || !s_idle || !s_fb_done) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_stage) {                              /* the board's display, once */
        s_fb_w = molty_display_width(); s_fb_h = molty_display_height();
        s_stage = s_fb_w < s_fb_h ? s_fb_w : s_fb_h;
        s_ox = (s_fb_w - s_stage) / 2; s_oy = (s_fb_h - s_stage) / 2;
        s_touch_jump = (int)lrintf(140.f * (float)s_stage / FLUID_STAGE_REF);
        fluid_grid_for_stage(s_stage, &s_grid_w0, &s_grid_h0);
        s_cfg.grid_w = s_grid_w0; s_cfg.grid_h = s_grid_h0;
    }
    xSemaphoreGive(s_mutex);
    if (!s_ring_dflt_loaded) {                   /* the console's ring colour, once */
        nvs_handle_t h;
        int32_t v = -1;
        if (nvs_open("fluid", NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_i32(h, "ring", &v) != ESP_OK || v > 0xFFFFFF) v = -1;
            nvs_close(h);
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_ring_dflt = v < 0 ? -1 : v; s_ring_dflt_loaded = true;
        xSemaphoreGive(s_mutex);
    }
    if (!s_text) {
        s_text = heap_caps_malloc(FLUID_TEXT_MAX + 1, MALLOC_CAP_SPIRAM);
        if (!s_text) return ESP_ERR_NO_MEM;
        s_text_len = (int)strlen(DEFAULT_TEXT);
        memcpy(s_text, DEFAULT_TEXT, (size_t)s_text_len + 1);
        s_text_ver++;
    }
    return ESP_OK;
}

esp_err_t mole_fluid_creature_init(void) {
    esp_err_t e = alloc_bufs();
    if (e != ESP_OK) return e;
    if (s_task) {                                /* a previous task that missed its exit */
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_INVALID_STATE;
        s_task = NULL;
    }
    if (s_dtask) {
        if (xSemaphoreTake(s_wdone, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_INVALID_STATE;
        s_dtask = NULL;
    }
    s_cpu_mhz = (int)(esp_clk_cpu_freq() / 1000000);
    if (s_cpu_mhz <= 0) s_cpu_mhz = 160;
    if (!s_ttext) s_ttext = heap_caps_malloc(FLUID_TEXT_MAX + 1, MALLOC_CAP_SPIRAM);
    if (!s_toks)  s_toks  = heap_caps_malloc(FLUID_MAX_TOKENS * sizeof(ftok_t), MALLOC_CAP_SPIRAM);
    if (!s_sdots)   s_sdots   = heap_caps_calloc(1, FLUID_GRID_MAX_W * FLUID_GRID_MAX_H, MALLOC_CAP_SPIRAM);
    if (!s_sdots_d) s_sdots_d = heap_caps_calloc(1, FLUID_GRID_MAX_W * FLUID_GRID_MAX_H, MALLOC_CAP_SPIRAM);
    if (!s_kdots)   s_kdots   = heap_caps_calloc(2, FLUID_GRID_MAX_W * FLUID_GRID_MAX_H, MALLOC_CAP_SPIRAM);
    s_kdots_d = s_kdots ? s_kdots + FLUID_GRID_MAX_W * FLUID_GRID_MAX_H : NULL;
    if (!s_still)   s_still   = heap_caps_calloc(2, FLUID_GRID_MAX_W * FLUID_GRID_MAX_H, MALLOC_CAP_SPIRAM);
    s_still_d = s_still ? s_still + FLUID_GRID_MAX_W * FLUID_GRID_MAX_H : NULL;
    if (!s_fshp)    s_fshp    = heap_caps_calloc(FLUID_MAX_SHAPES, sizeof(fluid_shape_t), MALLOC_CAP_SPIRAM);
    if (!s_shp)     s_shp     = heap_caps_calloc(2 * FLUID_MAX_SHAPES, sizeof(fluid_shape_t), MALLOC_CAP_SPIRAM);
    s_dshp = s_shp ? s_shp + FLUID_MAX_SHAPES : NULL;
    if (!s_rd)      s_rd      = heap_caps_calloc(1, sizeof(reading_t), MALLOC_CAP_SPIRAM);
    if (!s_spot_sc) s_spot_sc = heap_caps_calloc(1, sizeof(scene_t), MALLOC_CAP_SPIRAM);
    if (!s_ttext || !s_toks || !s_sdots || !s_sdots_d || !s_kdots || !s_shp || !s_rd || !s_spot_sc || !s_still || !s_fshp) return ESP_ERR_NO_MEM;
    s_ttext[0] = '\0';
    s_seen_ver = -1;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int W = s_cfg.grid_w, H = s_cfg.grid_h;
    xSemaphoreGive(s_mutex);
    if (build_grid(W, H) != ESP_OK) {
        ESP_LOGE(TAG, "sim alloc failed (%dx%d), PSRAM free %u", W, H,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return ESP_ERR_NO_MEM;
    }
    if (!s_win && molty_display_blit_can_overlap()) {   /* two smaller windows: the panel sends one while the next is drawn */
        s_win_px = FLUID_WIN2_PX;
        s_win = heap_caps_malloc((size_t)s_win_px * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        s_win2 = s_win ? heap_caps_malloc((size_t)s_win_px * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) : NULL;
        if (!s_win2) { heap_caps_free(s_win); s_win = NULL; }   /* not both: one big one, as before */
    }
    if (!s_win) {                                /* after the grid: its hot fields come first */
        s_win_px = s_fb_w * FLUID_WIN_ROWS;
        s_win = heap_caps_malloc((size_t)s_win_px * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_win) s_win = heap_caps_malloc((size_t)s_win_px * sizeof(uint16_t), MALLOC_CAP_SPIRAM);   /* bounced, slower */
        if (!s_win) { free_grid(); return ESP_ERR_NO_MEM; }
    }
    {   /* the chosen creature (boot_creature), built on the task */
        size_t clen = 0; bool pkg = false; char id[PKG_ID_MAX + 1];
        uint8_t *cb = boot_creature(&clen, &pkg, id);
        if (cb) queue_creature(cb, clen, pkg, id);
    }
    /* boot: the director wants the creature (the one queued above loads on the task's first
     * frame; the blob stands in only if none loads); at power on, the boot screen's word first */
    fd_init(&s_dir, NULL, esp_random());
    if (!s_booted) fd_boot(&s_dir);
    s_booted = true;
    s_cur = ST_BLOB; s_cur_fx = 0; s_word = 0; s_ntok = 0;
    s_tnow = s_acc = 0.f; s_melt_t = 1e9f;
    s_gx = s_gy = 0.f; s_touch_down = false;
    s_ask.scr.phase = FLUID_ASK_NONE; s_ask.scr.seq = s_ask_want.seq;     /* a question from before is gone */
    fluid_askview_destroy(s_ask.v); fluid_askview_destroy(s_ask.old); s_ask.v = s_ask.old = NULL;
    s_dask = 0; atomic_store(&s_glass_ask, 0);
    s_follow = false; s_follow_word = 0; s_text_hidden = false;
    s_rg = (fluid_ring_t){ 0 }; s_pg = (fluid_progress_t){ 0 }; s_dov = (fluid_overlay_t){ 0 };   /* the fluid's own ring is in the sim */
    s_sim->p.relax_up = RATE_UP[ST_BLOB]; s_sim->p.relax_down = RATE_DOWN[ST_BLOB];

    xSemaphoreTake(s_go, 0);                     /* fresh handshake: drawer idle, nothing queued */
    xSemaphoreTake(s_idle, 0);
    xSemaphoreGive(s_idle);
    s_run = true;
    /* drawer on core 0 (Wi-Fi's core, mostly idle), below lwip/httpd priority */
    if (xTaskCreatePinnedToCore(draw_task, "fluid_draw", 4096, NULL, 2, &s_dtask, 0) != pdPASS) {
        s_run = false; s_dtask = NULL;
        free_grid();
        return ESP_ERR_NO_MEM;
    }
    mole_diag_set_quiet(fluid_quiet);
    if (xTaskCreatePinnedToCore(fluid_task, "fluid_task", 6144, NULL, 2, &s_task, 1) != pdPASS) {
        s_run = false; s_task = NULL;
        xSemaphoreGive(s_go);                    /* wake the drawer so it sees s_run=false */
        if (xSemaphoreTake(s_wdone, pdMS_TO_TICKS(1000)) == pdTRUE) { s_dtask = NULL; free_grid(); }
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "fluid creature init: %dx%d cells (%d inside), PSRAM free %u",
             W, H, s_sim->inside, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

void mole_fluid_creature_deinit(void) {
    s_run = false;
    if (s_task) {
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            /* never free the sim under a live task: keep s_task set so the next
             * init waits for the exit before rebuilding */
            ESP_LOGE(TAG, "fluid task did not exit in 1 s — keeping its buffers");
            return;
        }
        s_task = NULL;
    }
    if (s_dtask) {
        xSemaphoreGive(s_go);                    /* wake the drawer so it sees s_run=false */
        if (xSemaphoreTake(s_wdone, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "fluid drawer did not exit in 1 s — keeping its buffers");
            return;
        }
        s_dtask = NULL;
    }
    atomic_store(&s_glass_ask, 0);                   /* nothing of the fluid on the panel now */
    scene_clear(&s_sp);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int q = 0; q < s_scene_nops; q++) scene_free(s_scene_ops[q].sc);
    s_scene_nops = s_scene_nq = 0;
    xSemaphoreGive(s_mutex);
    free_grid();
    cute_rt_destroy(s_cute); s_cute = NULL;
    cpk_destroy(s_cpk); s_cpk = NULL;
    ESP_LOGI(TAG, "fluid creature deinit");
}

esp_err_t mole_fluid_creature_render(void) {
    /* the task renders continuously; just ask it for a full repaint */
    s_full_flush = true;
    return ESP_OK;
}

static const char *state_name(int st, int fx, char *buf, size_t n) {
    switch (st) {
    case ST_THINKING: return "thinking";
    case ST_TEXT:     return "text";
    case ST_CREATURE: return "creature";
    case ST_SCENE:    return "scene";
    case ST_FX:       snprintf(buf, n, "effect:%s", FLUID_FX_NAMES[fx]); return buf;
    default:          return "blob";
    }
}

esp_err_t mole_fluid_creature_state_get_json(char **out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    fluid_cfg_t c = s_cfg;
    fluid_stats_t st = s_stats;
    int32_t ring = s_ring_dflt;
    if (!s_listen_dflt_loaded) { s_listen_dflt = listen_setting_load(); s_listen_dflt_loaded = true; }
    const uint32_t listen_ms = s_listen_dflt;
    if (!s_ack_dflt_loaded) { s_ack_dflt = ack_setting_load(); s_ack_dflt_loaded = true; }
    const int ack = s_ack_dflt;
    if (!s_filler_loaded) { s_filler_ms = filler_setting_load(); s_filler_loaded = true; }
    const uint32_t filler_ms = s_filler_ms;
    if (!s_shake_talk_loaded) { s_shake_talk = shake_talk_load(); s_shake_talk_loaded = true; }
    const bool shake_talk = s_shake_talk;
    if (!s_gestures_loaded) { s_gestures = gestures_load(); s_gestures_loaded = true; }
    const uint32_t gestures = s_gestures;
    if (!s_idle_life_loaded) { s_idle_life = idle_life_load(); s_idle_life_loaded = true; }
    const int idle_life = s_idle_life;
    char cname[40], cerr[40], cid[PKG_ID_MAX + 1];
    memcpy(cname, s_cute_name, sizeof(cname)); memcpy(cerr, s_cute_err, sizeof(cerr)); memcpy(cid, s_cr_id, sizeof(cid));
    cJSON *info = s_cr_info ? cJSON_Parse(s_cr_info) : NULL;   /* its gestures and moods (a package's) */
    char actn[25];
    strlcpy(actn, gesture_name(st.act), sizeof(actn));
    cJSON *root = cJSON_CreateObject();
    if (root) cJSON_AddStringToObject(root, "text", s_text ? s_text : "");
    xSemaphoreGive(s_mutex);
    if (!root) { cJSON_Delete(info); return ESP_ERR_NO_MEM; }

    char sb[24], grid[12];
    snprintf(grid, sizeof(grid), "%dx%d", c.grid_w, c.grid_h);
    cJSON_AddBoolToObject  (root, "ok",          true);
    cJSON_AddStringToObject(root, "creature",    "fluid");
    cJSON_AddStringToObject(root, "state",       state_name(st.state, st.fx, sb, sizeof(sb)));
    cJSON_AddBoolToObject  (root, "autoplay",    st.autoplay);
    cJSON_AddNumberToObject(root, "reel_step",   st.reel);
    cJSON_AddNumberToObject(root, "wpm",         c.wpm);
    cJSON_AddStringToObject(root, "mood",        st.mood >= 0 ? CUTE_MOOD_NAMES[st.mood] : "");
    cJSON_AddBoolToObject  (root, "asleep",      st.asleep);
    cJSON_AddBoolToObject  (root, "screen",      mole_fluid_screen_on());
    cJSON_AddBoolToObject(root, "working",       st.working);
    if (st.scene_pkg[0]) {                     /* the scene on screen: pkg, layout, s left */
        cJSON *sc = cJSON_AddObjectToObject(root, "scene");
        cJSON_AddStringToObject(sc, "pkg", st.scene_pkg);
        cJSON_AddStringToObject(sc, "layout", st.scene_layout);
        cJSON_AddNumberToObject(sc, "ttl_left", roundf(st.scene_left));
    }
    cJSON_AddStringToObject(root, "act",         actn);   /* the gesture playing */
    cJSON_AddStringToObject(root, "indicator",   FD_STATUS_NAMES[st.status]);
    {
        cJSON *cj = cJSON_AddObjectToObject(root, "creature_info");
        if (cj) {
            cJSON_AddStringToObject(cj, "name", cname);
            cJSON_AddStringToObject(cj, "package", cid);   /* "" = a .molty */
            cJSON_AddNumberToObject(cj, "tris", st.ctris);
            if (cerr[0]) cJSON_AddStringToObject(cj, "error", cerr);
            for (const char *const *k = (const char *const[]){ "gestures", "moods", NULL }; *k; k++) {
                cJSON *l = cJSON_DetachItemFromObjectCaseSensitive(info, *k);
                if (l) cJSON_AddItemToObject(cj, *k, l);
            }
        }
    }
    cJSON_Delete(info);
    cJSON_AddStringToObject(root, "palette",     FLUID_PAL_NAMES[c.palette]);
    {   /* the listening ring's default colour: the console's setting, else the accent */
        char rb[12];
        if (ring >= 0) snprintf(rb, sizeof(rb), "#%06x", (unsigned)ring & 0xFFFFFFu);
        cJSON_AddStringToObject(root, "ring_color", ring >= 0 ? rb : "accent");
    }
    cJSON_AddNumberToObject(root, "listen_ms", listen_ms);   /* 0 = unlimited */
    cJSON_AddStringToObject(root, "ack", link_ack_name((link_ack_t)ack));
    cJSON_AddNumberToObject(root, "filler_ms", filler_ms);   /* 0 = off */
    cJSON_AddBoolToObject(root, "shake_talk", shake_talk);
    cJSON_AddItemToObject(root, "gestures", gestures_json(gestures));
    cJSON_AddStringToObject(root, "idle_life", FD_IDLE_NAMES[idle_life]);
    cJSON_AddNumberToObject(root, "scene_dust", fluid_fine_dust_kind_get());       /* EXPERIMENT (mole_fluid_scene_dust) */
    cJSON_AddNumberToObject(root, "scene_hard_pct", fluid_fine_hard_density_get()); /* EXPERIMENT (mole_fluid_scene_hard) */
    cJSON_AddStringToObject(root, "edge_mode",   FLUID_EDGE_NAMES[c.edge_mode]);
    cJSON_AddStringToObject(root, "grid",        grid);
    cJSON_AddNumberToObject(root, "speed",       c.speed);
    cJSON_AddNumberToObject(root, "mic",         c.mic);
    cJSON_AddNumberToObject(root, "tilt",        c.tilt);
    cJSON_AddStringToObject(root, "word",        st.word);
    cJSON_AddNumberToObject(root, "word_idx",    st.word_idx);
    cJSON_AddNumberToObject(root, "nwords",      st.nwords);
    /* read-only timing (EMA, ms). Core 1: pre_ms (input, requests, director before the
     * sim), sim_ms, wait_ms (blocked on the drawer), frame_ms (sim + wait + snapshot,
     * excludes pacing). Core 0: draw_ms (which cells changed), flush_ms (the changed
     * windows drawn and sent to the panel). */
    cJSON_AddNumberToObject(root, "fps",         roundf(st.fps * 10.f) / 10.f);
    cJSON_AddNumberToObject(root, "frame_ms",    roundf(st.frame_ms * 100.f) / 100.f);
    cJSON_AddNumberToObject(root, "pre_ms",      roundf(st.pre_ms * 100.f) / 100.f);
    cJSON_AddNumberToObject(root, "sim_ms",      roundf(st.sim_ms * 100.f) / 100.f);
    cJSON_AddNumberToObject(root, "wait_ms",     roundf(st.wait_ms * 100.f) / 100.f);
    cJSON_AddNumberToObject(root, "draw_ms",     roundf(st.draw_ms * 100.f) / 100.f);
    cJSON_AddNumberToObject(root, "flush_ms",    roundf(st.flush_ms * 100.f) / 100.f);
    cJSON_AddNumberToObject(root, "cells_drawn", st.cells_drawn);
    cJSON_AddNumberToObject(root, "cells_inside", st.inside);
    cJSON_AddNumberToObject(root, "crisp_dots", st.crisp_dots);
    cJSON_AddNumberToObject(root, "band_share", st.share);
    cJSON_AddNumberToObject(root, "send_windows", roundf(st.send_n * 10.f) / 10.f);
    cJSON_AddNumberToObject(root, "send_kpx", roundf(st.send_kpx * 10.f) / 10.f);
    cJSON_AddNumberToObject(root, "blit_ms", roundf(st.blit_ms * 100.f) / 100.f);
    cJSON_AddNumberToObject(root, "blit_copy_ms", roundf(st.blit_part[0] * 100.f) / 100.f);
    cJSON_AddNumberToObject(root, "blit_wait_ms", roundf(st.blit_part[1] * 100.f) / 100.f);
    cJSON_AddNumberToObject(root, "blit_cmd_ms", roundf(st.blit_part[2] * 100.f) / 100.f);
    cJSON *prof = cJSON_AddObjectToObject(root, "prof_ms");
    for (int q = 0; prof && q < FP_N; q++)
        cJSON_AddNumberToObject(prof, FLUID_PROF_NAMES[q], roundf(st.prof_ms[q] * 100.f) / 100.f);
    cJSON_AddNumberToObject(root, "cpu_mhz",     s_cpu_mhz);
    {
        char mb[24]; snprintf(mb, sizeof(mb), "internal %d/%d", st.n_internal, st.n_blk);
        cJSON_AddStringToObject(root, "sim_mem", mb);
    }
    cJSON_AddNumberToObject(root, "energy",      roundf(st.energy * 1000.f) / 1000.f);
    cJSON_AddNumberToObject(root, "err",         roundf(st.err * 1000.f) / 1000.f);
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) return ESP_ERR_NO_MEM;
    *out = str;
    return ESP_OK;
}

static int lookup(const char *v, const char *const *names, int n) {
    for (int i = 0; i < n; i++) if (strcmp(v, names[i]) == 0) return i;
    return -1;
}

static bool parse_state(const char *v, int *st, int *fx) {
    *fx = 0;
    if (!strcmp(v, "blob"))     { *st = ST_BLOB; return true; }
    if (!strcmp(v, "thinking")) { *st = ST_THINKING; return true; }
    if (!strcmp(v, "text"))     { *st = ST_TEXT; return true; }
    if (!strcmp(v, "creature")) { *st = ST_CREATURE; return true; }   /* falls back to blob if none loaded */
    if (!strncmp(v, "effect:", 7)) { int f = lookup(v + 7, FLUID_FX_NAMES, FLUID_FX_COUNT); if (f >= 0) { *st = ST_FX; *fx = f; return true; } }
    return false;
}

static void set_text_locked(const char *text, int len) {
    if (len > FLUID_TEXT_MAX) len = FLUID_TEXT_MAX;
    s_tint_in = 0;                                     /* new words: the palette's colours */
    if (len == s_text_len && memcmp(s_text, text, (size_t)len) == 0) return;   /* duplicate straggler */
    memcpy(s_text, text, (size_t)len);
    s_text[len] = '\0';
    s_text_len = len;
    s_text_ver++;
    s_text_appended = false;
}

esp_err_t mole_fluid_creature_state_set_json(const char *body) {
    if (!body) return ESP_ERR_INVALID_ARG;
    if (alloc_bufs() != ESP_OK) return ESP_ERR_NO_MEM;
    cJSON *root = cJSON_Parse(body);
    if (!root) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = ESP_OK;
    cJSON *j;
    /* ring_color: "#rrggbb" or "accent" — the device default, kept in NVS */
    int32_t ring = -2;                                 /* -2: not given */
    if ((j = cJSON_GetObjectItem(root, "ring_color"))) {
        uint32_t v;
        if (cJSON_IsString(j) && !strcmp(j->valuestring, "accent")) ring = -1;
        else if (cJSON_IsString(j) && link_color_parse(j->valuestring, &v)) ring = (int32_t)v;
        else ret = ESP_ERR_INVALID_ARG;
    }
    /* listen_ms: 0 (unlimited) or 5000..30000 — the device default, kept in NVS */
    int32_t listen = -1;                               /* -1: not given */
    if ((j = cJSON_GetObjectItem(root, "listen_ms"))) {
        if (cJSON_IsNumber(j) && link_listen_setting_ok(j->valuedouble)) listen = (int32_t)j->valuedouble;
        else ret = ESP_ERR_INVALID_ARG;
    }
    /* ack: "chirp" | "thinking" | "off" — the turn acknowledgement, kept in NVS */
    int ack = -1;                                      /* -1: not given */
    if ((j = cJSON_GetObjectItem(root, "ack"))) {
        link_ack_t m;
        if (cJSON_IsString(j) && link_ack_parse(j->valuestring, &m)) ack = (int)m;
        else ret = ESP_ERR_INVALID_ARG;
    }
    /* filler_ms: 0 (off) or 400..5000 — the auto filler's delay, kept in NVS */
    int32_t filler = -1;                               /* -1: not given */
    if ((j = cJSON_GetObjectItem(root, "filler_ms"))) {
        if (cJSON_IsNumber(j) && link_filler_setting_ok(j->valuedouble)) filler = (int32_t)j->valuedouble;
        else ret = ESP_ERR_INVALID_ARG;
    }
    /* shake_talk: true | false — a shake starts a listen by itself, kept in NVS */
    int shake = -1;                                    /* -1: not given */
    if ((j = cJSON_GetObjectItem(root, "shake_talk"))) {
        if (cJSON_IsBool(j)) shake = cJSON_IsTrue(j) ? 1 : 0;
        else ret = ESP_ERR_INVALID_ARG;
    }
    /* gestures: {tap|double_tap|button_accept|button_reject: none|talk|event|stop}, kept in NVS */
    if ((j = cJSON_GetObjectItem(root, "gestures")) && mole_fluid_gestures_apply(j) != ESP_OK) ret = ESP_ERR_INVALID_ARG;
    /* idle_life: "lively" | "calm" | "off" — how much the creature moves by itself, kept in NVS */
    int idle = -1;                                     /* -1: not given */
    if ((j = cJSON_GetObjectItem(root, "idle_life"))) {
        idle = cJSON_IsString(j) ? lookup(j->valuestring, FD_IDLE_NAMES, FD_IDLE_COUNT) : -1;
        if (idle < 0) ret = ESP_ERR_INVALID_ARG;
    }
    if (idle >= 0) {
        nvs_handle_t h;
        esp_err_t ne = nvs_open("fluid", NVS_READWRITE, &h);
        if (ne == ESP_OK) {
            ne = nvs_set_i32(h, "idle_life", idle);
            if (ne == ESP_OK) ne = nvs_commit(h);
            nvs_close(h);
        }
        if (ne != ESP_OK) { ESP_LOGW(TAG, "idle_life not saved (%s)", esp_err_to_name(ne)); ret = ne; }
    }
    /* EXPERIMENT scene_dust: 0 | 1 | 2 | 3, scene_hard_pct: 0..25 (mole_fluid_scene_dust, _scene_hard; not kept) */
    if ((j = cJSON_GetObjectItem(root, "scene_dust"))) {
        if (cJSON_IsNumber(j) && j->valueint >= 0 && j->valueint <= 3) mole_fluid_scene_dust(j->valueint);
        else ret = ESP_ERR_INVALID_ARG;
    }
    if ((j = cJSON_GetObjectItem(root, "scene_hard_pct"))) {
        if (cJSON_IsNumber(j) && j->valueint >= 0 && j->valueint <= FINE_HARD_MAX) mole_fluid_scene_hard(j->valueint);
        else ret = ESP_ERR_INVALID_ARG;
    }
    /* act: a one-shot action (cute_act.h) on the creature, as the link's `act` */
    int act = -2;                                      /* -2: not given */
    if ((j = cJSON_GetObjectItem(root, "act"))) {
        act = cJSON_IsString(j) ? gesture_of(j->valuestring) : -1;
        if (act < 0) ret = ESP_ERR_INVALID_ARG;
    }
    if (shake >= 0) {
        nvs_handle_t h;
        esp_err_t ne = nvs_open("fluid", NVS_READWRITE, &h);
        if (ne == ESP_OK) {
            ne = nvs_set_i32(h, "shake_talk", shake);
            if (ne == ESP_OK) ne = nvs_commit(h);
            nvs_close(h);
        }
        if (ne != ESP_OK) { ESP_LOGW(TAG, "shake_talk not saved (%s)", esp_err_to_name(ne)); ret = ne; }
    }
    if (filler >= 0) {
        nvs_handle_t h;
        esp_err_t ne = nvs_open("fluid", NVS_READWRITE, &h);
        if (ne == ESP_OK) {
            ne = nvs_set_i32(h, "filler", filler);
            if (ne == ESP_OK) ne = nvs_commit(h);
            nvs_close(h);
        }
        if (ne != ESP_OK) { ESP_LOGW(TAG, "filler_ms not saved (%s)", esp_err_to_name(ne)); ret = ne; }
    }
    if (ack >= 0) {
        nvs_handle_t h;
        esp_err_t ne = nvs_open("fluid", NVS_READWRITE, &h);
        if (ne == ESP_OK) {
            ne = nvs_set_i32(h, "ack", ack);
            if (ne == ESP_OK) ne = nvs_commit(h);
            nvs_close(h);
        }
        if (ne != ESP_OK) { ESP_LOGW(TAG, "ack not saved (%s)", esp_err_to_name(ne)); ret = ne; }
    }
    if (listen >= 0) {
        nvs_handle_t h;
        esp_err_t ne = nvs_open("fluid", NVS_READWRITE, &h);
        if (ne == ESP_OK) {
            ne = nvs_set_i32(h, "listen", listen);
            if (ne == ESP_OK) ne = nvs_commit(h);
            nvs_close(h);
        }
        if (ne != ESP_OK) { ESP_LOGW(TAG, "listen_ms not saved (%s)", esp_err_to_name(ne)); ret = ne; }
    }
    if (ring != -2) {
        nvs_handle_t h;
        esp_err_t ne = nvs_open("fluid", NVS_READWRITE, &h);
        if (ne == ESP_OK) {
            ne = ring < 0 ? nvs_erase_key(h, "ring") : nvs_set_i32(h, "ring", ring);
            if (ne == ESP_ERR_NVS_NOT_FOUND) ne = ESP_OK;  /* already the accent */
            if (ne == ESP_OK) ne = nvs_commit(h);
            nvs_close(h);
        }
        if (ne != ESP_OK) { ESP_LOGW(TAG, "ring_color not saved (%s)", esp_err_to_name(ne)); ret = ne; }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if ((j = cJSON_GetObjectItem(root, "text")) && cJSON_IsString(j))
        set_text_locked(j->valuestring, (int)strlen(j->valuestring));
    if ((j = cJSON_GetObjectItem(root, "wpm")) && cJSON_IsNumber(j)) {
        int v = (int)j->valuedouble; s_cfg.wpm = v < 100 ? 100 : v > 800 ? 800 : v;
    }
    if ((j = cJSON_GetObjectItem(root, "palette")) && cJSON_IsString(j)) {
        int v = lookup(j->valuestring, FLUID_PAL_NAMES, FLUID_PAL_COUNT);
        if (v >= 0) s_cfg.palette = v; else ret = ESP_ERR_INVALID_ARG;
    }
    if ((j = cJSON_GetObjectItem(root, "edge_mode")) && cJSON_IsString(j)) {
        int v = lookup(j->valuestring, FLUID_EDGE_NAMES, FLUID_EDGE_COUNT);
        if (v >= 0) s_cfg.edge_mode = v; else ret = ESP_ERR_INVALID_ARG;
    }
    if ((j = cJSON_GetObjectItem(root, "grid")) && cJSON_IsString(j)) {
        int w = 0, h = 0;
        if (sscanf(j->valuestring, "%dx%d", &w, &h) == 2 && w >= FLUID_GRID_MIN_W && w <= FLUID_GRID_MAX_W
            && h >= FLUID_GRID_MIN_H && h <= FLUID_GRID_MAX_H) { s_cfg.grid_w = w; s_cfg.grid_h = h; }
        else ret = ESP_ERR_INVALID_ARG;
    }
    if ((j = cJSON_GetObjectItem(root, "speed")) && cJSON_IsNumber(j))
        s_cfg.speed = fminf(2.5f, fmaxf(0.4f, (float)j->valuedouble));
    if ((j = cJSON_GetObjectItem(root, "mic")) && cJSON_IsNumber(j))
        s_cfg.mic = fminf(1.f, fmaxf(0.f, (float)j->valuedouble));
    if ((j = cJSON_GetObjectItem(root, "tilt")) && cJSON_IsNumber(j))
        s_cfg.tilt = fminf(3.f, fmaxf(-3.f, (float)j->valuedouble));
    if ((j = cJSON_GetObjectItem(root, "ripple")) && cJSON_IsObject(j) && s_ripn < FLUID_RIPPLE_Q) {
        cJSON *x = cJSON_GetObjectItem(j, "x"), *y = cJSON_GetObjectItem(j, "y"), *k = cJSON_GetObjectItem(j, "strength");
        s_ripq[s_ripn][0] = cJSON_IsNumber(x) ? fminf(1.f, fmaxf(-1.f, (float)x->valuedouble)) : 0.f;
        s_ripq[s_ripn][1] = cJSON_IsNumber(y) ? fminf(1.f, fmaxf(-1.f, (float)y->valuedouble)) : 0.f;
        s_ripq[s_ripn][2] = cJSON_IsNumber(k) ? fminf(3.f, fmaxf(0.f, (float)k->valuedouble)) : 1.6f;
        s_ripn++;
    }
    bool state_given = (j = cJSON_GetObjectItem(root, "state")) && cJSON_IsString(j);
    float hold_s = -1.f;                               /* mood's default (fluid_director.c) */
    if ((j = cJSON_GetObjectItem(root, "hold_s"))) {
        if (cJSON_IsNumber(j) && j->valuedouble >= 0 && j->valuedouble <= 3600) hold_s = (float)j->valuedouble;
        else ret = ESP_ERR_INVALID_ARG;
    }
    if ((j = cJSON_GetObjectItem(root, "mood")) && cJSON_IsString(j)) {
        int m = -1;
        for (int k = 0; k < CUTE_N_MOODS; k++) if (!strcmp(j->valuestring, CUTE_MOOD_NAMES[k])) m = k;
        const bool work = !strcmp(j->valuestring, "working");
        if (m < 0 && !work) ret = ESP_ERR_INVALID_ARG;
        else {
            s_req_mood = work ? CM_IDLE : m; s_req_work = work; s_req_hold_s = hold_s;
            /* a mood alone means "show me the creature in that mood" */
            if (!state_given) { s_req_state = ST_CREATURE; s_req_fx = 0; s_req_implied = true; }
        }
    }
    if ((j = cJSON_GetObjectItem(root, "state")) && cJSON_IsString(j)) {
        int st, fx;
        if (!strcmp(j->valuestring, "autoplay")) s_req_reel = true;     /* the demo reel, on request only */
        else if (parse_state(j->valuestring, &st, &fx)) {
            s_req_state = st; s_req_fx = fx; s_req_implied = false;
            s_req_text_hold = st == ST_TEXT && cJSON_IsTrue(cJSON_GetObjectItem(root, "hold"));
        }
        else ret = ESP_ERR_INVALID_ARG;
    }
    if (ring != -2) { s_ring_dflt = ring; s_ring_dflt_loaded = true; }   /* live even if NVS failed */
    if (listen >= 0) { s_listen_dflt = (uint32_t)listen; s_listen_dflt_loaded = true; }
    if (ack >= 0) { s_ack_dflt = ack; s_ack_dflt_loaded = true; }
    if (filler >= 0) { s_filler_ms = (uint32_t)filler; s_filler_loaded = true; }
    if (shake >= 0) { s_shake_talk = shake == 1; s_shake_talk_loaded = true; }
    if (idle >= 0) { s_idle_life = idle; s_idle_life_loaded = true; }
    if (act >= 0) s_req_act = act;
    s_req_activity = true;                             /* someone is driving: no dozing off */
    xSemaphoreGive(s_mutex);
    cJSON_Delete(root);
    return ret;
}

esp_err_t mole_fluid_append_text(const char *chunk, int len) {
    if (!chunk || len < 0) return ESP_ERR_INVALID_ARG;
    esp_err_t e = alloc_bufs();
    if (e != ESP_OK) return e;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool sep = s_text_len > 0 && !is_ws(s_text[s_text_len - 1]) && len > 0 && !is_ws(chunk[0]);
    int room = FLUID_TEXT_MAX - s_text_len - (sep ? 1 : 0);
    if (len > room) len = room > 0 ? room : 0;
    if (len > 0) {
        if (sep) s_text[s_text_len++] = ' ';
        memcpy(s_text + s_text_len, chunk, (size_t)len);
        s_text_len += len;
        s_text[s_text_len] = '\0';
        if (s_seen_ver == s_text_ver) s_text_appended = true;   /* pure append since the task's last look */
        s_text_ver++;
    }
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t mole_fluid_set_text(const char *text, int len) {
    if (!text || len < 0) return ESP_ERR_INVALID_ARG;
    esp_err_t e = alloc_bufs();
    if (e != ESP_OK) return e;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    set_text_locked(text, len);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t mole_fluid_scene(mole_scene_op_t op, scene_t *sc) {
    if (!s_mutex || !s_run) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* the queue as it will be once the pending ops ran: play / clear empty it */
    int nq = s_scene_nq;
    for (int q = 0; q < s_scene_nops; q++) nq = s_scene_ops[q].op == MOLE_SCENE_QUEUE ? nq + 1 : 0;
    const bool full = s_scene_nops >= (int)(sizeof(s_scene_ops) / sizeof(s_scene_ops[0])) || (op == MOLE_SCENE_QUEUE && nq >= SCENE_QUEUE_MAX);
    if (!full) s_scene_ops[s_scene_nops++] = (scene_op_t){ op, sc };
    xSemaphoreGive(s_mutex);
    return full ? ESP_ERR_INVALID_STATE : ESP_OK;
}

void mole_fluid_event(mole_fluid_event_t ev) {
    if (!s_mutex) return;                              /* fluid not up yet */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (ev == MOLE_FLUID_SHAKE) s_req_shake = true; else s_req_user = true;
    xSemaphoreGive(s_mutex);
    atomic_store(&s_screen_want, true);
}

void mole_fluid_screen(bool on) { if (!atomic_load(&s_bye)) atomic_store(&s_screen_want, on); }

void mole_fluid_goodbye(void) {
    atomic_store(&s_bye, true);
    atomic_store(&s_screen_want, true);
    if (!s_mutex) atomic_store(&s_bye_done, true);     /* fluid not up: nothing to show */
}
bool mole_fluid_goodbye_done(void) { return atomic_load(&s_bye_done); }

void mole_fluid_progress(float pct, bool done) {
    if (done) { atomic_store(&s_prog_end, true); return; }
    atomic_store(&s_prog_in, pct < 0.f ? -1 : (int32_t)(fminf(pct, 100.f) * 10.f + 0.5f));
    atomic_fetch_add(&s_prog_seq, 1);
}
void mole_fluid_progress_stop(void) { atomic_store(&s_prog_stop, true); }
bool mole_fluid_screen_on(void) { return atomic_load(&s_screen); }
/* the screen quiet for the log's flash writes (mole_diag_set_quiet): off, or the panel getting little (a creature at
 * rest: ~10 k px a frame; walking ~50 k, a ring forming 100 k+) */
#define FLUID_QUIET_KPX 20.f
static bool fluid_quiet(void) {
    if (!atomic_load(&s_screen)) return true;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const bool q = s_stats.send_kpx < FLUID_QUIET_KPX;
    xSemaphoreGive(s_mutex);
    return q;
}

void mole_fluid_set_status(int status) {
    if (!s_mutex || status == s_status_in) return;    /* unlocked peek: only this caller writes it */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status_in = status;
    xSemaphoreGive(s_mutex);
}

void mole_fluid_listen(bool on) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_listen_in = on;
    if (!on) s_voice_in = 0.f;
    xSemaphoreGive(s_mutex);
}

void mole_fluid_listen_level(float level) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_voice_in = level < 0.f ? 0.f : level > 1.f ? 1.f : level;
    xSemaphoreGive(s_mutex);
}

void mole_fluid_text_cue(int word) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (word < 0) { s_follow_in = true; s_follow_word_in = -1; }   /* following; no word reached yet */
    else if (word > s_follow_word_in) s_follow_word_in = word;
    xSemaphoreGive(s_mutex);
}

void mole_fluid_text_cue_end(void) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_follow_in = false;
    xSemaphoreGive(s_mutex);
}

void mole_fluid_act(int act) {
    if (!s_mutex || act < 0) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (act < CUTE_N_ACTS + s_nown) s_req_act = act;
    xSemaphoreGive(s_mutex);
}

int mole_fluid_gesture(const char *name, float *dur) {
    if (!s_mutex) return -1;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const int a = gesture_of(name);
    if (dur) *dur = a >= CUTE_N_ACTS ? s_own[a - CUTE_N_ACTS].dur : cute_act_dur(a);
    xSemaphoreGive(s_mutex);
    return a;
}

/* a say's mood mark → its colour for the words (the relay's COLORS, commands.ts) */
static uint32_t mood_tint(int mood) {
    switch (mood) {
    case CM_HAPPY:     return FLUID_RGB | 0xffa94du;   /* amber */
    case CM_SURPRISED: return FLUID_RGB | 0x3bc9dbu;   /* cyan */
    case CM_THINKING:  return FLUID_RGB | 0xb197fcu;   /* violet */
    case CM_SLEEPY:    return FLUID_RGB | 0x4dabf7u;   /* blue */
    case CM_LISTENING: return FLUID_RGB | 0x3ccf96u;   /* green */
    default:           return 0;                       /* idle, speaking: the palette's */
    }
}

unsigned mole_fluid_text_ver(void) {
    if (!s_mutex) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const unsigned v = (unsigned)s_text_ver;
    xSemaphoreGive(s_mutex);
    return v;
}

void mole_fluid_cue_mood(int mood, unsigned text_ver) {
    if (!s_mutex || mood < 0 || mood >= CUTE_N_MOODS) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* only the say's own words: an update / show / pairing text set since then owns the
     * reader (checked and set under one lock, so no ownership race) */
    if ((unsigned)s_text_ver == text_ver) s_tint_in = mood_tint(mood);
    xSemaphoreGive(s_mutex);
}

void mole_fluid_shed_internal(void) {
    fluid_sim_allow_internal(false);                   /* any sim built from now on, too */
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_shed = s_req_regrid = true;
    xSemaphoreGive(s_mutex);
}

/* EXPERIMENT scene dust (mole_fluid.h; delete once Juan picks): the drawer reads them each frame */
void mole_fluid_scene_dust(int kind) { fluid_fine_dust_kind(kind); }
void mole_fluid_scene_hard(int percent) { fluid_fine_hard_density(percent); }

unsigned mole_fluid_spot_tap(int px, int py) {
    if (!s_mutex || !s_spot_sc || s_stage <= 0) return 0;
    const float x = (float)(px - s_ox) / (float)s_stage * 2.f - 1.f, y = 1.f - (float)(py - s_oy) / (float)s_stage * 2.f;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* (while the picture changes into the scene its spots are not on the glass yet: none is hit) */
    const int n = s_spot_on && !fluid_draw_morphing(s_draw) ? scene_spot_at(s_spot_sc, s_spot_t, x, y) : -1;
    const unsigned spot = n >= 0 && n <= 254 ? s_spot_gen << 8 | (unsigned)(n + 1) : s_spot_back && s_spot_gen ? MOLE_SPOT_BACK : 0;
    if (spot && spot != MOLE_SPOT_BACK) {
        const scene_item_t *it = &s_spot_sc->items[n];
        s_tapped = spot;
        snprintf(s_tapped_id, sizeof(s_tapped_id), "%.*s", (int)it->nsid, s_spot_sc->say + it->sid);
        snprintf(s_tapped_open, sizeof(s_tapped_open), "%.*s", (int)it->nopen, s_spot_sc->say + it->open);
    }
    xSemaphoreGive(s_mutex);
    if (spot) { atomic_store(&s_tap_at, (uint32_t)px << 16 | ((uint32_t)py & 0xFFFFu)); atomic_store(&s_spot_req, spot); }
    return spot;
}

void mole_fluid_spot_id(unsigned spot, char *id, size_t size, char *open, size_t open_size) {
    if (!size) return;
    id[0] = '\0';
    if (open && open_size) open[0] = '\0';
    if (!s_mutex || !s_spot_sc || !spot || spot == MOLE_SPOT_BACK) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (spot == s_tapped) {
        snprintf(id, size, "%s", s_tapped_id);
        if (open && open_size) snprintf(open, open_size, "%s", s_tapped_open);
    }
    xSemaphoreGive(s_mutex);
}

bool mole_fluid_scene_event(mole_scene_evt_t *out) {
    if (!s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const bool on = s_scene_evt_on;
    if (on) { *out = s_scene_evt; s_scene_evt_on = false; }
    xSemaphoreGive(s_mutex);
    return on;
}

void mole_fluid_allow_internal(void) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    static bool done;                                  /* once a boot; never after a shed */
    if (!done && !s_shed) { done = s_req_regrid = true; fluid_sim_allow_internal(true); }
    xSemaphoreGive(s_mutex);
}

int mole_fluid_internal_fields(void) {
    if (!s_mutex) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);            /* s_stats: the task's copy, never s_sim itself */
    const int n = s_stats.n_internal;
    xSemaphoreGive(s_mutex);
    return n;
}

void mole_fluid_spoke(void) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_req_spoke = true;
    xSemaphoreGive(s_mutex);
}

static int idle_life_load(void) {
    nvs_handle_t h;
    int32_t v = -1;
    if (nvs_open("fluid", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, "idle_life", &v) != ESP_OK) v = -1;
        nvs_close(h);
    }
    return v >= 0 && v < FD_IDLE_COUNT ? (int)v : FD_IDLE_DEFAULT;
}

void mole_fluid_listen_color(int32_t rgb) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_ring_in = rgb < 0 ? -1 : (int32_t)(rgb & 0xFFFFFF);
    xSemaphoreGive(s_mutex);
}

static uint32_t listen_setting_load(void) {
    nvs_handle_t h;
    int32_t v = -1;
    if (nvs_open("fluid", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, "listen", &v) != ESP_OK) v = -1;
        nvs_close(h);
    }
    return v >= 0 && link_listen_setting_ok(v) ? (uint32_t)v : LINK_LISTEN_DEFAULT_MS;
}

uint32_t mole_fluid_listen_window(void) {
    if (!s_mutex) return listen_setting_load();          /* before init: straight from NVS */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_listen_dflt_loaded) { s_listen_dflt = listen_setting_load(); s_listen_dflt_loaded = true; }
    const uint32_t w = s_listen_dflt;
    xSemaphoreGive(s_mutex);
    return w;
}

static int ack_setting_load(void) {
    nvs_handle_t h;
    int32_t v = -1;
    if (nvs_open("fluid", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, "ack", &v) != ESP_OK) v = -1;
        nvs_close(h);
    }
    return v >= LINK_ACK_OFF && v <= LINK_ACK_CHIRP ? (int)v : LINK_ACK_DEFAULT;
}

static uint32_t filler_setting_load(void) {
    nvs_handle_t h;
    int32_t v = -1;
    if (nvs_open("fluid", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, "filler", &v) != ESP_OK) v = -1;
        nvs_close(h);
    }
    return v >= 0 && link_filler_setting_ok(v) ? (uint32_t)v : LINK_FILLER_DEFAULT_MS;
}

uint32_t mole_fluid_filler_ms(void) {
    if (!s_mutex) return filler_setting_load();          /* before init: straight from NVS */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_filler_loaded) { s_filler_ms = filler_setting_load(); s_filler_loaded = true; }
    const uint32_t ms = s_filler_ms;
    xSemaphoreGive(s_mutex);
    return ms;
}

static bool shake_talk_load(void) {
    nvs_handle_t h;
    int32_t v = 1;
    if (nvs_open("fluid", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, "shake_talk", &v) != ESP_OK) v = 1;
        nvs_close(h);
    }
    return v != 0;
}

static uint32_t gestures_load(void) {
    nvs_handle_t h;
    int32_t v = -1;
    if (nvs_open("fluid", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, "gestures", &v) != ESP_OK) v = -1;
        nvs_close(h);
    }
    return v >= 0 && gesture_binds_valid((uint32_t)v) ? (uint32_t)v : gesture_binds_default();
}

uint32_t mole_fluid_gestures(void) {
    if (!s_mutex) return gestures_load();                /* before init: straight from NVS */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_gestures_loaded) { s_gestures = gestures_load(); s_gestures_loaded = true; }
    const uint32_t v = s_gestures;
    xSemaphoreGive(s_mutex);
    return v;
}

cJSON *mole_fluid_gestures_json(void) { return gestures_json(mole_fluid_gestures()); }

esp_err_t mole_fluid_gestures_apply(const cJSON *bindings) {
    if (!cJSON_IsObject(bindings)) return ESP_ERR_INVALID_ARG;
    uint32_t next = mole_fluid_gestures();
    const cJSON *it;
    cJSON_ArrayForEach(it, bindings) {
        const int in = gesture_input_lookup(it->string);
        const int act = cJSON_IsString(it) ? gesture_action_lookup(it->valuestring) : -1;
        if (in < 0 || act < 0) return ESP_ERR_INVALID_ARG;   /* all or nothing */
        next = gesture_binds_set(next, (gesture_input_t)in, (gesture_action_t)act);
    }
    nvs_handle_t h;
    esp_err_t ne = nvs_open("fluid", NVS_READWRITE, &h);
    if (ne == ESP_OK) {
        ne = nvs_set_i32(h, "gestures", (int32_t)next);
        if (ne == ESP_OK) ne = nvs_commit(h);
        nvs_close(h);
    }
    if (ne != ESP_OK) { ESP_LOGW(TAG, "gestures not saved (%s)", esp_err_to_name(ne)); return ne; }
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_gestures = next; s_gestures_loaded = true;
    if (s_mutex) xSemaphoreGive(s_mutex);
    return ESP_OK;
}

bool mole_fluid_shake_talk(void) {
    if (!s_mutex) return shake_talk_load();              /* before init: straight from NVS */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_shake_talk_loaded) { s_shake_talk = shake_talk_load(); s_shake_talk_loaded = true; }
    const bool on = s_shake_talk;
    xSemaphoreGive(s_mutex);
    return on;
}

int mole_fluid_idle_life(void) {
    if (!s_mutex) return idle_life_load();               /* before init: straight from NVS */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_idle_life_loaded) { s_idle_life = idle_life_load(); s_idle_life_loaded = true; }
    const int v = s_idle_life;
    xSemaphoreGive(s_mutex);
    return v;
}

int mole_fluid_ack_mode(void) {
    if (!s_mutex) return ack_setting_load();             /* before init: straight from NVS */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_ack_dflt_loaded) { s_ack_dflt = ack_setting_load(); s_ack_dflt_loaded = true; }
    const int m = s_ack_dflt;
    xSemaphoreGive(s_mutex);
    return m;
}

esp_err_t mole_fluid_ask_show(const fluid_ask_q_t *q, unsigned *seq) {
    if (!q || q->n < 1 || q->n > FLUID_ASK_MAX || !seq) return ESP_ERR_INVALID_ARG;
    esp_err_t e = alloc_bufs();
    if (e != ESP_OK) return e;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (++s_ask_want.seq == 0) s_ask_want.seq = 1;     /* 0 = no question (mole_fluid_ask_shown) */
    s_ask_want.active = true; s_ask_want.pick = -1; s_ask_want.q = *q;
    s_req_activity = true;                             /* an agent is here: no dozing off */
    *seq = s_ask_want.seq;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

unsigned mole_fluid_ask_shown(void) { return atomic_load(&s_glass_ask); }

void mole_fluid_ask_pick(int index) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_ask_want.active && index >= 0 && index < s_ask_want.q.n) s_ask_want.pick = index;
    xSemaphoreGive(s_mutex);
}

void mole_fluid_ask_end(void) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_ask_want.active = false;
    xSemaphoreGive(s_mutex);
}
