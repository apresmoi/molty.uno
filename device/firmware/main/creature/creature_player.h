/*
 * SPDX-License-Identifier: MIT
 *
 * creature_player.h: a creature package (docs/PACKAGES.md section 5, docs/CREATURES.md) played
 * on the device. Port of device/creatures/js/creature-player.js, hash and draw order included,
 * so the same package moves the same way on both (test/host/creature_test.c replays the JS
 * player's goldens, tools/creatures/golden.mjs).
 *
 * Plain C + cJSON, no FreeRTOS / ESP-IDF. The player knows channels, terms and a fixed list
 * of factor ops; nothing here names what a creature is.
 *
 *   parse     cp_create: the whole file, strictly (any key the format does not name, a wrong
 *             type, an unknown op, source, bone, group or palette name: refused, with the
 *             reason) and bounded (the limits of docs/CREATURES.md section 7).
 *   play      cp_pose once a frame with the mood engine's pose, the gesture playing (and the one
 *             fading out) and the walk; then cp_bone (the bone outputs), cp_tints (the palette)
 *             and cp_ambient (particles on the glass, a current in the fluid).
 *   numbers   channel values are float; the clocks (time, mood clocks, block clocks, integrated
 *             phases) are double, and the periodic ops form their argument in double, so a
 *             creature moves the same after a day as after a minute.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CP_FILE_MAX     (32 * 1024)
#define CP_BONES_MAX    96
#define CP_PARTS_MAX    96
#define CP_PALETTE_MAX  32
#define CP_PATH_MAX     16       /* points of a tube                                           */
#define CP_CHANNELS_MAX 512
#define CP_TERMS_MAX    1024
#define CP_FACTORS_MAX  2048
#define CP_TERM_FACTORS 8
#define CP_GROUP_MAX    64       /* bones in a group                                           */
#define CP_ATTRS_MAX    32       /* attributes of a group                                      */
#define CP_EVENTS_MAX   16
#define CP_HIST_S       1.f      /* history a channel may keep for `dl`                        */
#define CP_LIVE_MAX     64       /* live particles                                             */
#define CP_ID_MAX       32
#define CP_NAME_MAX     32

enum { CPS_ELLIPSOID, CPS_BOX, CPS_CONE, CPS_CYLINDER, CPS_TUBE };
enum { CP_EYE_NORMAL, CP_EYE_WIDE, CP_EYE_HAPPY, CP_EYE_SLEEPY, CP_EYE_SQUINT };
#define CP_N_MOODS 7             /* idle listening thinking speaking happy sleepy surprised   */

typedef struct {
    char id[25];
    int16_t parent;              /* bone index, -1 = the root                                  */
    float pos[3], rot[3];        /* rest pose; rot: euler XYZ (rad)                             */
    float d;                     /* depth down its chain (tints that ripple)                   */
} cp_bone_t;

typedef struct {
    uint8_t type;                /* CPS_ELLIPSOID ..                                             */
    int16_t bone;                /* -1 = the root                                               */
    int16_t pal;                 /* palette index, -1 = a literal colour (rgb)                  */
    uint8_t rgb[3];
    float pos[3], rot[3];
    float size[3];               /* ellipsoid radii, box sides                                  */
    float r, r2, h;              /* cylinder top r, bottom r2 (default r); cone r; tube radius   */
    uint8_t seg[2];              /* ellipsoid w, h; cone / cylinder / tube segments             */
    uint8_t npath;
    float path[CP_PATH_MAX][3];
} cp_part_t;

typedef struct {
    int nbones, nparts, npal;
    cp_bone_t bones[CP_BONES_MAX];
    cp_part_t parts[CP_PARTS_MAX];
    char pal_name[CP_PALETTE_MAX][25];
    uint8_t pal_rgb[CP_PALETTE_MAX][3];
} cp_body_t;

/* the shared pose (js/mood.js), read and written back */
typedef struct {
    float pos[3], rot[3], head[3], spine[3];
    float squash, lid, mouth, look[2], prowl;
    int eye;                     /* CP_EYE_*                                                    */
    int mood;                    /* 0..CP_N_MOODS-1 (cute_spec.h CM_*)                          */
} cp_pose_t;

/* a gesture play: act = cute_act.h CA_* (-1 none), t its clock, n a number new for each play */
typedef struct { int act; float t; unsigned n; } cp_action_t;
typedef struct { float q, k, go, along, side, fz, up; } cp_walk_t;

/* a mark on the glass (window units: radius 1, y up) */
enum { CP_MARK_DOT, CP_MARK_STAR, CP_MARK_BLOT };
typedef struct { uint8_t kind, d; uint8_t rgb[3]; float x, y, size, rot; } cp_mark_t;

/* what the ambient sees and moves: the body's coverage this frame (W×H cells over the window
 * of half-width win), and the fluid (x, y in -1..1, y down) */
typedef struct {
    float dt;
    const float *cov; int W, H; float win;
    void *ctx;
    void (*impulse)(void *ctx, float x, float y, float strength, float radius);
    void (*push)(void *ctx, float x, float y, float vx, float vy, float radius);
} cp_env_t;

typedef struct cp cp_t;

/** Parse and compile a creature package. NULL with *err (static) on any violation. */
cp_t *cp_create(const char *json, size_t len, const char **err);
void  cp_destroy(cp_t *c);
const char *cp_id(const cp_t *c);
const char *cp_name(const cp_t *c);
const char *cp_version(const cp_t *c);
/** What an agent is told about this creature, as JSON: {id, name, about?, gestures, moods}. about is the
 *  package's own (its look, a line per gesture and mood); gestures are those it plays that this firmware
 *  knows, moods idle and its own, in the firmware's order. */
const char *cp_info(const cp_t *c);
const cp_body_t *cp_body(const cp_t *c);
/** The bone index of `id`, nbones for "root", -1 for none. */
int   cp_bone_find(const cp_t *c, const char *id);
/** Play one frame: every layer in order (walk, events, base, gestures, moods, post). p is the
 *  mood engine's pose; the creature's is written back into it. prev: the gesture fading out at
 *  weight prev_w (or NULL). */
void  cp_pose(cp_t *c, cp_pose_t *p, double dt, const cp_action_t *act, const cp_action_t *prev, float prev_w, const cp_walk_t *walk);
/** This frame's output for bone b (0..nbones, nbones = root): rx ry rz px py pz (added to its
 *  rest pose) sx sy sz (multiplying its scale). */
const float *cp_bone_out(const cp_t *c, int b);
/** The palette this frame for a bone at depth d: out[i] = 0xRRGGBB of palette entry i. */
void  cp_tints(const cp_t *c, float d, uint32_t *out);
bool  cp_tinted(const cp_t *c);
/** The ambient this frame: marks into out (at most max, deepest d last as drawn: the caller
 *  sorts), impulses and currents through env. Returns the number of marks. */
int   cp_ambient(cp_t *c, const cp_env_t *env, cp_mark_t *out, int max);
/** Factor evaluations in the last frame (the cost meter of docs/CREATURES.md section 8). */
int   cp_factors(const cp_t *c);
/** The reference player's integer hash (murmur3 fmix32 of floor(16 n)), in 0..1. */
double cp_hash(double n);

#ifdef __cplusplus
}
#endif
