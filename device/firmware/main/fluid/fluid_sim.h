/*
 * fluid_sim.h — "fluid of characters" core: per-cell fluid sim, targets, braille renderer.
 *
 * Panel-agnostic: the fluid fills a square STAGE (the largest centred square of the
 * board's display, sized at runtime — see molty_board.h); every pixel size here is in
 * stage px. Tuned on the 412x412 Waveshare 1.46, where stage = panel.
 *
 * Plain-C port of device/creatures/js/fluid/{sim,synth,targets,render}.js.
 * NO ESP-IDF dependencies: the same three .c files (fluid_sim.c, fluid_target.c,
 * fluid_draw.c) build on the host for test/host/fluid_test.c. The creature
 * wrapper (task, JSON, LCD, touch, IMU) is mole_fluid.c.
 *
 * ONE per-cell state for the whole round screen; every visible thing is a TARGET
 * (T + tcol) the state relaxes toward, so a transition is the same physics
 * as a ripple. Per fixed step (60 Hz):
 *   1. wave:    h' = (2h − hp + c²·∇²h)·damp + forcing   (0 outside the disc)
 *   2. demand:  D = T − ink; φ = ½·box₂(D) + 0.9·box₇(D)
 *   3. flow:    (u,v) += k·∇φ·dt + tilt·dt; (u,v) ·= drag
 *   4. swirl:   s = curl(noise3)·amp + solid-body rotation (vortex)
 *   5. advect:  semi-Lagrangian backtrace of ink by mobility·((u,v)+s)
 *               (colour/accent: same backtrace once per frame, fluid_sim_color)
 *   6. relax:   ink += (T − ink)·(1 − e^(−k·dt))
 *   view:       ink sampled at x + refr·∇h (ripples refract) + crest highlight
 * Cells are not square, so y-derivatives carry the aspect A = cellH/cellW.
 *
 * Float math (the S3 FPU does single-cycle fmadd) over int16 storage; no libm calls
 * in per-cell loops (exp per step, noise by integer hash + fast floor, sin by poly).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLUID_STAGE_REF   412      /* stage px the pixel constants were tuned at (1.46) */
#define FLUID_GRID_MIN_W  24
#define FLUID_GRID_MAX_W  64
#define FLUID_GRID_MIN_H  16
#define FLUID_GRID_MAX_H  40
#define FLUID_MAX_LETTERS 64       /* a question's 5 labels × 8 letters; a chart's 22 symbols + a subtitle */
#define FLUID_WORD_LETTERS 32      /* the speed reader's display cap per word        */
#define FLUID_MAX_SHAPES  256      /* a scene's shapes in high definition (fluid_shape_t) */

/* ── profiling: CPU cycles per phase, accumulated until the caller resets ── */
enum { FP_TARGET, FP_BG, FP_SWIRL, FP_WAVE, FP_DEMAND, FP_FLOW, FP_ADVECT, FP_RELAX, FP_COLOR, FP_VIEW,
       FP_C_UPDATE, FP_C_RASTER, FP_C_CLEAR, FP_C_VERT, FP_C_SAMPLES, FP_C_AGG, FP_C_FILL,
       /* a rigged creature's frame (creature_dots.c), inside c_update */
       FP_K_PLAY, FP_K_PROJ, FP_K_SOLID, FP_K_LINES, FP_K_FEAT, FP_K_HALO, FP_K_CELLS, FP_K_WAIT, FP_K_BAND,
       FP_DSAMPLE, FP_DRASTER, FP_N };
extern const char *const FLUID_PROF_NAMES[FP_N];
extern uint32_t fluid_prof[FP_N];
#ifdef ESP_PLATFORM
#include "esp_cpu.h"
static inline uint32_t fluid_cycles(void) { return (uint32_t)esp_cpu_get_cycle_count(); }
#else
static inline uint32_t fluid_cycles(void) { return 0; }
#endif
#define FLUID_PROF_BEGIN(v) uint32_t v = fluid_cycles()
#define FLUID_PROF_END(ph, v) (fluid_prof[ph] += fluid_cycles() - (v))
/* host tests only (-DFLUID_WORK_COUNT): the dots of the 2 x 4 braille lattice a frame's per-dot loops visit, a
 * count no machine's speed changes (scene_test's frame-cost budget); nothing at all on the device. DRAW: a dot tried
 * against the drawing (an item's, a triangle's); PASS: a dot a whole-lattice pass reads or writes (clear, fold). */
enum { FLUID_DOTS_DRAW, FLUID_DOTS_PASS };
#ifdef FLUID_WORK_COUNT
extern unsigned long fluid_dot_visits[2];
#define FLUID_DOTS(k, n) (fluid_dot_visits[k] += (unsigned long)(n))
#else
#define FLUID_DOTS(k, n) ((void)0)
#endif

/* ── sim ──────────────────────────────────────────────────────────────────── */

typedef struct {
    float c2, damp, leak;              /* wave speed², velocity damping, height leak     */
    float attract, drag, vmax;         /* demand-flow gain, drag (1/s), speed cap (c/s)  */
    float relax_up, relax_down, tint_rate;
    float swirl, swirl_freq;           /* ambient eddies                                 */
    float refr, crest;                 /* ripple refraction + crest highlight            */
    float speed_mul, hold;
    bool  still;                       /* only the surface moves (fluid_sim_step: the wave
                                        * alone; fluid_sim_color: each cell its target's colour):
                                        * the ink holds while nothing shows it              */
    /* the dim background fluid: an ink floor in EVERY state */
    float bg_level, bg_freq, bg_drift, bg_speed, bg_rim, bg_mob;
    float bg_color[3];
    /* the floor (fluid_sim_floor_*): the dim fluid laid on a plane seen in perspective, 0 = flat */
    float floor, horizon;              /* weight 0..1 (eased by the caller), horizon y (−1 top) */
    float tilt_x, tilt_y;              /* gravity bias on the flow, cells/s² (IMU)        */
} fluid_params_t;

typedef struct { float amp, freq, x, y, phase; } fluid_osc_t;

/* A ripple on the floor plane (fluid_sim_floor_ripple): a ring spreading on the ground, so on screen
 * an oval, flatter and finer toward the horizon. Plane units: x across, z the depth (1 = the bottom). */
#define FLUID_FLOOR_RIPPLES 8
typedef struct { float x, z, age, amp; } fluid_floor_ripple_t;
/* What walks on it (fluid_sim_floor_track): the creature's feet, followed from its target. */
typedef struct {
    bool seen;                         /* feet found last frame                            */
    float x, y, w;                     /* the feet, smoothed: centre x, the bottom y, the width */
    float body;                        /* the whole body's centre x, smoothed (a walk moves it) */
    float ground;                      /* where its feet rest, y (the lowest of late)       */
    float dist;                        /* walked since the last step, stage units          */
    float air;                         /* how high it got off the ground since it left it  */
    int side;                          /* the next step's foot, ±1                         */
    float gap;                         /* s until another ripple may start                 */
    float settle;                      /* s until the feet may ripple (they just appeared)  */
    float shadow;                      /* the shadow's fade 0..1                           */
} fluid_floor_feet_t;

/* Storage: the hot per-step scalar fields are int16 fixed point (math stays float),
 * so a 52×32 sim is ~47 KB and lives in internal DRAM — PSRAM through the 32 KB
 * data cache made every per-cell pass 5-10× slower (measured: relax 1100 vs 200
 * cycles/cell). Colour + accent travel as packed RGBA16 in PSRAM and are advected
 * once per FRAME (fluid_sim_color), not once per step. */
#define FQ_UNIT  16384.f              /* Q14: ink, ink2, T, view, psi        (0..1)      */
#define FQ_PHI   8192.f               /* Q13: D, phi, tmp, blur scratch     (±4)        */
#define FQ_H     1024.f               /* Q10: h, hp                         (±32)       */
#define FQ_U     512.f                /* Q9:  u, v                          (±64 c/s)   */

