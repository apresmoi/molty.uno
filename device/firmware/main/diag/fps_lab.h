/*
 * SPDX-License-Identifier: MIT
 *
 * fps_lab.h: the frame rate lab (FPS-LAB.md). Named integer knobs, set at run time (GET / POST /api/lab, fps_lab_dev.c),
 * back to their defaults at boot; every default is today's behaviour. While `rec` is on, a record a frame goes into a
 * ring in PSRAM (the sim's side at the handoff, the drawer's when it is done), summarised outside the frame (the lab's
 * task, the HTTP task): each stage's mean and percentiles, the intervals in tearing pulses, and the tracked item's
 * distinct completed positions a second and its longest hold. While `pose` is on, the creature's pose key a frame goes
 * into a second ring: how often a pose repeats, idle and in gestures, and what a cache of N poses would hit.
 *
 * The rings are made and freed by fps_lab_reconcile (the lab's task), never by the frame's tasks; those only write.
 * Plain C (host-testable); the ESP side (HTTP, the task, the creature's glue) is fps_lab_dev.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "fluid_sim.h"   /* fluid_draw_opts_t, fluid_shape_t */
#include "creature_dots.h"   /* cd_reuse_t */

#ifdef __cplusplus
extern "C" {
#endif

/* the knobs: rec (the frame records), track (the tracked item: -1 the scene's whole drawing, i its shape i), pose (the
 * creature's pose repeats), skip (experiment 1: an unchanged pose's cells kept), cache and cache_n (experiment 2: the
 * creature's cells of its last cache_n distinct poses kept, PSRAM) */
enum { FL_REC, FL_TRACK, FL_POSE, FL_SKIP, FL_CACHE, FL_CACHE_N, FL_N };
/** Knob k's value now (any task: a word read). */
int  fps_lab(int k);
const char *fps_lab_name(int k);
int  fps_lab_default(int k);
/** Set knob `name` to v: 0, -1 for no such knob, -2 for a value out of its range. */
int  fps_lab_set(const char *name, int v);
/** ... only whether it would: the knob's index, else -1 or -2 as fps_lab_set. */
int  fps_lab_check(const char *name, int v);
void fps_lab_reset(void);
/** Any knob off its default (the 10 s log line runs then). */
bool fps_lab_nondefault(void);

/* ── the frame records ───────────────────────────────────────────────────── */

#define FL_FRAMES 1024           /* the ring, frames (34 s at 30 fps)                                                  */
enum { FL_OTHER, FL_CREATURE, FL_SCENE };
/** The sim's side at the handoff (fluid_task, mole_fluid.c, just before the drawer is told): c1 its stages (sim, dust
 *  ahead, the dust's build, a change's reading job, us), pace_us its wait for the pulse, idle_us its wait for the
 *  drawer, te the pulse it woke on, o what goes to the drawer (the tracked item's signature), N its cells. */
void fps_lab_handoff(const int c1[4], int pace_us, int idle_us, uint32_t te, const fluid_draw_opts_t *o, int N, bool crisp);
/** After a rigged creature's frame (fluid_task): its pose and reuse counters to the lab (the next handoff's signature,
 *  the poses' ring), the knobs to it for its next frame. */
struct cpk;
void fps_lab_cpk(struct cpk *k);
/** Its reuse counters as of its last frame (NULL: none yet). */
const cd_reuse_t *fps_lab_creature(void);
typedef struct {
    uint32_t cells_us, send_us, wait_us;    /* the drawer's cells, its send, its wait for the last transfer           */
    uint32_t wins, px, changed, owed;       /* windows and pixels sent, cells changed, cells owed to the next frame   */
    uint32_t te_pres;                       /* the pulse at the frame's first window (pres: 0 if none)                */
    bool pres, failed;                      /* a window went out; a window failed (all repainted next)                */
} fl_drawn_t;
/** The drawer's side, when its frame is done (fluid_draw, before it is idle again). */
void fps_lab_drawn(const fl_drawn_t *d);

typedef struct { float mean; uint32_t p50, p95, p99, max; } fl_stat_t;
enum { FM_SIM, FM_DUST, FM_JOB, FM_PACE, FM_IDLE, FM_CELLS, FM_SEND, FM_WAIT, FM_WINS, FM_PX, FM_CHANGED, FM_OWED,
       FM_FRAME, FM_LAB, FM_N };
extern const char *const FL_METRIC_NAMES[FM_N];
typedef struct {
    int n;                                  /* frames summarised (drawn, newest first, at most FL_FRAMES - 2)         */
    float span_s;                           /* ... their time                                                         */
    fl_stat_t m[FM_N];                      /* us, but wins px changed owed (counts)                                  */
    uint32_t wake[4], pres[4];              /* intervals in pulses (1 or less, 2, 3, 4 or more): handoffs, first windows */
    uint32_t late, late_pres, repeats;      /* handoffs past their target pulse (2 on); first windows over 2 apart;
                                             * frames that sent nothing (the same picture shown again)                */
    int kind;                               /* what showed in the newest: FL_OTHER, FL_CREATURE, FL_SCENE             */
    uint32_t moves, done, hold;             /* the tracked item: handoffs it moved in; distinct positions completed
                                             * (all its cells sent, none owed); the longest a completed one held, pulses */
    float moves_s, done_s;                  /* ... a second                                                           */
} fl_sum_t;
/** The newest scene frame's shapes, a letter a shape by kind (s segment, r ring, d disc, m mask, c char; at most
 *  FL_KINDS): the index to give `track`. */
#define FL_KINDS 64
const char *fps_lab_shapes(void);
/** The ring's summary (false: no ring). Allowed on any task but the frame's (it sorts). */
bool fps_lab_summary(fl_sum_t *s);
/** The newest n records as CSV into out (at most len bytes, a header first): the bytes written. */
int  fps_lab_csv(char *out, int len, int n);

/* ── the creature's poses (measurement 1) ─────────────────────────────────── */

#define FL_POSES 2048            /* the ring of pose keys (68 s at 30 fps)                                             */
#define FL_TOP   8
#define FL_LRU_N 4
extern const int FL_LRU[FL_LRU_N];   /* the cache sizes simulated over the minute: 1 (the skip), 4, 16, 64 */
/** The creature's pose this frame: its key (creature_dots.h cd_pose_key), whether it played more than its mood, now ms. */
void fps_lab_pose(uint64_t key, bool gesture, uint32_t t_ms);
typedef struct {
    float span_s;                           /* the window: the last minute's keys (less till a minute is in)          */
    int n, ng;                              /* frames in it; ... in a gesture                                         */
    int same, same_g;                       /* ... whose pose was the frame before's (and in a gesture)                */
    int distinct, distinct_i, distinct_g;   /* distinct poses: all, idle, in gestures                                 */
    int ntop; struct { uint32_t n; bool g; } top[FL_TOP];   /* the poses seen most, how often, in a gesture        */
    float lru[FL_LRU_N];                    /* the share of its frames a cache of FL_LRU[i] poses would have hit       */
    uint32_t total, total_same;             /* since `pose` came on: frames, and those the same as the frame before    */
} fl_pose_sum_t;
bool fps_lab_pose_summary(fl_pose_sum_t *s);

/** Make or free the rings the knobs ask for (the lab's task, every few hundred ms and when a knob changed). A ring is
 *  taken away first and freed after FL_QUIET_MS, so a frame's task never writes into freed memory. */
void fps_lab_reconcile(void);
/** The rings' PSRAM now, bytes. */
uint32_t fps_lab_bytes(void);

#ifdef __cplusplus
}
#endif
