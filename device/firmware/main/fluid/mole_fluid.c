/*
 * mole_fluid.c — "fluid" creature: Molty as a fluid of characters.
 *
 * Port of device/creatures/js/fluid/main.js (wiring) on top of fluid_sim/target/draw.c; what
 * is shown when (states, moods, idle life, the demo reel) is fluid_director.c. Over it:
 * a question (the link's ask: labels as text targets in their sectors, fluid_ask.h, the
 * director's transitions waiting underneath), rings of the fluid itself (the listening
 * ring while a listen records, fluid_target_ring / fluid_ring.h; a question's coloured
 * button borders, fluid_target_arcs) and
 * one pixel overlay (fluid_draw_overlay: a question's faint dividers). Threading:
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
#include "cute_rig.h"
#include "cute_act.h"
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

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "mole_diag.h"
#include "esp_private/esp_clk.h"
#include "cJSON.h"
#include "nvs.h"
#include "link_protocol.h"   /* link_color_parse */

#include "glyph_font.h"   /* gc_font5x7, GC_RAMP_ASCII */
#include "fluid_ring.h"   /* the listening ring's knobs */

static const char *TAG = "mole_fluid";

/* ── Constants ──────────────────────────────────────────────────────────────── */

#define FLUID_TEXT_MAX     4096
#define FLUID_MAX_TOKENS   512
#define FLUID_STEP         (1.f / 60.f)
#define FLUID_FRAME_TICKS  3          /* 100 Hz tick → 30 ms → 33.3 fps cap     */
#define FLUID_SLEEP_TICKS  5          /* asleep (slow breathing): 20 fps, less CPU */
#define FLUID_TOUCH_US     16000      /* touch poll ≤ 60 Hz (shared I2C bus)    */
#define FLUID_TILT_ACCEL   25.f       /* cells/s² of flow bias per g of tilt    */
#define FLUID_TILT_LEAN    0.12f      /* stage units a target leans per g       */
#define FLUID_IMU_SX       (1.f)      /* device-frame → screen: dashboard maps  */
#define FLUID_IMU_SY       (-1.f)     /* dot_dx = +ax, dot_dy = -ay             */
#define FLUID_RIPPLE_Q     4
#define DIV_FADE_S         0.4f       /* a question's dividers fade in / out       */

static const char *DEFAULT_TEXT =
    "hello, i am molty. i breathe in ascii and i speak in ascii. touch the water.";

/* ── States (the director decides which, fluid_director.h) ─────────────────── */

enum { ST_BLOB = FD_ST_BLOB, ST_THINKING = FD_ST_THINKING, ST_TEXT = FD_ST_TEXT,
       ST_FX = FD_ST_FX, ST_CREATURE = FD_ST_CREATURE };

/* relax rates per kind of target (text must condense within one word's time) */
static const float RATE_UP[]   = { 1.6f, 1.6f, 7.f, 1.8f, 2.6f };
static const float RATE_DOWN[] = { 2.0f, 2.0f, 2.4f, 2.2f, 3.4f };

/* ── Shared config / requests (httpd ↔ task, under s_mutex) ─────────────────── */

typedef struct {
    int   palette, edge_mode, grid_w, grid_h, wpm;
    float speed, mic, tilt;
} fluid_cfg_t;