static inline int16_t fq(float x, float scale) {
    x *= scale;
    if (!(x >= -32767.f)) x = -32767.f;           /* also maps NaN → floor */
    if (x > 32767.f) x = 32767.f;
    return (int16_t)(x >= 0.f ? x + 0.5f : x - 0.5f);
}
static inline float fqf(int16_t v, float inv_scale) { return (float)v * inv_scale; }

typedef struct { uint16_t r, g, b, a; } fluid_rgba16_t;           /* 0..65535 each        */
static inline uint32_t fluid_pack_rgba8(float r, float g, float b, float a) {
    int R = (int)(r * 255.f + 0.5f), G = (int)(g * 255.f + 0.5f), B = (int)(b * 255.f + 0.5f), A = (int)(a * 255.f + 0.5f);
    R = R < 0 ? 0 : R > 255 ? 255 : R; G = G < 0 ? 0 : G > 255 ? 255 : G;
    B = B < 0 ? 0 : B > 255 ? 255 : B; A = A < 0 ? 0 : A > 255 ? 255 : A;
    return (uint32_t)R | (uint32_t)G << 8 | (uint32_t)B << 16 | (uint32_t)A << 24;
}

typedef struct {
    int W, H, N, inside;
    int stage;                         /* stage side, px                                  */
    float A;                           /* cell aspect cellH/cellW                         */
    float *xs, *ys;                    /* cell centres in −1..1 (per column / per row)    */
    /* hot fields (internal DRAM when it fits) */
    int16_t *h, *hp, *ink, *ink2, *u, *v, *D, *phi, *tmp, *psi, *T, *view;
    int16_t *bgn;                      /* cached background-floor noise term (Q13)        */
    uint8_t *mask, *sponge, *rim, *bgm;   /* bgm: background multiplier (text darkens)   */
    uint8_t *tf;                       /* target solid features (creature eyes), 0/1       */
    float edge_w;                      /* >0 while the target supplies features (creature) */
    /* colour (PSRAM) */
    fluid_rgba16_t *col, *col2;        /* carried colour + accent                         */
    uint32_t *tcol;                    /* target colour + accent, RGBA8                   */
    void *blk[20], *cblock;            /* one allocation per hot field (+ colour block)   */
    void *ring_geom;                   /* the listening ring's cells (fluid_target_ring,
                                        * built on first use, freed with the sim)          */
    int ring_n;
    int n_blk, n_internal;             /* how many hot fields landed in internal DRAM     */
    bool internal;                     /* all of them                                     */
    fluid_params_t p;
    float swirl_kick, rotate;
    fluid_osc_t osc, vibe, sheet;
    fluid_floor_ripple_t frip[FLUID_FLOOR_RIPPLES];
    fluid_floor_feet_t feet;
    float t, err, energy;
} fluid_sim_t;

/** Prototype SIM_TUNING defaults. */
void         fluid_params_default(fluid_params_t *p);
/** W×H cells over a square stage of `stage` px (cells are stage/W × stage/H px). */
fluid_sim_t *fluid_sim_create(int W, int H, int stage);
/** Default grid for a stage: the 1.46's 52×32 at the same cell size in px, clamped to
 *  the FLUID_GRID limits (412 → 52×32, 240 → 30×19). */
void         fluid_grid_for_stage(int stage, int *W, int *H);
void         fluid_sim_destroy(fluid_sim_t *s);
/** Whether sims created from now on may put hot fields in internal DRAM (default: yes,
 *  while 32 KB stay free). Off: every field in PSRAM — the heap guard's shed
 *  (diag/heap_guard.h) gives the internal RAM back to Wi-Fi / TLS / the LCD. */
void         fluid_sim_allow_internal(bool on);
/** Gaussian bump in the height field; x,y in −1..1 stage space. */
void  fluid_sim_impulse(fluid_sim_t *s, float x, float y, float strength, float radius);
/** Push the flow around (x,y) (a drag on the stage). */
void  fluid_sim_push(fluid_sim_t *s, float x, float y, float du, float dv, float radius);
/** Raise T to the dim background floor (call after a target fill). refresh: recompute
 *  the slow floor noise (every other frame is enough; always on the first call). */
void  fluid_sim_background(fluid_sim_t *s, float t, float breath, bool refresh);
/** The floor (p.floor > 0): a step or a landing at screen (x, y) on the floor sends a ripple across
 *  the plane (amp ~1 a step); ignored above the horizon. */
void  fluid_sim_floor_ripple(fluid_sim_t *s, float x, float y, float amp);
/** Once per frame, after the creature's target fill and before fluid_sim_background: finds its
 *  feet in T, lays its shadow under them, and drops a ripple each stride it walks and when it lands.
 *  Ages the ripples by dt. creature: a creature is what the target shows. */
void  fluid_sim_floor_track(fluid_sim_t *s, float dt, bool creature);
/** Stream function for the eddies — once per frame, before the steps. */
void  fluid_sim_swirl_field(fluid_sim_t *s);
void  fluid_sim_step(fluid_sim_t *s, float dt);
/** Advect + relax the carried colour by the current flow over dt (once per frame). */
void  fluid_sim_color(fluid_sim_t *s, float dt);
void  fluid_sim_view(fluid_sim_t *s);
/** Effective flow (cells/s) at cell k — the advection velocity (ASCII streamlines). */
void  fluid_sim_flow_at(const fluid_sim_t *s, int i, int j, float *wu, float *wv);
void  fluid_sim_centroid(const fluid_sim_t *s, float *cx, float *cy);

/* ── frame snapshot: everything the renderer reads, so drawing + flushing frame N
 *    can run on the other core while the sim computes frame N+1 ── */
typedef struct {
    int W, H;
    int16_t *view, *ink, *h;
    fluid_rgba16_t *col;
    uint8_t *mask;
    uint8_t *stroke;                   /* ASCII streamline: 0 none, 1+dir (- \ | /)    */
    uint8_t *feat;                     /* solid feature cell (creature eyes / mouth)       */
    void *block;
} fluid_frame_t;

fluid_frame_t *fluid_frame_create(int W, int H);
void  fluid_frame_destroy(fluid_frame_t *f);
/** Copy the drawable state out of the sim (~15 B/cell). strokes: fill f->stroke. */
void  fluid_sim_snapshot(const fluid_sim_t *s, fluid_frame_t *f, bool strokes);
/** Bilinear lookup of a fixed-point field at stage coords (letters overlay). */
float fluid_frame_sample(const fluid_frame_t *f, const int16_t *arr, float inv_scale, float x, float y);
float fluid_frame_sample_acc(const fluid_frame_t *f, float x, float y);

/* ── field synth (synth.js) ───────────────────────────────────────────────── */

enum { FV_NOISE, FV_RADIAL, FV_SPIRAL, FV_ANGULAR, FV_LINX, FV_LINY };
enum { FW_SIN, FW_SAW, FW_TRI };

typedef struct { uint8_t field, wave; float freq, speed, amp; } fluid_voice_t;
typedef struct { float gain, bias; int nv; fluid_voice_t v[3]; } fluid_patch_t;  /* combine: add */

float fluid_noise3(float x, float y, float z);       /* value noise, 0..1 */
float fluid_patch(const fluid_patch_t *p, float x, float y, float t);   /* 0..1 */
float fluid_atan2(float y, float x);
/** sin(2π·t), |err| < 0.001 (parabola + one refinement), no libm */
static inline float fluid_sin_turns(float t) {
    int fi = (int)t; fi -= (t < (float)fi);
    float x = (t - (float)fi) * 2.f - 1.f;           /* −1..1 ↔ −π..π; sin(π(x+1)) = −sin(πx) */
    float y = 4.f * x * (1.f - (x < 0.f ? -x : x));
    y = 0.225f * (y * (y < 0.f ? -y : y) - y) + y;
    return -y;
}
static inline float fluid_clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
static inline float fluid_smooth(float a, float b, float x) {
    float t = fluid_clamp01((x - a) / (b - a)); return t * t * (3.f - 2.f * t);
}

