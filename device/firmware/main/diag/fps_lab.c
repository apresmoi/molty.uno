/*
 * SPDX-License-Identifier: MIT
 *
 * fps_lab.c: the frame rate lab's knobs, rings and summaries (fps_lab.h). Its state lives in PSRAM (EXT_RAM_BSS_ATTR,
 * zeroed at boot: a knob is kept as its offset from its default, so zero is the default); the rings are allocated in
 * PSRAM only while a knob asks for them. Plain C, host-testable.
 */
#include "fps_lab.h"
#include "creature_pkg.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define FL_PSRAM     EXT_RAM_BSS_ATTR
#define FL_CALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define FL_FREE(p)   heap_caps_free(p)
static int64_t now_us(void) { return esp_timer_get_time(); }
static void quiet(void) { vTaskDelay(pdMS_TO_TICKS(100)); }   /* a frame's task done with what it took (FL_QUIET_MS) */
static void lock_wait(void) { vTaskDelay(1); }
static uint32_t cyc_us(uint64_t c) { return (uint32_t)(c / (uint64_t)(esp_clk_cpu_freq() / 1000000)); }
#else
#include <time.h>
#define FL_PSRAM
#define FL_CALLOC(n) calloc(1, (n))
#define FL_FREE(p)   free(p)
static int64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
static void quiet(void) { }
static void lock_wait(void) { }
static uint32_t cyc_us(uint64_t c) { (void)c; return 0; }   /* (no cycle counter on the host: creature_dots.c's is 0) */
#endif

/* ── knobs ─────────────────────────────────────────────────────────────── */

static const struct { const char *name; int def, lo, hi; } KNOB[FL_N] = {
    [FL_REC] = { "rec", 0, 0, 1 }, [FL_TRACK] = { "track", -1, -1, FLUID_MAX_SHAPES - 1 }, [FL_POSE] = { "pose", 0, 0, 1 },
    [FL_SKIP] = { "skip", 0, 0, 1 }, [FL_CACHE] = { "cache", 0, 0, 1 }, [FL_CACHE_N] = { "cache_n", 16, 1, 128 },
};
FL_PSRAM static volatile int s_off[FL_N];   /* each knob's offset from its default */

int fps_lab(int k) { return KNOB[k].def + s_off[k]; }
const char *fps_lab_name(int k) { return KNOB[k].name; }
int fps_lab_default(int k) { return KNOB[k].def; }
int fps_lab_check(const char *name, int v) {
    for (int k = 0; k < FL_N; k++) if (!strcmp(name, KNOB[k].name)) return v < KNOB[k].lo || v > KNOB[k].hi ? -2 : k;
    return -1;
}
int fps_lab_set(const char *name, int v) {
    const int k = fps_lab_check(name, v);
    if (k < 0) return k;
    s_off[k] = v - KNOB[k].def;
    return 0;
}
void fps_lab_reset(void) { for (int k = 0; k < FL_N; k++) s_off[k] = 0; }
bool fps_lab_nondefault(void) { for (int k = 0; k < FL_N; k++) if (s_off[k]) return true; return false; }

/* ── the frame records ───────────────────────────────────────────────────── */

enum { FR_SIG = 1, FR_PRES = 2, FR_FAIL = 4 };
enum { V_N = FM_FRAME };                    /* the metrics a record keeps (FM_FRAME is made from t_us; FM_LAB below) */
typedef struct {
    uint32_t seq, done;                     /* done: seq + 1 once the drawer's side is in                             */
    uint32_t t_us, te_wake, te_target, te_pres, sig;
    uint8_t kind, flags;
    uint32_t v[V_N], lab;                   /* FM_SIM .. FM_OWED; the lab's own us this frame                        */
} fl_rec_t;
typedef struct { fl_rec_t r[FL_FRAMES]; uint32_t tmp[FL_FRAMES]; } fl_ring_t;
typedef struct {
    uint64_t key[FL_POSES]; uint32_t t[FL_POSES]; uint8_t g[FL_POSES];
    uint64_t tmp[FL_POSES], lk[64]; uint32_t la[64];   /* the summary's scratch: sorted keys, a simulated cache */
} fl_poses_t;
const char *const FL_METRIC_NAMES[FM_N] = { "sim", "dust", "job", "pace", "idle", "cells", "send", "wait", "wins", "px",
                                            "changed", "owed", "frame", "lab" };