typedef struct {
    float fps, frame_ms, sim_ms, wait_ms, draw_ms, flush_ms, energy, err;
    float prof_ms[FP_N];                       /* per-phase EMA, ms per frame */
    int   mood, ctris, status, act;
    bool  internal, autoplay, asleep;
    int   n_internal, n_blk;
    int   state, fx, reel, word_idx, nwords, inside, grid_w, grid_h, cells_drawn;
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
static bool    s_req_text_hold = false;         /* …"text" holding its last word    */
static bool    s_req_reel  = false;             /* pending autoplay restart         */
static bool    s_req_activity = false;          /* a state / mood / text change came */
static bool    s_req_shake = false, s_req_user = false;   /* sense.c: shake, button  */
static int     s_status_in = FD_STATUS_OK;      /* link status (mole_fluid_set_status) */
/* the question the link wants on screen (mole_fluid_ask_*): seq = which one; pick = the
 * answer once it came (the task holds it, then leaves by itself) */
typedef struct {
    unsigned seq; bool active; int n, pick;
    char label[FLUID_ASK_MAX][FLUID_ASK_LABEL_MAX + 1];
    uint32_t rgb[FLUID_ASK_MAX];                /* each option's colour, 0xRRGGBB */
} ask_want_t;
static ask_want_t s_ask_want;
static bool    s_listen_in = false;             /* link_audio: a listen is recording   */
static bool    s_follow_in = false;             /* link_audio: the reader follows a say's voice */
static int     s_follow_word_in = 0;            /* ...the last word the speaker reached  */
static float   s_level_in = 0.f;                /* ...its mic level 0..1               */
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
/* idle life (NVS "fluid"/"idle_life", the console's setting; FD_IDLE_*, lively by default) */
static int      s_idle_life = FD_IDLE_DEFAULT;
static bool     s_idle_life_loaded = false;
static int      idle_life_load(void);
static int      s_req_act = -1;                 /* an agent's action (act / a say's mark)  */
static bool     s_req_spoke = false;            /* a say played to its end                 */
static bool     s_req_shed = false;             /* the heap guard: hot fields → PSRAM (mole_fluid_shed_internal) */
/* a say's mood marks tint its words from the cued word on (mole_fluid_cue_mood):
 * 0 = the palette's colours, else FLUID_RGB | 0xRRGGBB */
static uint32_t s_tint_in = 0;
static float   s_ripq[FLUID_RIPPLE_Q][3];
static int     s_ripn = 0;

/* creature (.molty) — the blob is handed over under s_mutex, built by the task */
static uint8_t *s_cute_blob = NULL;  static size_t s_cute_len = 0;   /* pending load (PSRAM) */
static int      s_req_mood = -1;
static float    s_req_hold_s = -1.f;                /* with s_req_mood: < 0 = its default */
static char     s_cute_name[40] = "", s_cute_err[40] = "";
static cute_rt_t *s_cute = NULL;                    /* task-owned                        */
static int      s_cute_tris = 0;

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

/* the question on screen: labels as text targets over whatever the director shows (its
 * transitions wait until the question leaves) */
static struct {
    fluid_ask_screen_t scr;                     /* phase, pick, seq (the ask_want_t it
                                                 * follows), t, pick_t (fluid_ask.h)      */
    int n;
    float fs;                                   /* type size                             */
    char label[FLUID_ASK_MAX][FLUID_ASK_LABEL_MAX + 1];
    int len[FLUID_ASK_MAX];
    float cx[FLUID_ASK_MAX], cy[FLUID_ASK_MAX]; /* label centres, stage px              */
    uint32_t rgb[FLUID_ASK_MAX];                /* colours (labels + rim arcs)           */
} s_ask;
/* the listening ring (fluid_target_arc) and a question's dividers (fluid_draw_overlay) */
static bool  s_listen = false;                  /* task copies of s_listen_in / s_level_in */
static float s_level = 0.f;
static int32_t s_ring_rgb = -1, s_ring_def = -1;   /* task copies of s_ring_in / s_ring_dflt */
static fluid_ring_t s_rg;                       /* the listening ring (fluid_ring.h)     */
static float s_div = 0.f;
static int   s_div_n = 0;
static fluid_overlay_t s_ov;                    /* this frame's, from sim_frame          */
static fluid_overlay_t s_dov;                   /* riding with the snapshot              */
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
static int   s_cpu_mhz = 160;

/* ── Text: tokenise + schedule (same rules as mole_reader.c / reader.js) ───── */

static inline bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

static void tokenize(void) {
    s_ntok = 0;
    int len = (int)strlen(s_ttext), i = 0;
    while (i < len && s_ntok < FLUID_MAX_TOKENS) {
        int nl = 0;
        while (i < len && is_ws(s_ttext[i])) { if (s_ttext[i] == '\n') nl++; i++; }
        if (nl >= 2 && s_ntok > 0) s_toks[s_ntok - 1].para = 1;
        if (i >= len) break;
        int st = i;
        while (i < len && !is_ws(s_ttext[i])) i++;
        int tl = i - st;
        if (tl > FLUID_WORD_LETTERS) tl = FLUID_WORD_LETTERS;   /* display cap */
        gc_rsvp_orp_t orp = gc_rsvp_compute_orp(s_ttext + st, tl);
        ftok_t *t = &s_toks[s_ntok++];
        t->off = (int16_t)st; t->len = (int16_t)tl; t->orp = (int16_t)orp.focal; t->core = (int16_t)orp.core_len; t->para = 0;
    }
    /* A lone token is a static label (e.g. the pairing code), not a sequence:
     * centre it and drop the ORP accent letter (orp = -1, see fluid_text_set_word). */
    if (s_ntok == 1) s_toks[0].orp = -1;
}

static float word_delay_s(const ftok_t *t, int wpm) {
    float base = 60000.f / (float)(wpm < 1 ? 1 : wpm);
    float lf = t->core > 6 ? fminf(2.f, 1.f + 0.05f * (t->core - 6)) : 1.f;
    const char *w = s_ttext + t->off;
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

static void show_word(int idx) {
    const ftok_t *t = &s_toks[idx];
    fluid_text_set_word(s_tx, s_sim, s_ttext + t->off, t->len, t->orp);
    if (s_tint) tint_letters();
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
    if (st == ST_CREATURE && !s_cute) st = ST_BLOB;   /* no creature loaded */
    s_sim->edge_w = st == ST_CREATURE ? 1.f : 0.f;      /* solid eyes once settled */
    if (st == ST_TEXT) {
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
        s_word_until = s_tnow + 0.55f + word_delay_s(&s_toks[0], cfg->wpm);   /* lead-in */
    }
    /* the transition: the old shape breaks up in eddies + a ring from its centre */
    float cx, cy;
    fluid_sim_centroid(s_sim, &cx, &cy);
    if (s_sim->swirl_kick < 3.f) s_sim->swirl_kick = 3.f;
    if (st != ST_TEXT) ripple(cx, cy, 0.7f, 0.2f);
    else ripple(0.f, 0.f, 0.5f, 0.25f);
    s_cur = st; s_cur_fx = fx; s_fx_state = 0.f;
    if (st != ST_TEXT) memset(s_sim->bgm, 255, (size_t)s_sim->N);
    s_sim->p.relax_up = RATE_UP[st]; s_sim->p.relax_down = RATE_DOWN[st];
    if (from == ST_TEXT && st != ST_TEXT) s_melt_t = 0.f;
    if (st == ST_FX) fluid_effect_enter(s_sim, fx);
}

/* the director's pending transition, if any, then its mood on the creature (moods blend
 * inside the creature: cute_rt_set_mood fades between them and ignores the same mood) */
static void direct(const fluid_cfg_t *cfg) {
    int st, fx;
    /* a question owns the screen: the director's transitions wait under it */
    if (s_ask.scr.phase == FLUID_ASK_NONE && fd_take(&s_dir, &st, &fx)) enter_state(st, fx, cfg);
    if (s_cute) cute_rt_set_mood(s_cute, fd_mood(&s_dir));
}

/* ── A question (the link's ask): labels condense in their sectors ─────────── */

/* the labels as the text target: all while asking; after the pick only the chosen one
 * pools ink (accented), so the others melt back into the fluid */
static void ask_apply(void) {
    fluid_label_t lb[FLUID_ASK_MAX];
    for (int i = 0; i < s_ask.n; i++)
        lb[i] = (fluid_label_t){ s_ask.label[i], s_ask.len[i], s_ask.cx[i], s_ask.cy[i],
                                 s_ask.scr.phase == FLUID_ASK_SHOW || i == s_ask.scr.pick, s_ask.scr.phase == FLUID_ASK_PICK && i == s_ask.scr.pick,
                                 FLUID_RGB | s_ask.rgb[i] };
    fluid_text_set_labels(s_tx, s_sim, lb, s_ask.n, s_ask.fs);
    s_sim->edge_w = 0.f;
    s_sim->p.relax_up = RATE_UP[ST_TEXT]; s_sim->p.relax_down = RATE_DOWN[ST_TEXT];
}

/* a new question's labels (fluid_ask_follow already made it the one shown); `was` = the
 * phase before it */
static void ask_show(const ask_want_t *w, int was) {
    if (was == FLUID_ASK_NONE && s_cur == ST_FX) fluid_effect_exit(s_sim, s_cur_fx);   /* re-entered on leave */
    s_ask.n = w->n < 1 ? 1 : w->n > FLUID_ASK_MAX ? FLUID_ASK_MAX : w->n;
    for (int i = 0; i < s_ask.n; i++) {
        memcpy(s_ask.label[i], w->label[i], sizeof(s_ask.label[i]));
        s_ask.len[i] = (int)strlen(s_ask.label[i]);
        s_ask.rgb[i] = w->rgb[i];
    }
    s_ask.fs = fluid_ask_layout(s_ask.n, s_ask.len, s_stage, s_ask.cx, s_ask.cy);
    ask_apply();
    /* the transition, as into the reader: the old shape breaks up, a ring from the centre */
    if (s_sim->swirl_kick < 3.f) s_sim->swirl_kick = 3.f;
    ripple(0.f, 0.f, 0.5f, 0.25f);
    s_melt_t = 1e9f;                                       /* the overlay is the labels now */
    s_div_n = s_ask.n;
}

/* back to what the director shows: the creature, unless a request in this frame (the
 * command that ended the question) chose something; a state that is already the
 * director's forms again (its clock restarts) */
static void ask_leave(const fluid_cfg_t *cfg, bool requested) {
    s_melt_t = 0.f;                                        /* the labels' letters melt away */
    if (!requested) fd_request_state(&s_dir, ST_CREATURE, 0, false);
    if (!s_dir.pending) { fd_replay(&s_dir); enter_state(s_cur, s_cur_fx, cfg); }
}

/* follow what the link wants (once per frame, before the director's transitions);
 * `requested`: a state / mood request in this frame */
static void ask_follow(const ask_want_t *w, bool requested, const fluid_cfg_t *cfg) {
    int was = s_ask.scr.phase;
    bool keep;
    int did = fluid_ask_follow(&s_ask.scr, w->active, w->seq, w->pick, w->n, requested, &keep);
    if (did & FLUID_ASK_DID_SHOW) ask_show(w, was);
    if (did & FLUID_ASK_DID_PICK) ask_apply();             /* only the chosen one pools ink */
    if (did & FLUID_ASK_DID_LEAVE) ask_leave(cfg, keep);
}

/* ── (Re)build for a grid ───────────────────────────────────────────────────── */

static void free_grid(void) {
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
    s_noverlay = 0;
    s_full_flush = true;
    s_frame_no = 0;
    return ESP_OK;
}

/* ── Input ──────────────────────────────────────────────────────────────────── */

static int64_t s_touch_t0;            /* sim: tap seen, not yet handed to the drawer */
static volatile int64_t s_dtouch;     /* drawer: tap time riding with the current frame */
static void poll_touch(void) {
    uint16_t xs[MOLTY_TOUCH_MAX_POINTS], ys[MOLTY_TOUCH_MAX_POINTS];
    int n = molty_touch_poll(xs, ys, MOLTY_TOUCH_MAX_POINTS);
    if (n <= 0) { s_touch_down = false; return; }
    float x = (float)(xs[0] - s_ox) / (float)s_stage * 2.f - 1.f, y = (float)(ys[0] - s_oy) / (float)s_stage * 2.f - 1.f;
    if (!s_touch_down) {                                    /* tap = ripple */
        s_touch_down = true; s_tpx = x; s_tpy = y; s_last_tx = xs[0]; s_last_ty = ys[0];
        ripple(x, y, 1.6f, 0.09f);
        fd_user(&s_dir);                                    /* wakes a sleepy creature */
        if (!s_touch_t0) s_touch_t0 = esp_timer_get_time();
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
    const bool covered = s_ask.scr.phase != FLUID_ASK_NONE;
    fd_set_busy(&s_dir, s_listen || s_follow, covered);
    fd_tick(&s_dir, rdt, s_cur == ST_TEXT && s_word_until > 1e8f && !s_follow);
    direct(cfg);
    {   /* an action to start: an agent's, or idle life's (never a sound) */
        int a = fd_take_act(&s_dir);
        if (a >= 0 && s_cute) cute_rt_act(s_cute, a);
    }

    const bool asking = s_ask.scr.phase != FLUID_ASK_NONE;
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
        s_word_until = s_tnow + word_delay_s(&s_toks[s_word], cfg->wpm);
    } else if (!asking && s_cur == ST_TEXT && s_word < s_ntok && s_text_hidden) {
        /* the say ended (or failed) before word 0 was cued: the reader's own pace from word 0 */
        s_text_hidden = false;
        show_word(s_word);
        s_word_until = s_tnow + word_delay_s(&s_toks[s_word], cfg->wpm);
    } else if (!asking && s_cur == ST_TEXT && s_word < s_ntok && s_tnow >= s_word_until) {
        s_word++;
        if (s_word < s_ntok) {
            show_word(s_word);
            s_word_until = s_tnow + word_delay_s(&s_toks[s_word], cfg->wpm);
            ripple(0.f, 0.f, 0.45f + 0.6f * 0.3f, 0.12f);  /* a soft ring leaves the word */
        } else {
            s_word = s_ntok - 1; s_word_until = 1e9f;      /* hold the last word (the director
                                                            * moves on, or holds it) */
        }
    }
    s_melt = 0.f;
    if (asking) {
        s_ask.scr.t += rdt;
        if (s_ask.scr.phase == FLUID_ASK_PICK) s_ask.scr.pick_t += rdt;
        s_noverlay = s_tx->nletters;
        memcpy(s_overlay, s_tx->letters, sizeof(fluid_letter_t) * (size_t)s_noverlay);
        if (s_ask.scr.phase == FLUID_ASK_PICK) {                     /* the others' letters melt away */
            float m = fminf(0.8f, s_ask.scr.pick_t * 1.6f);
            for (int b = 0, off = 0; b < s_ask.n; off += s_ask.len[b++])
                if (b != s_ask.scr.pick)
                    for (int i = off; i < off + s_ask.len[b] && i < s_noverlay; i++) s_overlay[i].melt = m;
        }
    } else if (s_cur == ST_TEXT) {
        s_noverlay = s_tx->nletters;
        memcpy(s_overlay, s_tx->letters, sizeof(fluid_letter_t) * (size_t)s_noverlay);
    } else {
        s_melt_t += rdt; s_melt = s_melt_t;
        if (s_melt_t > 0.8f) s_noverlay = 0;               /* condensed letters melt away */
    }

    /* hold: crystallise the shape once the flow had time to arrive */
    if (asking) {                                          /* condenses like the reader's text */
        float q = fluid_clamp01((s_ask.scr.t * cfg->speed - 0.2f) / 0.4f);
        s->p.hold = q * q * (3.f - 2.f * q);
    } else s->p.hold = fd_solidity(&s_dir, cfg->speed);
    s->p.tint_rate = 3.f + 9.f * s->p.hold;

    /* input before the targets so the lean follows the hand of the moment */
    read_tilt(cfg->tilt);
    float ox = s_gx * FLUID_TILT_LEAN, oy = s_gy * FLUID_TILT_LEAN;

    /* a listen on the water, never over a question: each rise of the voice drops a
     * touch-style ripple and a faint line marks the rim (fluid_ring.h). The line is a target
     * of the fluid, around whatever the state shows. */
    const int ring_drop = fluid_ring_tick(&s_rg, rdt, s_listen && !asking, s_level);
    const float acc = cfg->palette == FLUID_PAL_PASTEL ? 0.f : 1.f;   /* mono palettes: the accent */

    switch (asking ? ST_TEXT : s_cur) {
    case ST_TEXT:     fluid_text_fill(s_tx, s, s_tnow); break;
    case ST_FX:       fluid_target_effect(s, s_cur_fx, s_tnow, swell, ox, oy); break;
    case ST_THINKING: fluid_target_blob(s, s_tnow * 1.4f, swell * 0.85f, ox, oy); break;
    case ST_CREATURE:
        if (s_cute) {
            cute_rt_update(s_cute, rdt);                   /* mood engine + kit + behaviours */
            cute_rt_raster(s_cute, s->W, s->H);
            cute_rt_fill(s_cute, s, swell);
            s_cute_tris = cute_rt_tris(s_cute);
        } else {
            fluid_target_blob(s, s_tnow, swell, ox, oy);   /* creature unloaded while showing */
        }
        break;
    default:          fluid_target_blob(s, s_tnow, swell, ox, oy); break;
    }
    if (asking) {                                          /* each button's border, in its colour */
        fluid_arc_t arcs[FLUID_ASK_MAX];
        for (int i = 0; i < s_ask.n; i++) {
            fluid_ask_arc_look(&s_ask.scr, i, cfg->speed, &arcs[i].amt, &arcs[i].hw);
            fluid_ask_arc(s_ask.n, i, s_stage, &arcs[i].a0, &arcs[i].a1);
            arcs[i].rgb = s_ask.rgb[i];
        }
        fluid_target_arcs(s, FLUID_ASK_ARC_R, arcs, s_ask.n, acc);   /* one pass for all */
    } else {
        fluid_target_ring(s, &s_rg, fluid_ring_rgb(s_ring_rgb, s_ring_def, cfg->palette), acc);
    }
    /* the always-on dim fluid; its slow noise (and the eddies' ψ) refresh on
     * alternating frames — both drift ≤ 0.012 lattice units per frame */
    fluid_sim_background(s, s_tnow, breath, (s_frame_no & 1) == 0 || s_frame_no < 2);
    if (s_cur == ST_FX && !asking) fluid_effect_tick(s, s_cur_fx, rdt, &s_fx_state);

    /* surface drivers (main.js): thinking = the motor's fine standing shimmer; the creature's
     * speech envelope rings the surface from its mouth; mic adds shimmer + random drops */
    int cmood = (s_cur == ST_CREATURE && s_cute && !asking) ? cute_rt_mood(s_cute) : -1;
    bool thinking = s_cur == ST_THINKING || cmood == CM_THINKING;
    float level = cmood == CM_SPEAKING ? cute_rt_env(s_cute) : 0.f;
    s->osc.x = 0.f; s->osc.y = cmood == CM_SPEAKING ? 0.12f : 0.f;
    s->osc.amp = (cmood == CM_SPEAKING ? 1.2f : thinking ? 0.25f : 0.5f) * level;
    s->osc.freq = 2.5f + 6.f * 0.4f;
    s->vibe.amp = (thinking ? 0.9f : 0.f) + cfg->mic * 1.4f;
    s->vibe.freq = 7.f + 9.f * 0.3f;
    if (cfg->mic > 0.f && frand() < cfg->mic * 0.35f * rdt * 60.f)
        ripple(frand() * 1.6f - 0.8f, frand() * 1.6f - 0.8f, 0.4f + cfg->mic, 0.06f);

    /* fixed-step physics */
    if ((s_frame_no & 1) == 1 || s_frame_no < 2) fluid_sim_swirl_field(s);
    s_acc += rdt;
    int n = 0;
    while (s_acc >= FLUID_STEP && n < 4) { fluid_sim_step(s, FLUID_STEP); s_acc -= FLUID_STEP; n++; }
    if (n == 4) s_acc = 0.f;
    fluid_sim_color(s, n * FLUID_STEP);                    /* colour rides the same flow, once per frame */
    fluid_sim_view(s);
    s_shimmer = 0.12f + (thinking ? 0.35f : 0.f) + cfg->mic * 0.4f;

    if (ring_drop) ripple(s_rg.drop_x, s_rg.drop_y, s_rg.drop_s, RING_DROP_R);   /* the voice touches the water */
    /* a question's faint dividers: in while it waits, out once it was answered */
    bool div_on = s_ask.scr.phase == FLUID_ASK_SHOW && s_ask.n >= 2;
    s_div = div_on ? fminf(1.f, s_div + rdt / DIV_FADE_S) : fmaxf(0.f, s_div - rdt / DIV_FADE_S);
    s_ov.sectors = s_div > 0.f ? s_div_n : 0;
    s_ov.divider = roundf(s_div * 32.f) / 32.f;
    s_frame_no++;
}

/* ── Creature (.molty): parse + build on the task; file I/O for the HTTP API ──── */

static esp_err_t alloc_bufs(void);

#define CUTE_PATH     "/spiffs/creature.molty"
#define CUTE_TMP_PATH "/spiffs/creature.molty.tmp"
/* firmware default (device/creatures/molty/duck.molty, EMBED_FILES in main/CMakeLists.txt) */
extern const uint8_t _binary_default_molty_start[];
extern const uint8_t _binary_default_molty_end[];

static void load_creature(const uint8_t *buf, size_t len) {
    cute_spec_t *spec = heap_caps_malloc(sizeof(cute_spec_t), MALLOC_CAP_SPIRAM);
    const char *err = "oom";
    cute_rt_t *rt = NULL;
    if (spec && cute_spec_parse(buf, len, spec, &err) == 0) {
        rt = cute_rt_create(spec, esp_random());
        if (!rt) err = "oom";
    }
    if (rt) {
        int mood = s_cute ? cute_rt_mood(s_cute) : CM_IDLE;
        cute_rt_destroy(s_cute);
        s_cute = rt;
        cute_rt_set_mood(s_cute, mood);
        ESP_LOGI(TAG, "creature '%s' loaded (%u B, %d parts)", spec->name, (unsigned)len, spec->nparts);
    } else {
        ESP_LOGW(TAG, "creature load failed: %s", err);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (rt) { strncpy(s_cute_name, spec->name, sizeof(s_cute_name) - 1); s_cute_err[0] = '\0'; }
    else strncpy(s_cute_err, err, sizeof(s_cute_err) - 1);
    xSemaphoreGive(s_mutex);
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

/* the stored creature (or the tmp left by a crash between remove and rename), else the
 * firmware default; the CRC check in cute_spec_parse makes a torn file harmless */
static uint8_t *creature_bytes(size_t *len, bool *is_default) {
    uint8_t *b = read_file(CUTE_PATH, len);
    if (!b) b = read_file(CUTE_TMP_PATH, len);
    *is_default = b == NULL;
    if (!b) {
        size_t n = (size_t)(_binary_default_molty_end - _binary_default_molty_start);
        b = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
        if (b) { memcpy(b, _binary_default_molty_start, n); *len = n; }
    }
    return b;
}

static void queue_creature(uint8_t *blob, size_t len) {   /* takes ownership */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_cute_blob) heap_caps_free(s_cute_blob);
    s_cute_blob = blob; s_cute_len = len;
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
    if (copy) { memcpy(copy, buf, len); queue_creature(copy, len); }       /* hot reload */
    if (err) *err = "ok";
    return ESP_OK;
}

esp_err_t mole_fluid_creature_file_get(uint8_t **out, size_t *len, bool *is_default) {
    bool d = false;
    *out = creature_bytes(len, &d);
    if (is_default) *is_default = d;
    return *out ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ── Drawer task (core 0): rasterise + flush the latest snapshot ─────────────── */

static void draw_task(void *arg) {
    (void)arg;
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK) ESP_LOGW(TAG, "drawer: esp_task_wdt_add failed (%s)", esp_err_to_name(wdt_err));
    float dms = 0.f, lms = 0.f, pema[2] = { 0.f, 0.f };
    /* director status → mark at the screen's edge (fluid_draw_mark) */
    static const int MARK[FD_STATUS_COUNT] = { FLUID_MARK_NONE, FLUID_MARK_ARC, FLUID_MARK_DOT, FLUID_MARK_ALERT };
    int mark_drawn = FLUID_MARK_NONE;
    fluid_overlay_t ov_drawn = { 0 };                   /* the overlay on screen */
    while (s_run) {
        esp_task_wdt_reset();
        if (xSemaphoreTake(s_go, pdMS_TO_TICKS(200)) != pdTRUE) continue;
        if (!s_run) { xSemaphoreGive(s_idle); break; }
        int64_t t0 = esp_timer_get_time();
        int64_t touch_t = s_dtouch; s_dtouch = 0;
        uint16_t *fb = molty_display_framebuffer();
        int y0 = 0, y1 = 0, nc = 0;
        int ds = s_dstatus, mark = ds > 0 && ds < FD_STATUS_COUNT ? MARK[ds] : FLUID_MARK_NONE;
        /* the mark appears or goes: one full repaint (cells under it are not dirty) */
        bool full = s_full_flush || mark != mark_drawn;
        const fluid_overlay_t ov = s_dov;
        const unsigned ask_seq = s_dask;
        if (fb && s_draw && s_frame) {
            if (full) { memset(fb, 0, (size_t)s_fb_w * s_fb_h * sizeof(uint16_t)); s_draw->force = true; }
            /* overlays blend over the pixel under them: redraw the cells under the last
             * one (restores what it covered) and the next one, then draw it on top */
            else { fluid_draw_overlay_dirty(s_draw, &ov_drawn); fluid_draw_overlay_dirty(s_draw, &ov); }
            nc = fluid_draw(s_draw, s_frame, fb, &s_dopts, &y0, &y1);
            fluid_draw_overlay(s_draw, fb, s_dopts.palette, &ov);
            fluid_draw_mark(s_draw, fb, s_dopts.palette, mark);    /* over any redrawn cell */
            ov_drawn = ov;
        }
        int64_t t1 = esp_timer_get_time();
        if (fb) {
            if (full) { molty_display_flush(); s_full_flush = false; mark_drawn = mark; }
            else if (y1 > y0) molty_display_flush_rows(y0, y1);
            atomic_store(&s_glass_ask, ask_seq);        /* on the panel now */
        }
        int64_t t2 = esp_timer_get_time();
        mole_diag_panel_frame((uint32_t)(t1 - t0), (uint32_t)(t2 - t1));
        if (touch_t) mole_diag_touch_latency((uint32_t)(t2 - touch_t));
        dms = dms * 0.9f + (t1 - t0) / 1000.f * 0.1f;
        lms = lms * 0.9f + (t2 - t1) / 1000.f * 0.1f;
        pema[0] = pema[0] * 0.9f + (float)fluid_prof[FP_DSAMPLE] / (float)s_cpu_mhz / 1000.f * 0.1f;
        pema[1] = pema[1] * 0.9f + (float)fluid_prof[FP_DRASTER] / (float)s_cpu_mhz / 1000.f * 0.1f;
        fluid_prof[FP_DSAMPLE] = fluid_prof[FP_DRASTER] = 0;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_stats.draw_ms = dms; s_stats.flush_ms = lms; s_stats.cells_drawn = nc;
        s_stats.prof_ms[FP_DSAMPLE] = pema[0]; s_stats.prof_ms[FP_DRASTER] = pema[1];
        xSemaphoreGive(s_mutex);
        xSemaphoreGive(s_idle);
    }
    esp_task_wdt_delete(NULL);
    xSemaphoreGive(s_wdone);
    vTaskDelete(NULL);
}

/* ── Sim task (core 1) ───────────────────────────────────────────────────────── */

static void fluid_task(void *arg) {
    (void)arg;
    /* TWDT: re-created on every select → self-subscribe; delete before exit */
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK) ESP_LOGW(TAG, "esp_task_wdt_add failed (%s)", esp_err_to_name(wdt_err));

    int64_t prev = esp_timer_get_time(), last_touch = 0;
    float fps = 0.f, sms = 0.f, wms = 0.f, fms = 0.f;
    TickType_t wake = xTaskGetTickCount();

    while (s_run) {
        esp_task_wdt_reset();
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
        req_reel = s_req_reel; s_req_reel = false;
        nrip = s_ripn; memcpy(rip, s_ripq, sizeof(rip)); s_ripn = 0;
        uint8_t *cblob = s_cute_blob; size_t clen = s_cute_len; s_cute_blob = NULL;
        int req_mood = s_req_mood; s_req_mood = -1;
        float req_hold_s = s_req_hold_s;
        int req_act = s_req_act; s_req_act = -1;
        bool spoke = s_req_spoke; s_req_spoke = false;
        bool shed = s_req_shed; s_req_shed = false;
        if (!s_idle_life_loaded) { s_idle_life = idle_life_load(); s_idle_life_loaded = true; }
        int idle_life = s_idle_life;
        uint32_t tint = s_tint_in;
        bool activity = s_req_activity, shake = s_req_shake, user = s_req_user;
        s_req_activity = s_req_shake = s_req_user = false;
        int status = s_status_in;
        ask_want_t want = s_ask_want;
        s_listen = s_listen_in; s_level = s_level_in; s_ring_rgb = s_ring_in; s_ring_def = s_ring_dflt;
        s_follow = s_follow_in; s_follow_word = s_follow_word_in;
        if (s_text_ver != s_seen_ver) {
            memcpy(s_ttext, s_text, (size_t)s_text_len + 1);
            s_seen_ver = s_text_ver; text_changed = true;
            text_append = s_text_appended; s_text_appended = false;
        }
        xSemaphoreGive(s_mutex);

        /* the heap guard: the same grid again, every hot field in PSRAM (internal RAM back to
         * Wi-Fi / TLS / the LCD; slower frames, never a starved radio) */
        const int shed_n = shed && s_sim ? s_sim->n_internal : 0;
        if (!s_sim || s_sim->W != cfg.grid_w || s_sim->H != cfg.grid_h || shed_n > 0) {
            xSemaphoreTake(s_idle, portMAX_DELAY);          /* own the drawer's buffers */
            if (build_grid(cfg.grid_w, cfg.grid_h) != ESP_OK) {
                ESP_LOGE(TAG, "grid %dx%d alloc failed — back to %dx%d", cfg.grid_w, cfg.grid_h, s_grid_w0, s_grid_h0);
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_cfg.grid_w = s_grid_w0; s_cfg.grid_h = s_grid_h0;
                xSemaphoreGive(s_mutex);
                if (build_grid(s_grid_w0, s_grid_h0) != ESP_OK) { xSemaphoreGive(s_idle); vTaskDelay(pdMS_TO_TICKS(200)); continue; }
            }
            xSemaphoreGive(s_idle);
            if (shed_n > 0)
                mole_diag_event(true, "fluid.shed hot fields internal %d -> %d, int_free=%u", shed_n,
                                s_sim ? s_sim->n_internal : -1, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            enter_state(s_cur, s_cur_fx, &cfg);         /* re-enter on the new grid */
            if (s_ask.scr.phase != FLUID_ASK_NONE) ask_apply();   /* ...and the question over it */
        }
        if (text_changed) {
            tokenize();
            if (s_cur == ST_TEXT && req < 0) {
                if (!text_append) { req = ST_TEXT; req_fx = 0; req_hold = s_dir.text_hold; }   /* new text: start over */
                else if (s_word_until > 1e8f && s_word + 1 < s_ntok)    /* streamed words arrived */
                    s_word_until = s_tnow;                              /* while holding the last */
            }
        }
        if (cblob) { load_creature(cblob, clen); heap_caps_free(cblob); fd_set_creature(&s_dir, s_cute != NULL); }
        /* the director: state first (it may end the reel), then the mood over it */
        if (req_reel) fd_autoplay(&s_dir);
        if (req >= 0) fd_request_state(&s_dir, req, req_fx, req_hold);
        if (req_mood >= 0) fd_agent_mood(&s_dir, req_mood, req_hold_s);
        else if (activity) fd_activity(&s_dir);
        if (user) fd_user(&s_dir);
        if (shake) fd_shake(&s_dir);
        fd_set_idle_life(&s_dir, idle_life);
        if (req_act >= 0) fd_agent_act(&s_dir, req_act);
        if (spoke) fd_turn_spoken(&s_dir);
        if (tint != s_tint) {                      /* a say's mood mark: its words from now on */
            s_tint = tint;
            if (s_cur == ST_TEXT && s_ask.scr.phase == FLUID_ASK_NONE) tint_letters();
        }
        fd_set_status(&s_dir, status);
        ask_follow(&want, req >= 0 || req_mood >= 0 || req_reel, &cfg);
        direct(&cfg);
        for (int q = 0; q < nrip; q++) ripple(rip[q][0], rip[q][1], rip[q][2], 0.09f);
        if (now - last_touch >= FLUID_TOUCH_US) { poll_touch(); last_touch = now; }

        int64_t t0 = esp_timer_get_time();
        sim_frame(rdt, &cfg);
        int64_t t1 = esp_timer_get_time();
        /* hand the frame to the drawer: wait until it finished the previous one */
        xSemaphoreTake(s_idle, portMAX_DELAY);
        int64_t t2 = esp_timer_get_time();
        fluid_sim_snapshot(s_sim, s_frame, cfg.edge_mode == FLUID_EDGE_OFF);
        s_dopts = (fluid_draw_opts_t){
            .palette = cfg.palette, .edge_mode = cfg.edge_mode,
            .shimmer = s_shimmer, .time = s_tnow, .melt = s_melt,
            .letters = s_dletters, .nletters = s_noverlay,
            .ramp = GC_RAMP_ASCII, .font = gc_font5x7,
        };
        memcpy(s_dletters, s_overlay, sizeof(fluid_letter_t) * (size_t)s_noverlay);
        if (s_touch_t0) { s_dtouch = s_touch_t0; s_touch_t0 = 0; }
        s_dstatus = fd_status(&s_dir);
        s_dov = s_ov;
        s_dask = fluid_ask_tappable(&s_ask.scr, cfg.speed);
        xSemaphoreGive(s_go);
        int64_t t3 = esp_timer_get_time();

        mole_diag_sim_frame(raw_dt_us, (uint32_t)(t1 - t0));
        /* stats (EMA) */
        fps = fps > 0.f ? fps * 0.93f + (1.f / fmaxf(rdt, 1e-3f)) * 0.07f : 30.f;
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
        s_stats.fps = fps; s_stats.frame_ms = fms; s_stats.sim_ms = sms; s_stats.wait_ms = wms;
        s_stats.energy = s_sim->energy; s_stats.err = s_sim->err;
        s_stats.state = s_cur; s_stats.fx = s_cur_fx; s_stats.reel = s_dir.autoplay ? s_dir.reel : -1;
        s_stats.autoplay = s_dir.autoplay; s_stats.asleep = fd_asleep(&s_dir); s_stats.status = fd_status(&s_dir);
        s_stats.mood = s_cute ? cute_rt_mood(s_cute) : -1; s_stats.ctris = s_cute_tris;
        s_stats.act = s_cute ? cute_rt_acting(s_cute) : -1;
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

        /* pace at 33.3 fps. An overrun frame used to always vTaskDelay(1): at a
         * 100 Hz tick that turned every 31 ms frame into a 40 ms one (p50 39.9 ms,
         * 27 fps, measured via /api/diag 2026-10-25). Now an overrun starts the
         * next frame at once — but never twice in a row, so lower-priority core-1
         * work still gets at least one tick every other frame. */
        static bool overran;
        if (xTaskDelayUntil(&wake, fd_asleep(&s_dir) ? FLUID_SLEEP_TICKS : FLUID_FRAME_TICKS) == pdFALSE) {
            if (overran) vTaskDelay(1);
            overran = !overran;
            wake = xTaskGetTickCount();
        } else overran = false;
    }
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
    if (!s_mutex || !s_done || !s_wdone || !s_go || !s_idle) return ESP_ERR_NO_MEM;
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
    if (!s_ttext || !s_toks) return ESP_ERR_NO_MEM;
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
    {   /* the stored creature (or the firmware default) — built on the task */
        size_t clen = 0; bool dflt = false;
        uint8_t *cb = creature_bytes(&clen, &dflt);
        if (cb) queue_creature(cb, clen);
    }
    /* boot: the director wants the creature (the stored or default .molty queued above
     * loads on the task's first frame); the blob stands in only if none loads */
    fd_init(&s_dir, NULL, esp_random());
    s_cur = ST_BLOB; s_cur_fx = 0; s_word = 0; s_ntok = 0;
    s_tnow = s_acc = 0.f; s_melt_t = 1e9f;
    s_gx = s_gy = 0.f; s_touch_down = false;
    s_ask.scr.phase = FLUID_ASK_NONE; s_ask.scr.seq = s_ask_want.seq;     /* a question from before is gone */
    s_dask = 0; atomic_store(&s_glass_ask, 0);
    s_follow = false; s_follow_word = 0; s_text_hidden = false;
    s_rg = (fluid_ring_t){ 0 }; s_div = 0.f; s_ov = s_dov = (fluid_overlay_t){ 0 };   /* the fluid's own ring is in the sim */
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
    free_grid();
    cute_rt_destroy(s_cute); s_cute = NULL;
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
    if (!s_idle_life_loaded) { s_idle_life = idle_life_load(); s_idle_life_loaded = true; }
    const int idle_life = s_idle_life;
    char cname[40], cerr[40];
    memcpy(cname, s_cute_name, sizeof(cname)); memcpy(cerr, s_cute_err, sizeof(cerr));
    cJSON *root = cJSON_CreateObject();
    if (root) cJSON_AddStringToObject(root, "text", s_text ? s_text : "");
    xSemaphoreGive(s_mutex);
    if (!root) return ESP_ERR_NO_MEM;

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
    cJSON_AddStringToObject(root, "act",         st.act >= 0 ? cute_act_name(st.act) : "");   /* the action playing */
    cJSON_AddStringToObject(root, "indicator",   FD_STATUS_NAMES[st.status]);
    {
        cJSON *cj = cJSON_AddObjectToObject(root, "creature_info");
        if (cj) {
            cJSON_AddStringToObject(cj, "name", cname);
            cJSON_AddNumberToObject(cj, "tris", st.ctris);
            if (cerr[0]) cJSON_AddStringToObject(cj, "error", cerr);
        }
    }
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
    cJSON_AddStringToObject(root, "idle_life", FD_IDLE_NAMES[idle_life]);
    cJSON_AddStringToObject(root, "edge_mode",   FLUID_EDGE_NAMES[c.edge_mode]);
    cJSON_AddStringToObject(root, "grid",        grid);
    cJSON_AddNumberToObject(root, "speed",       c.speed);
    cJSON_AddNumberToObject(root, "mic",         c.mic);
    cJSON_AddNumberToObject(root, "tilt",        c.tilt);
    cJSON_AddStringToObject(root, "word",        st.word);
    cJSON_AddNumberToObject(root, "word_idx",    st.word_idx);
    cJSON_AddNumberToObject(root, "nwords",      st.nwords);
    /* read-only timing (EMA, ms). Core 1: sim_ms, wait_ms (blocked on the drawer),
     * frame_ms (sim + wait + snapshot, excludes pacing). Core 0: draw_ms, flush_ms. */
    cJSON_AddNumberToObject(root, "fps",         roundf(st.fps * 10.f) / 10.f);
    cJSON_AddNumberToObject(root, "frame_ms",    roundf(st.frame_ms * 100.f) / 100.f);
    cJSON_AddNumberToObject(root, "sim_ms",      roundf(st.sim_ms * 100.f) / 100.f);
    cJSON_AddNumberToObject(root, "wait_ms",     roundf(st.wait_ms * 100.f) / 100.f);
    cJSON_AddNumberToObject(root, "draw_ms",     roundf(st.draw_ms * 100.f) / 100.f);
    cJSON_AddNumberToObject(root, "flush_ms",    roundf(st.flush_ms * 100.f) / 100.f);
    cJSON_AddNumberToObject(root, "cells_drawn", st.cells_drawn);
    cJSON_AddNumberToObject(root, "cells_inside", st.inside);
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
    /* act: a one-shot action (cute_act.h) on the creature, as the link's `act` */
    int act = -2;                                      /* -2: not given */
    if ((j = cJSON_GetObjectItem(root, "act"))) {
        act = cJSON_IsString(j) ? cute_act_lookup(j->valuestring) : -1;
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
        if (m < 0) ret = ESP_ERR_INVALID_ARG;
        else {
            s_req_mood = m; s_req_hold_s = hold_s;
            /* a mood alone means "show me the creature in that mood" */
            if (!state_given) { s_req_state = ST_CREATURE; s_req_fx = 0; }
        }
    }
    if ((j = cJSON_GetObjectItem(root, "state")) && cJSON_IsString(j)) {
        int st, fx;
        if (!strcmp(j->valuestring, "autoplay")) s_req_reel = true;     /* the demo reel, on request only */
        else if (parse_state(j->valuestring, &st, &fx)) {
            s_req_state = st; s_req_fx = fx;
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

void mole_fluid_event(mole_fluid_event_t ev) {
    if (!s_mutex) return;                              /* fluid not up yet */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (ev == MOLE_FLUID_SHAKE) s_req_shake = true; else s_req_user = true;
    xSemaphoreGive(s_mutex);
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
    if (!on) s_level_in = 0.f;
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
    if (!s_mutex || act < 0 || act >= CUTE_N_ACTS) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_req_act = act;
    xSemaphoreGive(s_mutex);
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
    s_req_shed = true;
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

void mole_fluid_listen_level(float level) {
    if (!s_mutex) return;
    level = isfinite(level) ? fminf(1.f, fmaxf(0.f, level)) : 0.f;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_level_in = level;
    xSemaphoreGive(s_mutex);
}

esp_err_t mole_fluid_ask_show(int n, const char *const *labels, const int32_t *colors, unsigned *seq) {
    if (n < 1 || n > FLUID_ASK_MAX || !labels || !seq) return ESP_ERR_INVALID_ARG;
    esp_err_t e = alloc_bufs();
    if (e != ESP_OK) return e;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (++s_ask_want.seq == 0) s_ask_want.seq = 1;     /* 0 = no question (mole_fluid_ask_shown) */
    s_ask_want.active = true; s_ask_want.n = n; s_ask_want.pick = -1;
    for (int i = 0; i < n; i++) {
        snprintf(s_ask_want.label[i], sizeof(s_ask_want.label[i]), "%s", labels[i] ? labels[i] : "");
        s_ask_want.rgb[i] = fluid_ask_color(colors ? colors[i] : -1, i);
    }
    s_req_activity = true;                             /* an agent is here: no dozing off */
    *seq = s_ask_want.seq;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

unsigned mole_fluid_ask_shown(void) { return atomic_load(&s_glass_ask); }

void mole_fluid_ask_pick(int index) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_ask_want.active && index >= 0 && index < s_ask_want.n) s_ask_want.pick = index;
    xSemaphoreGive(s_mutex);
}

void mole_fluid_ask_end(void) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_ask_want.active = false;
    xSemaphoreGive(s_mutex);
}