/* ── targets (targets.js) ─────────────────────────────────────────────────── */

enum { FLUID_FX_RIPPLE, FLUID_FX_VORTEX, FLUID_FX_WAVE, FLUID_FX_COUNT };
extern const char *const FLUID_FX_NAMES[FLUID_FX_COUNT];

/** Breathing metaball. (ox,oy) leans it (IMU tilt). */
void fluid_target_blob(fluid_sim_t *s, float t, float swell, float ox, float oy);
void fluid_target_effect(fluid_sim_t *s, int fx, float t, float swell, float ox, float oy);
void fluid_effect_enter(fluid_sim_t *s, int fx);
void fluid_effect_tick(fluid_sim_t *s, int fx, float dt, float *st);   /* st: 1 float of fx state */
void fluid_effect_exit(fluid_sim_t *s, int fx);

typedef struct {
    char  ch;
    bool  acc;                         /* ORP letter                                      */
    bool  whole;                       /* over a scene: whole whatever the wave (a question's words
                                        * break into dust by themselves, fluid_askview.h)  */
    float x, y, w, h, fs;              /* stage px                                        */
    float melt;                        /* this letter's own melt, added to the frame's    */
    uint32_t rgb;                      /* FLUID_RGB | 0xRRGGBB: its own colour (pastel);
                                        * 0 = the palette's                               */
    const uint8_t *mask;               /* not a letter but a symbol: a 1-bit mw x mh image
                                        * (rows MSB first) drawn crisp in the whole box   */
    uint16_t mw, mh;
} fluid_letter_t;
#define FLUID_RGB 0x1000000u           /* "a colour is set" flag over 0xRRGGBB            */

/* A scene's shape in high definition (fluid_draw_opts_t.shapes): drawn anti-aliased at the
 * panel's own pixels, in painter's order (each covers what is under it by its coverage). */
enum { FLUID_SHAPE_SEG, FLUID_SHAPE_RING, FLUID_SHAPE_DISC, FLUID_SHAPE_MASK, FLUID_SHAPE_CHAR };
typedef struct {
    uint8_t k;                         /* FLUID_SHAPE_*                                   */
    bool  acc;                         /* a letter in the accent (the subtitle's ORP)      */
    uint16_t mw, mh;                   /* mask: its bits' size (a char: the 5 x 7 font)    */
    uint32_t rgb;                      /* as fluid_letter_t; FLUID_RGB | 0 erases          */
    float x, y, x1, y1;                /* stage px, y down: a segment (x, y)..(x1, y1); the
                                        * others' centre (x, y)                           */
    float r, hw;                       /* ring / disc radius; segment / ring half width   */
    float a0, span;                    /* ring: from a0 over span degrees (counter-clockwise,
                                        * 0 = right), span >= 360 the whole ring          */
    float w, h, co, si;                /* mask / char: box size px, turned (cos, sin)      */
    const uint8_t *mask;               /* mask: 1-bit rows MSB first; char: 5 x 7 font rows */
    uint8_t tag;                       /* FLUID_TAG_*: what it is to the fine dust          */
} fluid_shape_t;
enum { FLUID_TAG_DRAWING, FLUID_TAG_CAPTION };   /* a caption letter forms from the dust (fluid_draw_opts_t.cap_reveal) */

/** A shape's own colour in the palette, rgb565 (not byte-swapped); 0: it erases. */
uint16_t fluid_shape_565(const fluid_shape_t *sh, int palette);
/** Each shape's box in fb px [x0, x1) x [y0, y1), its soft edge included (stage at ox, oy). */
void fluid_shape_box(const fluid_shape_t *sh, int ox, int oy, int16_t b[4]);
/** shapes[0..n) (boxes: fluid_shape_box) over fb pixel row y of row (row[0] is fb x bx), inside [xa, xb), each over what
 *  is under it by its coverage times wt[x] / 8 (wt NULL: whole); caption letters only where they formed (unformed 0..1). */
void fluid_shapes_row(const fluid_shape_t *sh, int n, const int16_t (*box)[4], int ox, int oy, int palette, const uint8_t *wt,
                      float unformed, uint16_t *row, int bx, int y, int xa, int xb);
/** The same with the caption's backdrop over the drawing: dim = { x0, x1, y0, y1 (its box, stage px), strength, from, to }:
 *  shapes but the caption's own at (1 - strength) inside it, back to whole FLUID_DIM_REACH px out, only from `from` px
 *  of the box to `to` (the backdrop coming and lifting as a front). */
#define FLUID_DIM_REACH 28.f
void fluid_shapes_row_dim(const fluid_shape_t *sh, int n, const int16_t (*box)[4], int ox, int oy, int palette, const uint8_t *wt,
                          float unformed, const float *dim, uint16_t *row, int bx, int y, int xa, int xb);

typedef struct {
    float *cov, *orp, *near, *near_s, *dark, *tmp;
    uint8_t *dots;                     /* fluid_text_set_glyphs: each cell's braille dots  */
    void  *block;
    int    stage;                      /* the sim's stage side, px                        */
    float  plain_font;
    int    nletters;
    fluid_letter_t letters[FLUID_MAX_LETTERS];
} fluid_text_t;

fluid_text_t *fluid_text_create(const fluid_sim_t *s);
void fluid_text_destroy(fluid_text_t *tx);
/** Fix the type size for a sentence: ext = max over words of the wider pivot half. */
void fluid_text_set_sentence(fluid_text_t *tx, float ext);
void fluid_text_set_word(fluid_text_t *tx, const fluid_sim_t *s, const char *w, int len, int orp);
/** A word in the fluid's own braille dots (the boot screen's "molty.uno"): its 5x7 letters
 *  on the dot lattice (tx->dots, for fluid_draw_opts_t.dots), the fluid pooling under them
 *  (cov), no ASCII letters (nletters 0). As wide as the stage allows; lowercase stays
 *  lowercase. Then fluid_text_fill. */
void fluid_text_set_glyphs(fluid_text_t *tx, const fluid_sim_t *s, const char *w);
void fluid_text_fill(fluid_text_t *tx, fluid_sim_t *s, float t);

/** A question's labels (fluid_ask.h lays them out): each centred at (cx, cy) stage px, one
 *  type size fs. Every label's letters go into tx->letters in order (label i's letters
 *  follow label i−1's); only `shown` labels pool ink (the target), so a label that is not
 *  shown melts back into the fluid; `acc` labels are accented. Then fluid_text_fill. */
typedef struct { const char *text; int len; float cx, cy; bool shown, acc; uint32_t rgb; } fluid_label_t;   /* rgb: as fluid_letter_t */
void fluid_text_set_labels(fluid_text_t *tx, const fluid_sim_t *s, const fluid_label_t *lb, int n, float fs);

/** A ring / arc of the fluid itself (the listening ring, a question's button borders):
 *  raises T toward amt·FLUID_ARC_INK over the band |r − rc| ≤ hw (soft edge outside it),
 *  angles a0..a1 (radians clockwise from 12 o'clock; a1 − a0 ≥ 2π = the whole ring, soft
 *  ends otherwise), and tints those cells toward rgb (0xRRGGBB) with accent acc (0..1:
 *  mono palettes show the accent, pastel the colour). Radii in stage units (1 = the rim).
 *  Call after the state's target fill, before fluid_sim_background. */
#define FLUID_ARC_INK 0.72f
void fluid_target_arc(fluid_sim_t *s, float rc, float hw, float a0, float a1, float amt, uint32_t rgb, float acc);
/** Several arcs on one centre line in ONE pass (a question's 1..5 borders): each cell
 *  takes the strongest arc there (its ink and colour). */
typedef struct { float a0, a1, hw, amt; uint32_t rgb; } fluid_arc_t;
void fluid_target_arcs(fluid_sim_t *s, float rc, const fluid_arc_t *arcs, int n, float acc);

