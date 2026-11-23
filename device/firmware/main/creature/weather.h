/*
 * SPDX-License-Identifier: MIT
 *
 * weather.h: the weather on the glass behind the creature (docs/CREATURES.md "The weather"): rain falling behind the
 * cat, each drop that lands a soft bump in the real water with a ring on the floor, drops on the glass itself, and the
 * floor wet afterwards, reflecting the cat and the moon until it dries in patches. Like the sky (sky.h) it is light in
 * the creature's world: cells of braille dots that creature_dots.c merges after its own (cd_weather), so a change of
 * picture takes it with the creature's picture and the touch's wave moves it as the world's dust.
 *
 * It is told, never guessed: a kind (clear, drizzle, rain, downpour) and how long it holds (weather_tell). When that
 * runs out it eases back to clear by itself. Nothing told: clear, nothing made or drawn, no random number drawn.
 *
 *   level    0 clear, 1 drizzle, 2 rain, 3 downpour; it moves toward the kind told at WX_EASE_S a step, so rain starts
 *            and ends through drizzle and nothing pops. Every rate and look is the level's, between the kinds' rows.
 *   streaks  dotted lines on the dot lattice falling at the kind's slant, near ones long, fast and bright, far ones
 *            short, slow and dim; the near ones land on the floor (a bump, a splash, a ring), the far ones end in the
 *            distance. Behind the cat; in a downpour a few also in front of it (thinner and dimmer across its body,
 *            never on its lines or its face).
 *   rings    a thin oval growing from where a drop landed, fading in half a second.
 *   glass    a few drops sitting on the glass, now and then one running down with a trail (in front, off the face).
 *   moonlit  at drizzle and rain with the moon up, the rain catches the moon's light: weather_frame's moon_lit (the
 *            moon's lit fraction, 0 when it is not shown) blends it in from WX_MOONLIT_LO to WX_MOONLIT_HI lit; at a thin
 *            or new moon, or with no sky, plain rain.
 *   wet      the floor wets over WX_WET_S of rain and dries over WX_DRY_S after it, in patches (a fixed puddle map: the
 *            low places wet first and dry last); creature_dots.c darkens it there and mirrors the cat and the moon in it.
 *   shake    in a full downpour the cat shakes itself off now and then (weather_t.shake; the caller plays it when the
 *            cat is free).
 * Plain C, no ESP-IDF, no maths library in a frame (host-tested: test/host/weather_test.c).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "sky.h"                            /* sky_cell_t; where the moon sits */

