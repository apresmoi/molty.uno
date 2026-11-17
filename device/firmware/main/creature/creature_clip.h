/*
 * SPDX-License-Identifier: MIT
 *
 * creature_clip.h: a creature package with a rig (docs/CREATURES.md section 12) played from its baked clips.
 * The C port of device/creatures/js/clip-player.js, step for step:
 *
 *   rig       nodes (a parent, the rest translation, rotation and scale), the body's rounded primitives
 *             (creature_dots.h draws them), the paws' contact points, the palette
 *   clips     glTF's animation layout: per node channels for t / r / s, each keys (frames) and int16 outputs
 *             (STEP, or CUBICSPLINE with an in- and out-tangent per key), decoded once into floats
 *   graph     pose families, one authored clip per ordered pair (the route map), the families' resting loops,
 *             what each mood and gesture plays, the startle's own way in, the walk's stride for leaving it
 *   play      cc_mood / cc_act, then cc_update once a frame: routing, sampling, crossfades (with a light paw
 *             lock), root motion, the camera
 *
 * Plain C, no ESP-IDF (host-testable).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CC_NODES_MAX  80
#define CC_CLIPS_MAX  64
#define CC_SHAPES_MAX 128
#define CC_FAMS_MAX   8
#define CC_EDGES_MAX  64
#define CC_GEST_MAX   32
#define CC_NAME_MAX   24
#define CC_POSE       10          /* per node: t (3), r (4, x y z w), s (3)                          */

enum { CC_BLOB, CC_ELL, CC_CAP, CC_CONE, CC_CYL, CC_PATCH };
/* outline groups (in the depth buffer), then features */
enum { CG_TORSO, CG_HINDL, CG_HINDR, CG_HEAD, CG_LEGFL, CG_LEGFR, CG_TAIL, CG_BOWL, CG_YARN, CG_NSTRUCT,
       CG_EYE = CG_NSTRUCT, CG_EARIN, CG_NOSE, CG_MAW, CG_TONGUE, CG_WHISKER, CG_CHEST, CG_MILK, CG_YARNLINE, CG_COUNT };
enum { CC_IRIS, CC_PUPIL, CC_SHINE, CC_COL_OTHER };

typedef struct {
    uint8_t kind, node, group, col;     /* col: palette index                                              */
    uint8_t eye;                        /* CC_IRIS / CC_PUPIL / CC_SHINE for the eye group                  */
    float p[3], p1[3], q[4], r[3], st, fa[3];
    float rq[9];                        /* q as a 3x3 matrix, rows                                          */
} cc_shape_t;

typedef struct cc cc_t;

/** Parse the creature's `rig` (cr is the `creature` object, a cJSON *). NULL with *err. */
cc_t *cc_create(const void *cr, const char **err);
void  cc_destroy(cc_t *c);

int  cc_nodes(const cc_t *c);
const char *cc_node_id(const cc_t *c, int i);
int  cc_node_find(const cc_t *c, const char *id);
int  cc_shapes(const cc_t *c, const cc_shape_t **out);
/** The palette entry's colour (0xRRGGBB) by name; 0x000000 for a missing one. */
uint32_t cc_color(const cc_t *c, const char *name);
uint32_t cc_color_at(const cc_t *c, int i);
int  cc_clips(const cc_t *c);
const char *cc_clip_name(const cc_t *c, int i);
int  cc_clip_find(const cc_t *c, const char *name);

/** The agent's mood (idle listening thinking speaking happy sleepy surprised); a gesture by name (false: it lacks it). */
void cc_mood(cc_t *c, const char *mood);
bool cc_act(cc_t *c, const char *gesture);
/** Called (a tap, a shake): first its own reaction if given (a shake: "shaken"), then it walks to the front of the frame
 *  facing the viewer and sits up attending (CC_ATTEND), or, facing front near the middle already, just perks up; then
 *  back to its mood. cc_act and cc_play end a call. */
bool cc_come(cc_t *c, const char *first);
bool cc_coming(const cc_t *c);
/** Play a clip at once (no route; the preview's player and the tests). */
void cc_play(cc_t *c, const char *clip);

/** One frame: dt seconds. */
void cc_update(cc_t *c, float dt);
/** Each node's world matrix this frame: 12 floats a node (3 rows of 4). */
const float *cc_world(const cc_t *c);
/** The camera this frame: where it looks on the floor and its yaw in degrees (0 in front, -90 from the cat's right). */
void cc_cam(const cc_t *c, float *x, float *z, float *yaw);
/** What plays: the clip, its time, the queued route (count). */
int  cc_clip(const cc_t *c, float *t);
int  cc_queue(const cc_t *c, int *out, int max);

/** The authored route from family a to family b (clip indices); -1 when there is none, else its length. */
int  cc_route(const cc_t *c, int fa, int fb, int *out, int max);
int  cc_families(const cc_t *c);
const char *cc_family_name(const cc_t *c, int f);
int  cc_clip_from(const cc_t *c, int clip);
int  cc_clip_to(const cc_t *c, int clip);
bool cc_clip_loop(const cc_t *c, int clip);

#ifdef __cplusplus
}
#endif