/** A listen on the water (knobs: fluid_ring.h): a fixed circle in the ring colour. fluid_ring_tick
 *  advances it once per frame: on = a listen records (it fades in / melts out over RING_FADE_S).
 *  fluid_target_ring writes this frame's circle into the target in the given colour (as
 *  fluid_target_arcs: after the state's fill, before fluid_sim_background); nothing while faded out. */
typedef struct {
    float on;                          /* fade 0..1                                       */
    float ink;                         /* this frame's line ink                           */
} fluid_ring_t;
void fluid_ring_tick(fluid_ring_t *r, float dt, bool on);
void fluid_target_ring(fluid_sim_t *s, const fluid_ring_t *r, uint32_t rgb, float acc);

/** The agent's progress on the same line (knobs: fluid_ring.h): it fills clockwise from 12 o'clock
 *  to pct (0..1, eased), or, with pct < 0 (no percent given), a comet circles. fluid_progress_tick
 *  once per frame: on = work in progress; done = it finished (the full ring flashes, then melts).
 *  fluid_target_progress writes it into the target as fluid_target_ring does. */
typedef struct {
    float on;                          /* fade 0..1                                       */
    float pct;                         /* the latest percent, 0..1, < 0 = unknown          */
    float shown;                       /* the fill drawn, eased toward pct                 */
    float spin;                        /* the comet's head, turns                          */
    float flash;                       /* s of the done flash left                         */
} fluid_progress_t;
void fluid_progress_tick(fluid_progress_t *p, float dt, bool on, float pct, bool done);
void fluid_target_progress(fluid_sim_t *s, const fluid_progress_t *p, uint32_t rgb, float acc);

/* ── renderer (render.js) ─────────────────────────────────────────────────── */

enum { FLUID_PAL_GREEN, FLUID_PAL_AMBER, FLUID_PAL_PASTEL, FLUID_PAL_COUNT };
enum { FLUID_EDGE_OFF, FLUID_EDGE_CONTOURS, FLUID_EDGE_ALL, FLUID_EDGE_COUNT };  /* off = ASCII */
extern const char *const FLUID_PAL_NAMES[FLUID_PAL_COUNT];
extern const char *const FLUID_EDGE_NAMES[FLUID_EDGE_COUNT];

/** U+2800 braille bit for dot (col 0..1, row 0..3): dots 1-3 = col0 rows0-2 → bits 0-2,
 *  dots 4-6 = col1 rows0-2 → bits 3-5, dot 7 = col0 row3 → bit 6, dot 8 = col1 row3 → bit 7. */
static inline int fluid_braille_bit(int col, int row) { return row < 3 ? col * 3 + row : 6 + col; }
/** Tonal fill of 8 sub-samples (sub[col*4+row]) → U+2800 mask, ordered dither on the global
 *  4×4 Bayer lattice of sub-dots. (si,sj) = cell; shimmer jitters the thresholds. */
uint8_t fluid_braille_dither(const float *sub, int si, int sj, float shimmer, uint32_t frame);
/** Contour: dots on the dense side of the cell's own mid-level. */
uint8_t fluid_braille_contour(const float *sub);

/* A change of the whole picture (the boot word to the creature, the creature to a scene and back, a scene to a scene, a
 * question coming and going): everywhere at once, as dust ("A for Molty", Juan's approved prototype). Each picture is
 * read as its dust (grains of 1 or 2 x 2 px: a scene's specks, the creature's and the word's dots) and its lines (the
 * rest: a scene's drawing, the creature's own cells, letters). The old lines go out a cell at a time, scattered; the old
 * dust loses its shape: pairs of old and new grains (each picture's grains along a Hilbert curve, matched rank to rank, at
 * most FLUID_MORPH_FLY of them, spaced evenly) fly into one sparse cloud over the glass with a soft turbulence and on into
 * the new picture's dust, one path each that never stops; the old grains with no pair go out where they are, the new
 * ones with none come up on their spot (one in 8 near it, gliding in); the new lines come in over the last landings.
 * Times in seconds of the change (fluid_draw.c MP_*); its clock waits while the old picture is read (its first frames,
 * nothing sent) and slows (never a jump) before the pairs' cloud points till the new picture is read and paired (its dust
 * in: `ready`; built late, the dissolve goes a little slower from the start), standing there never longer than
 * FLUID_MORPH_HOLD_US (or _N frames): then the new picture is read as it is (its dust, if not in yet, comes after the
 * change as a change of its own: FLUID_LOOK_DUST, its grains coming up on their spots, the lines staying). Every grain
 * that moves is drawn and sent the frame
 * it moves: the glass is kept (snap), each frame the pixels it changes are composed there, and the cell rows they are
 * in are sent straight from there in windows of whole rows (fluid_draw.c morph_send). Its last frame draws the new look
 * whole where it changed since it was read. A look marked
 * FLUID_LOOK_KEEP is copied into snap as the drawer sends it; under FLUID_LOOK_GLASS the panel's own mirror keeps it (a
 * change goes from such a look, never into one: those come at once). */
#define FLUID_LOOK_KEEP       0x80000000u  /* a look's number with this: the drawer keeps the glass as it sends it      */
#define FLUID_LOOK_GLASS      0x40000000u  /* ... with this: the panel's mirror keeps it (windows on any rows)          */
#define FLUID_LOOK_DUST       0x10000000u  /* ... with this: its dust is in. Gained by a look that showed without it: the
                                            * dust comes as a change of its own (the lines stay); lost: nothing      */
/* a kept look's number: what it is (its kind, in its own bits: a scene numbered 1 is not the creature) and which one */
enum { FLUID_LOOK_CREATURE, FLUID_LOOK_SCENE, FLUID_LOOK_ASK };
static inline uint32_t fluid_look_of(int kind, uint32_t id, bool dust) {
    const uint32_t tag = kind == FLUID_LOOK_SCENE ? 0x08000000u : kind == FLUID_LOOK_ASK ? 0x20000000u : 0x04000000u;
    return FLUID_LOOK_KEEP | tag | (id & 0x03FFFFFFu) | (dust ? FLUID_LOOK_DUST : 0u);
}
#define FLUID_MORPH_FLY       600      /* pairs that fly at most (the rest of each picture fades in place; 750 till bench 21:
                                        * the movers never wait, so they are what a slow send leaves; 500 left the cloud
                                        * too empty, scene_test) ..                                                   */
#define FLUID_MORPH_FLY_PX    2000     /* .. and px of their grains (a pair of 2 x 2 px grains: 4): both fitted to the panel
                                        * model, every change's worst frame under 33 ms (scene_test.c test_morph)        */
#define FLUID_MORPH_LAND_PX   1200     /* .. and px of the new picture's grains they land on (pairs past it fade in the cloud:
                                        * a 2 x 2 px grain is 4 px to clear and draw a frame, the budget's)              */
#define FLUID_MORPH_GRAINS    16384    /* grains of a picture read at most (beyond: the new look's cells come whole at the end) */
#define FLUID_MORPH_LINES     16384    /* old lines' pixels read at most (beyond: dust)                                 */
#define FLUID_MORPH_DUST_US   150000   /* at the cloud, the new look's inputs (`ready`) awaited this long at most ..   */
#define FLUID_MORPH_DUST_N    5        /* .. or this many frames (whichever first) ..                                  */
#define FLUID_MORPH_HOLD_US   400000   /* .. and the clock stands there this long at most: then what is missing is made at once
                                        * (a scene's specks not in come after, as their own change), never a cut   */
#define FLUID_MORPH_HOLD_N    12
#define FLUID_MORPH_LINES_US  700000   /* the clock stands before the new lines come in till they are read, this long at
                                        * most, then the rest is read at once                                         */
#define FLUID_MORPH_LINES_N   20
#define FLUID_MORPH_CELLS_US  16000    /* the drawer's cells a frame in a change (us): the panel's frame is ~31.5 ms, its
                                        * send ~12. The change's own work (fluid_morph_t.budget) is what the frame's other
                                        * work before it left of this (fluid_draw_morph_spent): what moves goes first, then
                                        * the still layers' moments, a moment late when the frame has no room            */
#define FLUID_MORPH_IDLE_US   24000    /* .. and in its first frames, the old picture read, sorted, its pairs chosen: they
                                        * send nothing                                                               */
#define FLUID_MORPH_FRAME_US  28000    /* .. and its cells with its send at most this (the panel's frame is two refreshes,
                                        * 33.5 ms, from the pulse, less the handoff: bench 25 lost one at 30.2 of cells +
                                        * send, none at 29.2) ..                                                       */