const int FL_LRU[FL_LRU_N] = { 1, 4, 16, 64 };

/* the rings and the lock in internal RAM (12 bytes: the S3's atomic read-modify-write is for internal RAM only), the rest
 * in PSRAM */
static struct {
    fl_ring_t *_Atomic ring; fl_poses_t *_Atomic poses;
    atomic_int lock;                        /* the summaries and reconcile (never the frame's tasks)                  */
} A;
FL_PSRAM static struct {
    /* the sim's (fluid_task) */
    uint32_t seq, last_te; int64_t last_t; bool have_last;
    uint32_t pend; bool pend_ok;            /* the record the drawer completes                                       */
    uint32_t cr_sig, cr_cost; bool cr_on;   /* the creature's pose key for the next handoff, its cost                 */
    cd_reuse_t cr; bool cr_have; uint64_t upkeep;   /* its reuse counters, its last frame's; their upkeep seen so far */
    char kinds[FL_KINDS + 1];               /* the newest scene frame's shapes by kind (fps_lab_shapes)               */
    /* the poses' */
    uint32_t pn, total, total_same; uint64_t plast;
} L;

static void lock(void) { while (atomic_exchange(&A.lock, 1)) lock_wait(); }
static void unlock(void) { atomic_store(&A.lock, 0); }

