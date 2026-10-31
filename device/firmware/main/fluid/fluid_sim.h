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
#define FLUID_MAX_LETTERS 40       /* a question's 5 labels × 8 letters             */
#define FLUID_WORD_LETTERS 32      /* the speed reader's display cap per word        */

/* ── profiling: CPU cycles per phase, accumulated until the caller resets ── */
enum { FP_TARGET, FP_BG, FP_SWIRL, FP_WAVE, FP_DEMAND, FP_FLOW, FP_ADVECT, FP_RELAX, FP_COLOR, FP_VIEW,
       FP_C_UPDATE, FP_C_RASTER, FP_C_CLEAR, FP_C_VERT, FP_C_SAMPLES, FP_C_AGG, FP_C_FILL, FP_DSAMPLE, FP_DRASTER, FP_N };
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

/* ── sim ──────────────────────────────────────────────────────────────────── */

typedef struct {
    float c2, damp, leak;              /* wave speed², velocity damping, height leak     */
    float attract, drag, vmax;         /* demand-flow gain, drag (1/s), speed cap (c/s)  */
    float relax_up, relax_down, tint_rate;
    float swirl, swirl_freq;           /* ambient eddies                                 */
    float refr, crest;                 /* ripple refraction + crest highlight            */
    float speed_mul, hold;
    /* the dim background fluid: an ink floor in EVERY state */
    float bg_level, bg_freq, bg_drift, bg_speed, bg_rim, bg_mob;
    float bg_color[3];
    float tilt_x, tilt_y;              /* gravity bias on the flow, cells/s² (IMU)        */
} fluid_params_t;

typedef struct { float amp, freq, x, y, phase; } fluid_osc_t;

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
    float x, y, w, h, fs;              /* stage px                                        */
    float melt;                        /* this letter's own melt, added to the frame's    */
    uint32_t rgb;                      /* FLUID_RGB | 0xRRGGBB: its own colour (pastel);
                                        * 0 = the palette's                               */
} fluid_letter_t;
#define FLUID_RGB 0x1000000u           /* "a colour is set" flag over 0xRRGGBB            */

typedef struct {
    float *cov, *orp, *near, *near_s, *dark, *tmp;
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
    int8_t   *yrow;                    /* fb row → dot row 0..3 inside its cell, or −1    */
    uint8_t  rdirty[FLUID_GRID_MAX_W]; /* per cell of the row being drawn               */
    void *block;
    bool force;                        /* redraw every cell next frame                    */
} fluid_draw_t;

typedef struct {
    int   palette, edge_mode;
    float shimmer, time, melt;
    const fluid_letter_t *letters; int nletters;
    const char *ramp;                  /* ASCII ramp, dark → bright                       */
    const uint8_t *(*font)(char);      /* 5×7 rows, bit (4-col); NULL → no ASCII/letters  */
} fluid_draw_opts_t;

/** W×H cells drawn into an fb_w×fb_h framebuffer; the stage is its largest centred
 *  square (the whole framebuffer on a square panel). */
fluid_draw_t *fluid_draw_create(int W, int H, int fb_w, int fb_h);
void fluid_draw_destroy(fluid_draw_t *d);
/** Draw into the fb_w×fb_h rgb565-BE framebuffer, touching only cells that changed.
 *  Returns cells drawn; *y0,*y1 = dirty row band ([0,0) when nothing changed). */
int  fluid_draw(fluid_draw_t *d, const fluid_frame_t *f, uint16_t *fb,
                const fluid_draw_opts_t *o, int *y0, int *y1);

/** A small status mark at the bottom of the stage, just inside the round panel's edge,
 *  in the palette's colours, drawn over the fluid (the caller repaints everything once
 *  when the mark changes or goes; fluid_draw only redraws cells that changed). ARC: a
 *  short dim arc · DOT: a dim dot · ALERT: a dot in the accent colour. */
enum { FLUID_MARK_NONE, FLUID_MARK_ARC, FLUID_MARK_DOT, FLUID_MARK_ALERT, FLUID_MARK_COUNT };
/** Returns the pixels written (0 for FLUID_MARK_NONE). */
int  fluid_draw_mark(const fluid_draw_t *d, uint16_t *fb, int palette, int mark);

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
/** Draw ov over the framebuffer. Returns the pixels touched. */
int  fluid_draw_overlay(const fluid_draw_t *d, uint16_t *fb, int palette, const fluid_overlay_t *ov);

#ifdef __cplusplus
}
#endif