#define FLUID_MORPH_SEND_US   12000    /* .. a send guessed this long at least (a whole glass), else as the last ones went */
#define FLUID_MORPH_GATE_US   1000     /* (a frame sent on a pulse, fluid_draw_morph_room: its cells end this before it, the
                                        * budget's misses and the compose after it)                                    */
#define FLUID_MORPH_PASS_WAIT_N 6      /* the drawer's pass for its new look waits this many frames at most for a scene's
                                        * inputs back (a frame of a scene with no shapes: never its new picture)        */
#define FLUID_MORPH_SPENT_US  1000     /* (the frame's other work before the change's, as the host's model counts it)    */
#define FLUID_MORPH_PASS_US   12000    /* (the drawer's one pass for the new look, as the panel model counts it)        */
#define FLUID_MORPH_READ_MIN_US 2000   /* at the cloud, the new picture read a frame at least, whatever the grains took .. */
#define FLUID_MORPH_SORT_MIN_US 2500   /* .. and its grains put in order                                               */
#define FLUID_MORPH_FINE_US   5000     /* (the dust's frame, fluid_fine_frame, as the panel model counts it)            */
/* the steps of a slice of the change's reading job (fluid_morph_t.jslow): the new grains' list, the new lines', the new
 * picture read, its grains put in order; of them, allocating */
enum { FLUID_MJ_GRAINS, FLUID_MJ_LINES, FLUID_MJ_READ, FLUID_MJ_SORT, FLUID_MJ_ALLOC, FLUID_MJ_N };
/* the parts of a change's own work in a frame (fluid_morph_t.prof, the logs' `anim wave.work`): the look's own changes
 * looked at, the old picture read, the new one read, the pairs planned and matched, the movers taken off the glass, the
 * old picture's lists turned, the gliders drawn, the new picture's lists turned, the pairs drawn (their motion,
 * turbulence, colour), the look drawn whole at the end, the rows to send found */
enum { FLUID_MP_LOOK, FLUID_MP_OLD, FLUID_MP_NEW, FLUID_MP_PAIR, FLUID_MP_FLY, FLUID_MP_SPARE, FLUID_MP_BORN, FLUID_MP_LINES,
       FLUID_MP_COMPOSE, FLUID_MP_END, FLUID_MP_MARKS, FLUID_MP_N };
/* a change's stages, for the log (fluid_morph_t.dg; mole_fluid.c "change stages"): the frame (m->frames) each was
 * reached, and what reading its new picture took */
enum { FLUID_MD_OLD, FLUID_MD_PASS, FLUID_MD_READY, FLUID_MD_DUST, FLUID_MD_PAIRED, FLUID_MD_LINES, FLUID_MD_LCELLS,
       FLUID_MD_LUS, FLUID_MD_DUS, FLUID_MD_SORTF, FLUID_MD_WAITC, FLUID_MD_WAITL, FLUID_MD_LEFT,
       FLUID_MD_JOBN, FLUID_MD_JOBUS, FLUID_MD_JOBMAX,  /* (read on the job: its slices that worked, their us in all, the longest) */
       FLUID_MD_BORN, FLUID_MD_WPX,                     /* (the most new grains with no pair moved, pixels written, in a frame) */
       FLUID_MD_SLOW,                                   /* (frames its clock went slower than a quarter frame a quarter) */
       FLUID_MD_MOVE0, FLUID_MD_SETTLE, FLUID_MD_HANG,  /* (the first frame anything moved, the dust all down, the frames
                                                         * between them nothing moved or the clock stood) */
       FLUID_MD_LIN0, FLUID_MD_LINF,                    /* (the first frame a new line showed, all of them) */
       FLUID_MD_FRAMES, FLUID_MD_N };
typedef struct fluid_morph_mem fluid_morph_mem_t;  /* (fluid_draw.c: the two pictures' grains and lines, the pairs) */
typedef struct {
    bool     on;
    bool     whole;                    /* snap holds the glass (kept since the last full send)                        */
    bool     ready;                    /* the new look's inputs are whole: its picture may be read                     */
    uint32_t look;                     /* the picture the drawer's inputs show                                         */
    uint16_t *snap;                    /* fb_w x fb_h: the glass kept; in a change, the change as shown                */
    uint8_t  *lc, *lcn;                /* N: the cells whose dots are lines (a creature's own), this frame's and the next's */
    uint8_t  *ord;                     /* N: when each cell's new grains with no pair come up, as a share of their window
                                        * (0..254, outward from where the look grows); 255: each at its own moment     */
    uint8_t  *colx, *rowy;             /* fb_w, fb_h: a pixel's cell column, row (255: none)                           */
    fluid_morph_mem_t *mm;             /* the change's pictures and pairs (PSRAM, while it runs)                        */
    int      frames;                   /* frames since it began                                                       */
    int      n, w;                     /* its clock: quarter frames of its schedule; frames of the cloud's flow       */
    float    t, dur;                   /* its time and length, s                                                      */
    int      na, nb, nlines, nfly;     /* grains of the old picture, of the new (-1: not read yet), old lines' px, pairs flying */
    int      px, wins, comp;           /* this frame: the pixels and windows sent, the pixels composed                */
    int      work;                     /* .. the change's own work (the panel model's us: reading, pairing, moving grains) */
    int      cmp_us, push_us;          /* .. the time its windows took to copy, and to hand to the panel              */
    int      prof[FLUID_MP_N];         /* .. the time of each part of its own work, us (FLUID_MP_*)                     */
    int64_t  held;                     /* when its clock first stood at the cloud (0: not yet), us                      */
    int      nheld;                    /* .. frames since                                                             */
    int      ready_at;                 /* the frame its inputs were whole (-1: not yet)                                 */
    int      cuts;                     /* changes cut at the cloud's limit (FLUID_MORPH_HOLD_*), counted for the log    */
    int      dg[FLUID_MD_N];           /* its stages (the last change's, kept after it ends; ends counted in `ends`)    */
    int      ends;
    bool     forced;                   /* the new picture read before its inputs were whole: its dust comes after      */
    bool     keep;                     /* its dust only comes (FLUID_LOOK_DUST): the lines stay as they are             */
    bool     pass;                     /* this frame the drawer made its new look (its one cells pass)                  */
    /* its new picture read on another task (fluid_draw_morph_job): the drawer and that task each own their part of it,
     * handed over only at the drawer's handoff (fluid_draw_morph_handoff) */
    bool     job;                      /* that task is there                                                           */
    bool     jhold;                    /* the change ended in the drawer's frame while that task may still read: nothing
                                        * of the look is drawn till the handoff                                       */
    bool     jhurry;                   /* a wait at its limit: that task reads the rest at once                         */
    bool     still;                    /* (at the handoff: the drawer has made its new look, fluid_draw_morph_still)    */
    bool     seen_hd; uint8_t pwait;   /* (its new look's inputs were a scene's in a frame of it; frames its pass waited for
                                        * such inputs again: FLUID_MORPH_PASS_WAIT_N at most)                         */
    bool     full;                     /* a change cut: the next send is whole (its glass may hold what was not sent)   */
    uint8_t  fulls;                    /* sends in a row that were whole (a drawer that repaints all every frame: many)  */
    int      spent, budget;            /* the drawer's frame before the change's work (us), and so the change's this frame */
    int      room;                     /* (told each frame) its cells' time till the send's pulse, us; 0: none (FRAME_US) */
    int      sendp;                    /* the send a frame of it is likely to take (us: the last, its peaks held a while) */
    int      jover;                    /* (tests) the most the job's slices went past their budget, in the model's work  */
    int      jslow[2 + FLUID_MJ_N];    /* (the job's, for the log "change job") its longest slice this change: us, its budget
                                        * (-1: none), its us by step MJ_* (grains' list, lines' list, read, sort, of
                                        * them allocating)                                                            */
    int64_t  c0;                       /* (this frame's cells began, us)                                              */
    int      top[3][6];                /* its three moving frames with the longest cells (the log): frame, cells us (with
                                        * the poll), send us (with the wait for its pulse), its biggest part (FLUID_MP_*),
                                        * the movers' us, the still layers' us                                        */
    const char *bound; int bound_n;    /* (the readers', for the log) a count found past what the glass can hold: which, its
                                        * value (refused: the change is cut, never an allocation from it)              */
    int      pace[5];                  /* (fluid_task's alone, for the log "change pace": refreshes lost at its pace while
                                        * a change ran; for the latest, how late it was there, its job's room, the drawer's
                                        * lateness at the handoff before; the drawer's most, us)                        */
    fluid_morph_mem_t *jmm;            /* the change that task reads for (set and cleared at the handoff only)          */
    fluid_morph_mem_t *jfree;          /* changes ended in the drawer's frame while that task had them: freed at the handoff */
} fluid_morph_t;
/** What a change shows now (tests, logs): the pairs waiting, flying, landed; the grains with no pair shown (old, new);
 *  the old lines' and the new lines' pixels lit. */
