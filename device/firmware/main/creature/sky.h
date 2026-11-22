/*
 * SPDX-License-Identifier: MIT
 *
 * sky.h: the sky behind the creature (docs/CREATURES.md "The sky"): today's moon, small, up and to the right of the
 * cat, lit to its real phase and sized by its real distance; and now and then a falling star. Both are light in the
 * creature's world: braille dots on its cells (creature_dots.c cd_sky merges them), so the creature is always in front
 * (a cell with its dots keeps only them, its halo keeps the sky off it), the touch's wave moves them as it moves the
 * world's dust, and a change of picture takes them with the creature's picture.
 *
 *   moon   its cells are made once a day (sky_moon_cells: the phase and distance at that day's noon, UTC) and kept: a
 *          frame costs only their merge. Lit side as seen from the northern hemisphere (waxing: the right). A new moon
 *          shows nothing (the dark side is not drawn; under SKY_LIT_MIN lit, no dot at all).
 *   star   a streak from the upper sky down at a slant, left or right of the cat, away from the moon, its trail fading
 *          after it; one every SKY_STAR_GAP_MIN_S..MAX_S of the creature on the glass (the wait counts only while one
 *          may show), at random from the sky's own seed.
 * Without a date nothing is made or drawn and no random number is drawn (the sky never touches the creature's own).
 * Plain C, no ESP-IDF (host-tested: test/host/sky_test.c).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* where the moon sits: the options page's "A sky" (stage fraction; 354, 98 of 466 px) and its size there */
#define SKY_MOON_X      0.7597f
#define SKY_MOON_Y      0.2099f
#define SKY_MOON_R      0.085f          /* option A's radius, stage fraction; the moon is SKY_SIZE of it            */
#define SKY_GAIN        0.45f           /* "quiet": its brightest dots at ~0.45 of white, under the cat's eyes      */
#define SKY_LIT_MIN     0.01f           /* lit fraction under which no dot shows (a new moon)                        */
/* the drawn size by the moon's real distance (km -> share of option A's radius), straight lines between, held at the
 * ends: the true change (about 14 % between farthest and nearest) is too little to see at this size, so it is
 * exaggerated. A supermoon is simply the full moon on a big night. */
#define SKY_SIZE_N      3
#define SKY_SIZE_KM     { 357000.f, 384400.f, 406000.f }
#define SKY_SIZE_K      { 1.00f, 0.75f, 0.70f }

/* the falling star */
#define SKY_STAR_GAP_MIN_S  240.f       /* the wait for the next one: at random between these                         */
#define SKY_STAR_GAP_MAX_S  900.f
#define SKY_STAR_DUR_MIN_S  0.35f       /* the head's run                                                              */
#define SKY_STAR_DUR_MAX_S  0.6f
#define SKY_STAR_TAU_S      0.3f        /* the trail fades by e each this long after the head passed, while it runs    */
#define SKY_STAR_FADE_S     0.6f        /* ... and what is left of it is gone this long after the head went out       */
#define SKY_STAR_LEN_MIN    0.16f       /* its path, stage fraction                                                    */
#define SKY_STAR_LEN_MAX    0.26f
#define SKY_STAR_HEAD       0.78f       /* the head's brightness (of SKY_STAR_RGB): its luma ~190, the cat's eyes ~205 */
#define SKY_STAR_RGB        0xfff8e6u

#define SKY_MOON_CELLS  192             /* the largest moon's cells, with room                                        */
#define SKY_STAR_CELLS  64              /* a streak's cells at most (its dots: SKY_STAR_DOTS)                          */
#define SKY_STAR_DOTS   96

/** A cell of the sky: its index (j * W + i), its braille dots (fluid_braille_bit order), its colour 0xRRGGBB. */
typedef struct { uint16_t k; uint8_t bits; uint8_t _pad; uint32_t rgb; } sky_cell_t;

typedef struct { int16_t X, Y; float b; } sky_dot_t;   /* (a streak's dot while its cells are made) */

/** The moon at a moment (sky_moon_at). */
typedef struct {
    double elong;                       /* its elongation from the sun, degrees 0..360 (0 new, 180 full)              */
    float  lit;                         /* the lit fraction of its face, 0..1                                         */
    bool   waxing;                      /* growing (lit on the right, seen from the north)                            */
    float  age;                         /* days since the new moon, from the elongation                              */
    float  km;                          /* its distance, centre to centre                                             */
} sky_moon_t;

/** The moon at unix time t (s, UTC): Meeus, Astronomical Algorithms (2nd ed.), ch. 47, the main terms. */
sky_moon_t sky_moon_at(double t);
/** Its drawn radius as a share of option A's, for distance km (SKY_SIZE). */
float sky_size_k(float km);
/** Its drawn radius on a grid of W x H cells: whole dots across (2W dots a stage). */
int   sky_moon_rdots(float km, int W);

typedef struct {
    uint32_t rng;                       /* xorshift32 (the sky's own)                                                 */
    float wait;                         /* s of a star allowed till the next one starts                               */
    float t;                            /* the star's age, s; < 0: none                                               */
    float x0, y0, ux, uy, len, dur;     /* its start, direction, length (stage fraction) and run (s)                  */
    float px0, py0, pux;                /* the last one's start and direction across (never the same path twice)     */
    int   n;                            /* stars shown since the seed                                                 */
} sky_star_t;

typedef struct sky {
    bool on;                            /* the setting (sky_set): off, nothing                                        */
    int64_t day;                        /* the UTC day the moon's cells were made for (-1 none)                        */
    int W, H;                           /* ... on this grid                                                           */
    sky_moon_t moon;                    /* ... the moon then                                                          */
    int rdots;                          /* ... its radius, dots across                                                */
    int nmoon, nstar;                   /* the moon's cells made; the star's this frame                               */
    int nmoon_shown;                    /* the moon's cells to draw this frame (0: it may not show)                   */
    sky_cell_t moon_c[SKY_MOON_CELLS];  /* its cells, by index                                                        */
    sky_cell_t star_c[SKY_STAR_CELLS];  /* this frame's star                                                          */
    sky_star_t star;
    sky_dot_t sdot[SKY_STAR_DOTS];      /* (scratch: the streak's dots, off the caller's stack)                      */
    bool show;                          /* the last sky_frame drew anything (moon or star)                            */
} sky_t;

/** A sky, its stars seeded (never 0 from a non-zero seed's mix): on, nothing made yet. */
void sky_init(sky_t *s, uint32_t seed);
/** The setting: off, nothing is drawn (the moon is made again when it comes back on). */
void sky_set(sky_t *s, bool on);
/** Once a frame: utc the time now (s; <= 0 not known: nothing), the grid, dt since the last frame (s); moon: the moon
 *  may show (the creature on the glass); star: a falling star may show (nothing else owns the glass, not dark and
 *  quiet); a star showing when it may not any more goes out at once. Its cells are then s->moon_c, s->star_c. */
void sky_frame(sky_t *s, double utc, int W, int H, float dt, bool moon, bool star);
/** Cells made for the moon at time t on W x H (out: at most cap; returns how many). Its radius in dots: *rdots. */
int  sky_moon_cells(const sky_moon_t *m, int W, int H, sky_cell_t *out, int cap, int *rdots);

#ifdef __cplusplus
}
#endif