static inline uint32_t mix(uint32_t h, uint32_t w) { return (h ^ w) * 16777619u; }
static inline uint32_t fword(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint32_t shape_sig(uint32_t h, const fluid_shape_t *s) {
    const float f[12] = { s->x, s->y, s->x1, s->y1, s->r, s->hw, s->a0, s->span, s->w, s->h, s->co, s->si };
    h = mix(mix(h, s->k), s->rgb);
    for (int i = 0; i < 12; i++) h = mix(h, fword(f[i]));
    return h;
}
/* what the drawer gets of the tracked item: shape `track`, or (-1) every shape and every dot */
static uint32_t sig_of(const fluid_draw_opts_t *o, int N, int track) {
    uint32_t h = 2166136261u;
    if (track >= 0) return o->shapes && track < o->nshapes ? shape_sig(h, &o->shapes[track]) : h;
    for (int i = 0; o->shapes && i < o->nshapes; i++) h = shape_sig(h, &o->shapes[i]);
    if (o->dots) {
        int i = 0;
        for (; i + 4 <= N; i += 4) { uint32_t w; memcpy(&w, o->dots + i, 4); h = mix(h, w); }
        for (; i < N; i++) h = mix(h, o->dots[i]);
    }
    return h;
}

void fps_lab_cpk(struct cpk *k) {
    const cd_reuse_t *st = cpk_reuse_stats(k);
    if (st && st->key) {
        const uint64_t up = st->upkeep >= L.upkeep ? st->upkeep - L.upkeep : st->upkeep;   /* (counters reset: from 0) */
        L.upkeep = st->upkeep; L.cr = *st; L.cr_have = true;
        L.cr_sig = (uint32_t)(st->key ^ st->key >> 32); L.cr_on = true; L.cr_cost += cyc_us(up);
        if (fps_lab(FL_POSE)) fps_lab_pose(st->key, st->gesture, (uint32_t)(now_us() / 1000));
    }
    cpk_reuse(k, fps_lab(FL_POSE) || fps_lab(FL_REC), fps_lab(FL_SKIP), fps_lab(FL_CACHE) ? fps_lab(FL_CACHE_N) : 0);
}
const cd_reuse_t *fps_lab_creature(void) { return L.cr_have ? &L.cr : NULL; }
const char *fps_lab_shapes(void) { return L.kinds; }

void fps_lab_handoff(const int c1[4], int pace_us, int idle_us, uint32_t te, const fluid_draw_opts_t *o, int N, bool crisp) {
    fl_ring_t *R = atomic_load(&A.ring);
    if (!R) { L.have_last = false; L.pend_ok = false; L.cr_cost = 0; return; }
    const int64_t t0 = now_us();
    const uint32_t seq = L.seq++;
    fl_rec_t *r = &R->r[seq % FL_FRAMES];
    r->done = 0; r->seq = seq;
    r->t_us = (uint32_t)t0; r->te_wake = te; r->te_target = L.have_last ? L.last_te + 2 : te; r->te_pres = 0;
    r->v[FM_SIM] = (uint32_t)c1[0]; r->v[FM_DUST] = (uint32_t)(c1[1] + c1[2]); r->v[FM_JOB] = (uint32_t)c1[3];
    r->v[FM_PACE] = pace_us > 0 ? (uint32_t)pace_us : 0; r->v[FM_IDLE] = idle_us > 0 ? (uint32_t)idle_us : 0;
    r->flags = 0;
    if (crisp) { r->kind = FL_CREATURE; r->sig = L.cr_sig; r->flags = L.cr_on ? FR_SIG : 0; }
    else if (o->shapes && o->nshapes > 0) {
        r->kind = FL_SCENE; r->sig = sig_of(o, N, fps_lab(FL_TRACK)); r->flags = FR_SIG;
        static const char K[] = "srdmc";             /* FLUID_SHAPE_SEG, RING, DISC, MASK, CHAR */
        int i = 0;
        for (; i < o->nshapes && i < FL_KINDS; i++) L.kinds[i] = o->shapes[i].k < 5 ? K[o->shapes[i].k] : '?';
        L.kinds[i] = 0;
    }
    else { r->kind = FL_OTHER; r->sig = 0; }
    L.last_te = te; L.last_t = t0; L.have_last = true;
    L.pend = seq; L.pend_ok = true;
    r->lab = L.cr_cost + (uint32_t)(now_us() - t0); L.cr_cost = 0;
}

void fps_lab_drawn(const fl_drawn_t *d) {
    fl_ring_t *R = atomic_load(&A.ring);
    if (!R || !L.pend_ok) return;
    const int64_t t0 = now_us();
    fl_rec_t *r = &R->r[L.pend % FL_FRAMES];
    L.pend_ok = false;
    if (r->seq != L.pend) return;
    r->v[FM_CELLS] = d->cells_us; r->v[FM_SEND] = d->send_us; r->v[FM_WAIT] = d->wait_us;
    r->v[FM_WINS] = d->wins; r->v[FM_PX] = d->px; r->v[FM_CHANGED] = d->changed; r->v[FM_OWED] = d->owed;
    r->te_pres = d->te_pres;
    r->flags |= (uint8_t)((d->pres ? FR_PRES : 0) | (d->failed ? FR_FAIL : 0));
    r->lab += (uint32_t)(now_us() - t0);
    atomic_thread_fence(memory_order_release);
    r->done = r->seq + 1;
}

static int cmp_u32(const void *a, const void *b) { const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
static void stat_of(uint32_t *a, int n, fl_stat_t *s) {
    memset(s, 0, sizeof(*s));
    if (n <= 0) return;
    qsort(a, (size_t)n, sizeof(uint32_t), cmp_u32);
    double sum = 0; for (int i = 0; i < n; i++) sum += a[i];
    s->mean = (float)(sum / n);
    s->p50 = a[(n * 50 + 99) / 100 - 1]; s->p95 = a[(n * 95 + 99) / 100 - 1]; s->p99 = a[(n * 99 + 99) / 100 - 1]; s->max = a[n - 1];
}
static inline int bucket(uint32_t dp) { return dp <= 1 ? 0 : dp >= 4 ? 3 : (int)dp - 1; }
/* the records drawn, oldest first: [first, end) of seq, a record valid when complete (the newest may not be yet) */
static const fl_rec_t *rec_at(const fl_ring_t *R, uint32_t seq) { const fl_rec_t *r = &R->r[seq % FL_FRAMES]; return r->seq == seq && r->done == seq + 1 ? r : NULL; }

bool fps_lab_summary(fl_sum_t *s) {
    memset(s, 0, sizeof(*s));
    lock();
    fl_ring_t *R = atomic_load(&A.ring);
    if (!R) { unlock(); return false; }
    atomic_thread_fence(memory_order_acquire);
    const uint32_t end = L.seq, first = end > FL_FRAMES - 2 ? end - (FL_FRAMES - 2) : 0;
    for (int m = 0; m < FM_N; m++) {
        int n = 0;
        const fl_rec_t *prev = NULL;
        for (uint32_t q = first; q < end; q++) {
            const fl_rec_t *r = rec_at(R, q);
            if (!r) { prev = NULL; continue; }
            if (m == FM_FRAME) { if (prev) R->tmp[n++] = r->t_us - prev->t_us; }
            else R->tmp[n++] = m == FM_LAB ? r->lab : r->v[m];
            prev = r;
        }
        stat_of(R->tmp, n, &s->m[m]);
    }
    const fl_rec_t *prev = NULL, *pp = NULL, *a = NULL, *z = NULL;
    bool have = false; uint32_t dsig = 0, dte = 0;
    for (uint32_t q = first; q < end; q++) {
        const fl_rec_t *r = rec_at(R, q);
        if (!r) { prev = NULL; continue; }
        if (!a) a = r;
        z = r; s->n++;
        if (prev) {
            s->wake[bucket(r->te_wake - prev->te_wake)]++;
            if ((r->flags & FR_SIG) && (prev->flags & FR_SIG) && r->sig != prev->sig) s->moves++;
        }
        if ((int32_t)(r->te_wake - r->te_target) > 0) s->late++;
        if (!r->v[FM_WINS]) s->repeats++;
        if (r->flags & FR_PRES) {
            if (pp) { const uint32_t dp = r->te_pres - pp->te_pres; s->pres[bucket(dp)]++; s->late_pres += dp > 2; }
            pp = r;
        }
        /* a position completed: its frame sent, nothing failed, nothing owed */
        if ((r->flags & (FR_SIG | FR_PRES | FR_FAIL)) == (FR_SIG | FR_PRES) && !r->v[FM_OWED]) {
            if (!have) { have = true; dsig = r->sig; dte = r->te_pres; }
            else if (r->sig != dsig) {
                s->done++;
                if (r->te_pres - dte > s->hold) s->hold = r->te_pres - dte;
                dsig = r->sig; dte = r->te_pres;
            }
        }
        prev = r;
    }
    if (have && z) { const uint32_t te = z->te_pres ? z->te_pres : z->te_wake; if ((int32_t)(te - dte) > (int32_t)s->hold) s->hold = te - dte; }
    if (a && z && z != a) s->span_s = (float)(z->t_us - a->t_us) / 1e6f;
    if (z) s->kind = z->kind;
    if (s->span_s > 0.f) { s->moves_s = (float)s->moves / s->span_s; s->done_s = (float)s->done / s->span_s; }
    unlock();
    return true;
}

int fps_lab_csv(char *out, int len, int n) {
    int o = snprintf(out, (size_t)len, "seq,t_us,te_wake,te_target,te_pres,kind,flags,sig,sim,dust,job,pace,idle,cells,send,wait,wins,px,changed,owed,lab\n");
    if (o < 0 || o >= len) return 0;
    lock();
    fl_ring_t *R = atomic_load(&A.ring);
    const uint32_t end = R ? L.seq : 0, lim = end > FL_FRAMES - 2 ? end - (FL_FRAMES - 2) : 0;
    const uint32_t first = n > 0 && end - lim > (uint32_t)n ? end - (uint32_t)n : lim;
    for (uint32_t q = first; R && q < end; q++) {
        const fl_rec_t *r = rec_at(R, q);
        if (!r) continue;
        const uint32_t *v = r->v;
        const int w = snprintf(out + o, (size_t)(len - o), "%lu,%lu,%lu,%lu,%lu,%u,%u,%08lx,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
                               (unsigned long)r->seq, (unsigned long)r->t_us, (unsigned long)r->te_wake, (unsigned long)r->te_target,
                               (unsigned long)r->te_pres, (unsigned)r->kind, (unsigned)r->flags, (unsigned long)r->sig,
                               (unsigned long)v[FM_SIM], (unsigned long)v[FM_DUST], (unsigned long)v[FM_JOB], (unsigned long)v[FM_PACE],
                               (unsigned long)v[FM_IDLE], (unsigned long)v[FM_CELLS], (unsigned long)v[FM_SEND], (unsigned long)v[FM_WAIT],
                               (unsigned long)v[FM_WINS], (unsigned long)v[FM_PX], (unsigned long)v[FM_CHANGED], (unsigned long)v[FM_OWED],
                               (unsigned long)r->lab);
        if (w < 0 || w >= len - o) break;
        o += w;
    }
    unlock();
    return o;
}

/* ── the poses ─────────────────────────────────────────────────────────── */

void fps_lab_pose(uint64_t key, bool gesture, uint32_t t_ms) {
    fl_poses_t *P = atomic_load(&A.poses);
    if (!P) return;
    const uint32_t i = L.pn % FL_POSES;
    P->key[i] = key; P->t[i] = t_ms; P->g[i] = gesture;
    atomic_thread_fence(memory_order_release);
    L.total_same += L.pn && key == L.plast;
    L.total++; L.plast = key; L.pn++;
}

static int cmp_u64(const void *a, const void *b) { const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }

bool fps_lab_pose_summary(fl_pose_sum_t *s) {
    memset(s, 0, sizeof(*s));
    lock();
    fl_poses_t *P = atomic_load(&A.poses);
    if (!P) { unlock(); return false; }
    atomic_thread_fence(memory_order_acquire);
    const uint32_t end = L.pn, lim = end > FL_POSES - 1 ? end - (FL_POSES - 1) : 0;
    s->total = L.total; s->total_same = L.total_same;
    if (end == lim) { unlock(); return true; }
    const uint32_t tz = P->t[(end - 1) % FL_POSES];
    uint32_t first = end - 1;
    while (first > lim && tz - P->t[(first - 1) % FL_POSES] <= 60000) first--;
    s->span_s = (float)(tz - P->t[first % FL_POSES]) / 1000.f;
    int n = 0;
    for (uint32_t q = first; q < end; q++) {
        const uint32_t i = q % FL_POSES;
        const bool g = P->g[i];
        s->n++; s->ng += g;
        if (q > first && P->key[i] == P->key[(q - 1) % FL_POSES]) { s->same++; s->same_g += g; }
        P->tmp[n++] = (P->key[i] & ~1ull) | (uint64_t)g;   /* (a pose idle and in a gesture: two) */
    }
    qsort(P->tmp, (size_t)n, sizeof(uint64_t), cmp_u64);
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && P->tmp[j] == P->tmp[i]) j++;
        const bool g = P->tmp[i] & 1;
        if (!i || P->tmp[i] >> 1 != P->tmp[i - 1] >> 1) s->distinct++;
        if (g) s->distinct_g++; else s->distinct_i++;
        const uint32_t c = (uint32_t)(j - i);
        int at = s->ntop < FL_TOP ? s->ntop++ : FL_TOP;
        while (at > 0 && s->top[at - 1].n < c) { if (at < FL_TOP) s->top[at] = s->top[at - 1]; at--; }
        if (at < FL_TOP) { s->top[at].n = c; s->top[at].g = g; }
        i = j;
    }
    for (int c = 0; c < FL_LRU_N; c++) {      /* an LRU cache of FL_LRU[c] poses over the window, in order */
        const int N = FL_LRU[c];
        int used = 0, hits = 0;
        for (uint32_t q = first; q < end; q++) {
            const uint64_t k = P->key[q % FL_POSES];
            int at = -1, old = 0;
            for (int e = 0; e < used; e++) { if (P->lk[e] == k) { at = e; break; } if (P->la[e] < P->la[old]) old = e; }
            if (at >= 0) hits++;
            else { at = used < N ? used++ : old; P->lk[at] = k; }
            P->la[at] = q;
        }
        s->lru[c] = s->n ? (float)hits / (float)s->n : 0.f;
    }
    unlock();
    return true;
}

/* ── the rings' lives ─────────────────────────────────────────────────── */

void fps_lab_reconcile(void) {
    lock();
    const bool rec = fps_lab(FL_REC) != 0, pose = fps_lab(FL_POSE) != 0;
    fl_ring_t *R = atomic_load(&A.ring);
    if (rec && !R && (R = FL_CALLOC(sizeof(fl_ring_t)))) atomic_store(&A.ring, R);
    else if (!rec && R) { atomic_store(&A.ring, NULL); quiet(); FL_FREE(R); }
    fl_poses_t *P = atomic_load(&A.poses);
    if (pose && !P && (P = FL_CALLOC(sizeof(fl_poses_t)))) { L.pn = L.total = L.total_same = 0; L.plast = 0; atomic_store(&A.poses, P); }
    else if (!pose && P) { atomic_store(&A.poses, NULL); quiet(); FL_FREE(P); }
    unlock();
}

uint32_t fps_lab_bytes(void) {
    return (atomic_load(&A.ring) ? (uint32_t)sizeof(fl_ring_t) : 0) + (atomic_load(&A.poses) ? (uint32_t)sizeof(fl_poses_t) : 0);
}