typedef struct { int wait, fly, landed, spare, born, born_landed, old_lines, new_lines;
                 uint32_t read; } fluid_morph_census_t;   /* (read: a hash of the new picture as read, once whole; else 0) */

/* animations the drawer tells apart (fluid_draw_t.anim_*): each frame, which had cells change, and which of those
 * had any change left unsent (none may) */
enum { FLUID_ANIM_WAVE, FLUID_ANIM_GROW, FLUID_ANIM_WORD, FLUID_ANIM_WATER, FLUID_ANIM_N };

typedef struct {
    int W, H;
    int fb_w, fb_h;                    /* framebuffer geometry (row stride = fb_w)        */
    int stage, ox, oy;                 /* the square stage inside it, px                  */
    int dot_w, dot_h;
    int16_t *cx;                       /* W+1 cell x edges (fb px)                        */
    int16_t *cy;                       /* H+1 cell y edges                                */
    int16_t *dx;                       /* 2·W dot left x                                  */
    int16_t *dy;                       /* 4·H dot top y                                   */
    uint16_t *key, *col;               /* per-cell last-drawn (dots|kind, rgb565)         */
    uint8_t  *skip;
    uint8_t  *dirty;                   /* per cell: changed this frame                    */
    int16_t  *span;                    /* per row: first, last+1 changed cell (none: 0, 0) */
    int8_t   *yrow;                    /* fb row → dot row 0..3 inside its cell, or −1    */
    uint8_t  *hds;                     /* per cell: its scene shapes' signature (redraw on change) */
    uint8_t  *hdw;                     /* fb_w: a pixel row's high-definition weight 0..8   */
    uint8_t  *halo, *halo1;            /* 2W x 4H: each dot's distance to a scene's drawing, in
                                        * dots (0 on it, HALO_REACH + 1 and beyond: far); and
                                        * the next one, measured when the drawing changes  */
    uint32_t halo_sig;                 /* the drawing it was measured from (a hash)        */
    uint32_t lt_sig[FLUID_MAX_LETTERS]; /* each letter last drawn (a hash): its cells are sent again only when it changes */
    uint8_t  *hde;                     /* per cell: the dust's reseed it shows (still cells) */
    uint8_t  *hch;                     /* per cell: its halo or the subtitle's dark changed */
    uint8_t  *cdist;                   /* per cell: cells to the nearest with a drawing's dots */
    int8_t   *scat;                    /* 2W x 4H x 3: each dot's fixed scatter as dust (x, y in
                                        * -127..127 of the radius) and when it thins (0..255) */
    uint8_t  *pdots;                   /* a transition's dust: the dots each cell shows ...   */
    int16_t  *psrc;                    /* ... and the cell the first came from (its colour)   */
    uint16_t *cd0;                     /* crisp dots (o->scene, no shapes): per cell its dots | 0x100 when the
                                        * wave left them in place, and its colour's bytes, as last looked at */
    uint32_t *csig;
    bool     crisp0;                   /* the last frame was crisp dots: those are what the cells show */
    uint8_t  ilace;                    /* the frame's count: which rows' water-only cells update this frame (crisp
                                        * dots: every other row)                                            */
    uint8_t  *why;                     /* N: this frame's animations that changed the cell (1 << FLUID_ANIM_*)    */
    uint8_t  anim_on, anim_late;       /* this frame: the animations that changed cells, those with a change unsent */
    int16_t  cap[4], cap0[4];          /* the subtitle's box in dots [x0, x1] x [y0, y1], last frame's */
    int16_t  sb[FLUID_MAX_SHAPES][4];  /* this frame's shapes' boxes, fb px [x0, x1) x [y0, y1) */
    int16_t  sb0[FLUID_MAX_SHAPES][4]; /* ...the last frame's, and each one's hash: a shape that  */
    uint32_t sh0[FLUID_MAX_SHAPES];    /* changes redraws the cells it covered and covers  */
    fluid_shape_t *shp0;               /* ...and the shapes themselves (a stroke redraws its own cells only) */
    uint32_t *sh1;                     /* this frame's hashes (compared with sh0 as lists: some may come or go) */
    fluid_shape_t *near;               /* scratch: the shapes that reach one cell of the drawing's cache      */
    uint16_t *hdc;                     /* a scene on fine dust: its drawing (but the caption) drawn once, fb_w x fb_h (PSRAM) .. */
    uint8_t  *hdcv;                    /* .. each cell's part of it valid (a changed shape's cells drawn again when shown):
                                        * 1 black, 2 drawn                                                            */
    uint8_t  *owe;                     /* N: over a kept glass, a changed cell whose cache had no room in its frame: drawn
                                        * (and sent) the next (fluid_draw_send2)                                        */
    int      hdc_next, nbuilt, nowed;   /* the cache made ahead from this cell on; cells made, owed this frame (the log) */
    int      ndrawn;                   /* this frame: px of changed cells drawn into the kept glass (the bench)        */
    int      move_us, cache_us, draw_us;   /* this frame: the dust moved alone, the drawing's cache made, the changed cells drawn
                                           * into the glass (the three parts of cmp; the bench) */
    int      nsh0;
    struct {                           /* this frame's condensed letters, ready to draw   */
        const uint8_t *gl, *mask; uint16_t c, mw, mh; int16_t x, y, w, h, cx0, cy0, cx1, cy1;
    } lt[FLUID_MAX_LETTERS];
    int nlt;
    void *block;
    bool force;                        /* redraw every cell next frame                    */
    fluid_morph_t m;                   /* the whole picture changing, as dust (fluid_draw_morph_look) */
    /* EXPERIMENT scene dust (fluid_fine_dust_kind 1; delete with it): a scene on fine dust shows the creature's kind of
     * dust instead, braille dots on the cells riding the water (fluid_draw.c: sd_field) */
    uint16_t *sd_dist;                 /* 2W x 4H: each dot's distance to the drawing, 1/4 px                    */
    uint8_t  *sd_dots;                 /* N: the dust's dots of each cell, calm                                   */
    uint16_t *sd_col;                  /* N: their colour                                                         */
    uint8_t  *sd_near;                 /* N: how near the drawing the cell's nearest dust dot is (0..255)          */
    uint8_t  *sd_live;                 /* N: the dots the light on the water shows this frame (sd_alive) ..       */
    uint8_t  *sd_ch;                   /* N: only that changed this frame (it waits for its row's turn)           */
    uint8_t  *sd_pm;                   /* N: a word's specks or its forming in the cell, this frame (1), the last (2) */
    uint16_t *sd_lcol;                 /* N: .. and their colour, twinkling                                       */
    uint8_t   sd_m;                    /* the water's rows take turns over this many frames (2..SD_ILACE_MAX) ..   */
    int       sd_w, sd_o, sd_c;        /* .. after this frame's cells only the water changed and were sent, the others sent, and
                                        * the cells in their rows' turn the water moved or left                   */
    float     sd_a;                    /* of those, how many changed (the last frame's)                           */
    float     sd_t, sd_t0;             /* the dust's clock (it stands while the picture changes), the frame's time */
    uint32_t  sd_sig;                  /* the drawing they were made from (a hash)                                */
    int8_t    sd_kind;                 /* the kind the last frame showed (a change redraws the glass)             */
    uint8_t  *sd_hard;                 /* N (kind 3): the hard dots among the cell's dots as drawn, on black only ..  */
    uint16_t *sd_hcol;                 /* N: .. and their colour                                                  */
    int16_t   hold[4];                 /* cells [i0, i1] x [j0, j1] the water never breaks: the caption and its backdrop */
} fluid_draw_t;