#ifdef __cplusplus
extern "C" {
#endif

enum { WX_CLEAR, WX_DRIZZLE, WX_RAIN, WX_DOWNPOUR, WX_KINDS };
extern const char *const WX_NAMES[WX_KINDS];

#define WX_LIFE_DEF_S   3600.f              /* told without a lifetime: an hour                                         */
#define WX_LIFE_MIN_S   10.f                /* a lifetime is held to these                                              */
#define WX_LIFE_MAX_S   21600.f
#define WX_EASE_S       6.f                 /* s a step of level (clear to rain: 12 s, through drizzle)                   */
#define WX_WET_S        90.f                /* s of rain (at drizzle or more) to wet the floor through                   */
#define WX_DRY_S        900.f               /* s for a wet floor to dry once the rain stopped                            */
#define WX_MOONLIT_LO   0.35f               /* the moon's lit fraction where the moonlit rain starts to blend in ...      */
#define WX_MOONLIT_HI   0.85f               /* ... and is whole                                                          */
#define WX_SHAKE_FIRST_MIN_S 25.f           /* a full downpour: the first shake after this much ...                      */
#define WX_SHAKE_FIRST_MAX_S 60.f
#define WX_SHAKE_GAP_MIN_S   180.f          /* ... then one every this much                                              */
#define WX_SHAKE_GAP_MAX_S   420.f

#define WX_STREAK_EVERY 3                   /* a streak moves every this many frames, the streaks taking turns: the cells
                                             * that change a frame (what the panel is sent) halved, the rain as fast      */
#define WX_STREAKS      144                 /* the falling streaks at once, at most (a downpour has ~90)                   */
#define WX_RINGS        48                  /* the rings spreading at once                                                */
#define WX_BEADS        16                  /* the drops on the glass                                                     */
#define WX_BUMPS        3                   /* bumps in the water a frame, at most                                        */
#define WX_BACK_CELLS   640                 /* cells a frame behind the cat (streaks, splashes, rings)                    */
#define WX_FRONT_CELLS  160                 /* ... in front of it (a downpour's few streaks)                              */
#define WX_GLASS_CELLS  128                 /* ... on the glass                                                           */
#define WX_GRID_MAX     (64 * 40)           /* the largest grid (fluid_sim.h FLUID_GRID_MAX_W x _H)                       */

/** A bump for the water: glass units (-1..1, y down), as fluid_sim_impulse takes it. */
typedef struct { float x, y, strength, radius; } wx_bump_t;

typedef struct {
    float x0, y0, ux, uy;                   /* where its head started (stage 0..1) and its way down                       */
    float v, len, b, z;                     /* speed (stage/s), length (stage), brightness, depth (0 far .. 1 near)        */
    float t, T;                             /* its age and when it ends (s)                                               */
    float td;                               /* its age where it is drawn (it moves every WX_STREAK_EVERY frames)          */
    uint8_t lands, front, bump, ph;         /* it lands on the floor; falls in front of the cat; its landing bumps; its turn */
} wx_streak_t;
typedef struct { float x, y, z, age, ad; uint8_t ph, splash; } wx_ring_t;   /* ad: its age where drawn (in turns, as a streak's) */
typedef struct {
    float x, y, age, life;                  /* where (stage), its age and how long it stays (s)                           */
    float y0, vy;                           /* a runner: where it started, its speed down (stage/s); vy 0: it sits         */
    uint8_t size;                           /* 0 a speck, 1 a drop, 2 a big drop                                          */
} wx_bead_t;

typedef struct weather {
    bool on;                                /* the setting (weather_set): off, nothing                                    */
    int kind;                               /* what was told (WX_CLEAR when nothing is or it ran out)                      */
    float left;                             /* s it still holds                                                           */
    float level;                            /* 0 .. 3, eased toward kind                                                   */
    float wet;                              /* the floor, 0 dry .. 1 wet through                                          */
    float moonlit;                          /* the moonlit look's share this frame, 0..1                                   */
    uint32_t rng;                           /* xorshift32 (the weather's own)                                             */
    float due_land, due_far, due_front, due_bead, runner_wait;
    uint32_t frame;                         /* frames made (whose turn a streak moves)                                    */
    float shake_wait;                       /* s of full downpour till the next shake                                     */
    bool shake;                             /* a shake is due (weather_shook once played)                                 */
    int W, H;                               /* the grid its puddles were made for                                         */
    int nstreak, nring, nbead;
    wx_streak_t streak[WX_STREAKS];
    wx_ring_t ring[WX_RINGS];
    wx_bead_t bead[WX_BEADS];
    int nbump;                              /* this frame's bumps for the water                                           */
    wx_bump_t bump[WX_BUMPS];
    int nback, nfront, nglass;              /* this frame's cells                                                         */
    sky_cell_t back_c[WX_BACK_CELLS], front_c[WX_FRONT_CELLS], glass_c[WX_GLASS_CELLS];
    uint8_t puddle[WX_GRID_MAX];            /* per cell: how low it lies, 0..255 (wets first, dries last)                   */
    uint16_t at[WX_GRID_MAX];               /* (scratch: a cell's place in the list being made, + 1)                       */
    uint8_t lvl[WX_GRID_MAX];               /* (scratch: the brightest dot's level in a cell being made)                   */
    bool show;                              /* the last frame drew anything (rain, glass, or a wet floor)                  */
} weather_t;

/** A weather, its randomness seeded: on, clear, dry. */
void weather_init(weather_t *w, uint32_t seed);
/** The setting: off, nothing at once (clear, dry, no drops); on again: clear until told. */
void weather_set(weather_t *w, bool on);
/** Told: kind (WX_*) for life s (<= 0: WX_LIFE_DEF_S; held to WX_LIFE_MIN_S..MAX_S); WX_CLEAR ends what holds. It eases
 *  there from where it is. */
void weather_tell(weather_t *w, int kind, float life);
/** The kind by name ("clear", "drizzle", "rain", "downpour"); -1 none. */
int  weather_kind(const char *name);
/** Once a frame: the grid, dt (s since the last frame: the time the screen was off counts, the lifetime and the drying
 *  go by the clock); show: the creature is on the glass with nothing over it (else nothing is made, the clocks run);
 *  moon_lit: the moon's lit fraction when it shows (0: none). Makes this frame's cells and bumps. */
void weather_frame(weather_t *w, int W, int H, float dt, bool show, float moon_lit);
/** Anything to merge this frame (cells or a wet floor): false, the world as without weather. */
static inline bool weather_drawn(const weather_t *w) { return w && w->show; }
/** Cell k's wetness as drawn, in quarter steps 0..4 (0 dry): the floor darker there and the mirror in it. A cell is wet
 *  once the floor's wetness passes its own mark (the low places first), a quarter step at a time: the floor changes a
 *  few cells a frame, never all of it at once. (Inline: creature_dots.c builds without weather.c in some host tests.) */
static inline int weather_wet_at(const weather_t *w, int k) {
    if (!w || w->wet <= 0.f || k < 0 || k >= WX_GRID_MAX) return 0;   /* (wet through, every cell is: a puddle's high places dry first) */
    const float p = (float)w->puddle[k] * (1.f / 255.f), v = (w->wet - (1.f - p) * 0.85f) / 0.15f;
    return v <= 0.f ? 0 : v >= 1.f ? 4 : (int)(v * 4.f + 0.5f);
}
/** The shake was played (or given up): the next one a while later. */
void weather_shook(weather_t *w);

#ifdef __cplusplus
}
#endif