/** A scene on fine dust: where the water shifts the dots (wave slope x 3.3, in sub-dots: half a dot across, a quarter
 *  down) by this much or more, its drawing breaks into its dots and they ride the water ("lines wobble with water");
 *  below it the drawing is whole. A band about the crest of a tap's ring, thinner than where the water moves dots at
 *  all: its cells are sent every frame, so its width is the cost (1.5: the open chart's middle tap over a frame with
 *  the specks; 1.75 fits, ~160 px of reach). */
#define FINE_BREAK 1.75f

/** A window of pixels to draw: px is pixel (x0, y0), rows `stride` px apart. A whole
 *  framebuffer is { fb, fb_w, 0, 0, fb_w, fb_h }. */
typedef struct { uint16_t *px; int stride, x0, y0, x1, y1; } fluid_rect_t;

typedef struct {
    int   palette, edge_mode;
    float shimmer, time, melt;
    const fluid_letter_t *letters; int nletters;
    const char *ramp;                  /* ASCII ramp, dark → bright                       */
    const uint8_t *(*font)(char);      /* 5×7 rows, bit (4-col); NULL → no ASCII/letters  */
    const uint8_t *dots;               /* W×H braille dots drawn wherever the fluid's ink
                                        * reaches them (fluid_text_set_glyphs), or NULL   */
    bool  scene;                       /* the dots are a scene's, in high definition: drawn
                                        * alone and crisp on black (no fluid), its letters
                                        * whole; a steep wave (a touch) scatters them into
                                        * dust, calm water puts them back on the lattice   */
    float dust;                        /* scene: its transition, 0 = whole .. 1 = dust, gone */
    const fluid_shape_t *shapes;       /* scene: it in high definition (NULL: on the dots): calm,
                                        * these shapes at the panel's pixels instead of its dots
                                        * (and its letters, which are among them); a steep wave
                                        * or a transition fades them into the dust            */
    int nshapes;
    const uint8_t *kdots;              /* scene in high definition: the dots it keeps calm too
                                        * (a ball, an image: no shape draws them), or NULL   */
    uint32_t frame;                    /* crisp dots: FLUID_FRAME(number, m), which picks the cell rows the water's
                                        * own changes go in (the dots' maker interlaces by the same; 0: the drawer's own) */
    /* a scene in high definition on fine dust (fluid_fine.h) instead of the braille haze: */
    struct fluid_fine *fine;           /* its dust, or NULL (the drawer's per-frame part is written by fluid_draw_cells) */
    const uint8_t *still;              /* W x H: cells the water never breaks (a tapped spot's drawing), or NULL */
    unsigned cap_word;                 /* the caption's word: a new number each word, 0 none (it dissolves)   */
    float cap_unformed;                /* how much of it has not formed from the dust yet, 0 (whole) .. 1    */
    bool  haze_live;                   /* the braille haze lives as the page's dust does (a heading on the website,
                                        * wm_haze; never the device): each of its dots reseeded at its own moment ..  */
    float haze_water[2];               /* .. and the water moves its dots, its slope taken these many times across and
                                        * down (wm_haze_water: as far as the page's dust moves; 0: still)          */
} fluid_draw_opts_t;

/* crisp dots' interlace: frame n of the dots' maker, its water's rows taking turns over m frames (2..9) */
#define FLUID_FRAME(n, m) (0x80000000u | (uint32_t)((m) - 2) << 28 | ((uint32_t)(n) & 0x0FFFFFFFu))
/* whether cell row j takes the water's changes in that frame */
static inline bool fluid_ilace_row(uint32_t frame, int j) {
    const uint32_t m = (frame >> 28 & 7u) + 2u;
    return (uint32_t)j % m == (frame & 0x0FFFFFFFu) % m;
}

/** W×H cells drawn into an fb_w×fb_h framebuffer; the stage is its largest centred
 *  square (the whole framebuffer on a square panel). */
fluid_draw_t *fluid_draw_create(int W, int H, int fb_w, int fb_h);
void fluid_draw_destroy(fluid_draw_t *d);
/** Draw into the fb_w×fb_h rgb565-BE framebuffer, touching only cells that changed.
 *  Returns cells drawn; *y0,*y1 = dirty row band ([0,0) when nothing changed).
 *  = fluid_draw_cells + fluid_draw_rect(whole fb, all = false). */
int  fluid_draw(fluid_draw_t *d, const fluid_frame_t *f, uint16_t *fb,
                const fluid_draw_opts_t *o, int *y0, int *y1);
/** What each cell looks like this frame, which changed (d->dirty, d->span) and the
 *  condensed letters. Returns the cells and letters to draw. */
int  fluid_draw_cells(fluid_draw_t *d, const fluid_frame_t *f, const fluid_draw_opts_t *o);
/** Draw the cells (the changed ones, or all = every one, over a cleared window) and the
 *  letters inside t, from the last fluid_draw_cells. With all, t gets exactly what a
 *  framebuffer kept by fluid_draw holds there: a panel can be sent just the windows that
 *  changed, drawn in a small buffer, with no framebuffer behind them. */
void fluid_draw_rect(const fluid_draw_t *d, const fluid_draw_opts_t *o, const fluid_rect_t *t, bool all);

/** A small status mark at the bottom of the stage, just inside the round panel's edge,
 *  in the palette's colours, drawn over the fluid (the caller repaints everything once
 *  when the mark changes or goes; fluid_draw only redraws cells that changed). ARC: a
 *  short dim arc · DOT: a dim dot · ALERT: a dot in the accent colour. */
enum { FLUID_MARK_NONE, FLUID_MARK_ARC, FLUID_MARK_DOT, FLUID_MARK_ALERT, FLUID_MARK_COUNT };
/** Returns the pixels written (0 for FLUID_MARK_NONE). */
int  fluid_draw_mark(const fluid_draw_t *d, const fluid_rect_t *t, int palette, int mark);

/** The palette's accent colour, 0xRRGGBB (the listening ring's default). */
uint32_t fluid_palette_accent(int palette);
/** The listening ring's colour (0xRRGGBB): the listen's own `given` when ≥ 0, else the
 *  device default `dflt` when ≥ 0 (the console's setting), else the palette's accent. */
uint32_t fluid_ring_rgb(int32_t given, int32_t dflt, int palette);

/** Pixel overlays drawn over the fluid (after fluid_draw, before the mark), blended:
 *  dividers a question's faint sector dividers (fluid_ask.h): sectors ≥ 2 rays from the
 *           centre at 12 o'clock + k·360/sectors; divider = their opacity 0..1 (0 = none)
 *  (The listening ring and the buttons' borders are the fluid itself: fluid_target_arc.)
 *  Blending reads the pixel under it, so it must be a freshly drawn one: before every
 *  fluid_draw call fluid_draw_overlay_dirty with the overlay last drawn AND the one about
 *  to be drawn — the cells under them are redrawn (restored), nothing else is. */
typedef struct { int sectors; float divider; } fluid_overlay_t;
static inline bool fluid_overlay_on(const fluid_overlay_t *ov) {
    return ov->sectors >= 2 && ov->divider > 0.f;
}
/** Mark the cells under ov for redraw by the next fluid_draw. */
void fluid_draw_overlay_dirty(fluid_draw_t *d, const fluid_overlay_t *ov);
/** The picture changing whole (fluid_morph_t). With the drawer idle, once a frame, before its inputs are written: look,
 *  the picture those inputs show from now (a number of its own: a scene's serial, a creature, a question; with
 *  FLUID_LOOK_KEEP the drawer keeps the glass as it sends it, with FLUID_LOOK_GLASS the panel's mirror does; 0: not
 *  known, it comes at once), snap, fb_w x fb_h px the glass is kept in (the panel's mirror; NULL: it comes at once),
 *  ready, the inputs are whole (a scene's dust is in: until then, at most FLUID_MORPH_DUST_US, the cloud waits).
 *  A look that differs only by FLUID_LOOK_DUST changes nothing while a change runs; gained afterwards, its dust comes.
 *  A new look starts a change; the inputs are the new look's from the same frame. */
void fluid_draw_morph_look(fluid_draw_t *d, uint32_t look, uint16_t *snap, bool ready);
/** With the drawer idle, once a frame with the inputs: the cells (N) whose dots are the look's lines, not its dust (a
 *  creature's own cells; copied), or NULL: none. */
void fluid_draw_morph_lines(fluid_draw_t *d, const uint8_t *cells);
/** With the drawer idle, once a frame with the inputs: per cell (N) when its new grains with no pair come up in a change
 *  into this look, as a share of their window, 0..254 (a look that grows outward from somewhere: a question's answers),
 *  255 each at its own moment; copied. NULL: all at their own moments. */
void fluid_draw_morph_order(fluid_draw_t *d, const uint8_t *order);
/** A new look given now would come as a change of picture that runs to its end: a glass kept to change from, and the
 *  drawer sending what changes (not repainting all of it every frame, which ends a change at its first send). */
bool fluid_draw_morph_carries(const fluid_draw_t *d);
static inline int fluid_draw_morphing(const fluid_draw_t *d) { return d && (d->m.on || d->m.jhold); }
/** A change runs and the drawer has made its new look: it stands (its inputs are not read) till the change ends. */
bool fluid_draw_morph_still(const fluid_draw_t *d);
/** (tests) The waits' limits in frames: at the cloud, before the new lines; and the new lines' cells read a frame at
 *  most (the device's slow reads; < 0: all of the new picture read at the first chance, by either reader); 0: as built. */
void fluid_draw_morph_bounds(int cloud_n, int lines_n, int read_cells);
/** (tests) The new picture's lists made `rows` cell rows a call at most (each pass's end a call's end too); 0: by budget. */
void fluid_draw_morph_walk_rows(int rows);
/** (tests) The panel model's costs of a change's own work this many times as built (the device's, ~2: bench10); 0: as built. */
void fluid_draw_morph_costs(float k);
/** A change runs and nothing of its new look's dust is read yet: its inputs may still change whole (a scene's dust
 *  swapping in). With the drawer idle. */
bool fluid_draw_morph_open(const fluid_draw_t *d);
/** The new picture of a change read on another task (on: the device's sim task, which then calls fluid_draw_morph_work
 *  between its handoffs and fluid_draw_morph_handoff at each); off (as created): in the drawer's own frames, the same code
 *  (the web, the host tests' in-line runs). */
void fluid_draw_morph_job(fluid_draw_t *d, bool on);
/** That task's share, while the drawer draws: the new picture read and put in order, budget_us at most (one cell row, a
 *  sort's chunk or the dust's frame past it at worst), with the inputs of the last handoff (s, o: the drawer's). */
void fluid_draw_morph_work(fluid_draw_t *d, const fluid_frame_t *s, const fluid_draw_opts_t *o, int budget_us);
/** At each handoff, with the drawer idle and that task between slices, after fluid_draw_morph_look and _lines: what it
 *  read becomes the drawer's, a change whose new look is made goes to it to read, changes ended meanwhile are freed. */
void fluid_draw_morph_handoff(fluid_draw_t *d);
/** The drawer, each frame before fluid_draw_cells: its frame's time so far (us; the touch poll), the change's budget the rest
 *  of FLUID_MORPH_CELLS_US. Not told: FLUID_MORPH_SPENT_US. */
void fluid_draw_morph_spent(fluid_draw_t *d, int us);
/** The drawer, after fluid_draw_morph_spent: a change's frame is sent on the panel's next tearing pulse, in us from now
 *  (0: no pulse to wait for: its cells and send share FLUID_MORPH_FRAME_US). Its cells then have that, less
 *  FLUID_MORPH_GATE_US. */
void fluid_draw_morph_room(fluid_draw_t *d, int us);
/** The window t as the change shows it this frame, composed whole (the glass holds the same: tests). Returns its pixels. */
int  fluid_draw_morph_rect(const fluid_draw_t *d, const fluid_rect_t *t);
void fluid_draw_morph_census(const fluid_draw_t *d, fluid_morph_census_t *c);
/** Draw ov over the window t. Returns the pixels touched. */
int  fluid_draw_overlay(const fluid_draw_t *d, const fluid_rect_t *t, int palette, const fluid_overlay_t *ov);

/** Send a panel what changed since the last frame sent (everything when full), with no
 *  framebuffer: each changed row of cells (x and width rounded out to multiples of 4, as
 *  panels such as the SPD2010 take windows) is drawn whole into buf (max_px px; taller
 *  rows go in parts): cells, letters, the overlay ov and the mark, then handed to send
 *  (non-zero: stop). Returns the px sent, or -1. Call after fluid_draw_cells. */
typedef int (*fluid_send_fn)(void *ctx, int x, int y, int w, int h, const uint16_t *px);
int  fluid_draw_send(const fluid_draw_t *d, const fluid_draw_opts_t *o, const fluid_overlay_t *ov, int mark, bool full,
                     uint16_t *buf, int max_px, fluid_send_fn send, void *ctx);
/** The same with two buffers (buf2 non-NULL, each max_px): send may return while the panel still reads the buffer it
 *  was given (an asynchronous transfer) as long as it waits for that one before taking the other; each window is drawn
 *  into the buffer the panel is not reading. The caller waits for the last send after. */
int  fluid_draw_send2(const fluid_draw_t *d, const fluid_draw_opts_t *o, const fluid_overlay_t *ov, int mark, bool full,
                      uint16_t *buf, uint16_t *buf2, int max_px, fluid_send_fn send, void *ctx);
/** A scene on fine dust over its kept glass (fluid_draw_send2) hands its windows to fn instead of sending them: rows
 *  [y, y + h) x [x, x + w) of glass (stride px a row) to copy and draw the mark over (fluid_draw_mark: mark, palette), or
 *  whole rows straight from it (direct), in this order. The glass's rows handed over are not drawn again in the frame, so
 *  another task may send them while the drawer draws on; the caller waits for all of them before the drawer's next
 *  frame and before any other send. Non-zero: not taken (the frame stops). fn NULL: sent here, as before. */
typedef int (*fluid_glass_fn)(void *ctx, const uint16_t *glass, int stride, int x, int y, int w, int h, bool direct, int mark, int palette);
void fluid_draw_glass_sender(fluid_glass_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif
