/* mole_diag — see mole_diag.h. Everything here is cheap by construction:
 * one memcpy per log line, two uint16 stores per frame, a 1 Hz sampler. */
#include "mole_diag.h"
#include "log_upload.h"
#include "heap_guard.h"
#include "sta_watch.h"
#include "loop_watch.h"
#include "net/network.h"
#include "net/console_store.h"
#include "mole_fluid.h"
#include "molty_board.h"
#include "pkg/pkg_cmd.h"
#include "pkg/pkg_store.h"
#include "bench_chart.h"
#include "link/link_client.h"
#include "link/link_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <math.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "ping/ping_sock.h"
#include "nvs.h"
#include "sdkconfig.h"

#define RING_SLOTS   400
#define SLOT_TEXT    152
#define RING_MAGIC   0x4D444C32u   /* "MDL2": slots carry a job tag, the header a boot + time sync */
#define FRAMES_N     1024          /* ~30 s of frames at 33 fps */
#define TL_N         180           /* 1 Hz timeline, 3 min */
#define MAX_TASKS    40
#define PERSIST_PATH "/spiffs/diag.log"
#define PERSIST_TMP  "/spiffs/diag.tmp"
/* SPIFFS writes freeze both cores (the flash cache off: flash_gate.h). Crashes/WDT/OTA are covered by the no-init RAM
 * ring, so the file only has to survive power loss: small, rare, never at boot, only while the screen is calm. The
 * policy and its numbers live in log_upload.h (LOG_PERSIST_*), host-tested. */
#define PERSIST_BYTES LOG_PERSIST_BYTES

typedef struct { uint32_t seq, ms; char job[LOG_TAG_MAX + 1]; char text[SLOT_TEXT]; } slot_t;
/* boot: the NVS boot count; sync_up/sync_wall: uptime ms + relay wall ms at this boot's
 * last welcome (0/0: none) — kept in no-init RAM, so a soft reset's previous ring still
 * knows its wall time */
typedef struct {
    uint32_t magic, boot, next, nslots, sync_up, pad;
    int64_t sync_wall;
    slot_t s[RING_SLOTS];
    uint32_t magic2;
} ring_t;

/* Survives software resets (panic, TWDT, esp_restart, OTA) — not power loss. */
static EXT_RAM_NOINIT_ATTR ring_t s_ring;
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_ring_ok;

static ring_t *s_prev;               /* previous boot's ring copy (PSRAM) */
static char *s_prev_file;            /* or: previous boot's persisted text */
static size_t s_prev_file_len;
static const char *s_prev_src = "none";

static log_jobs_t s_jobs;            /* the jobs running now (s_ring_mux) */
static atomic_bool s_upload_req;     /* a job ended: the link uploads new lines */

/* previous boot's lines for the relay (mole_diag_log_prev_frame), from the RAM copy or
 * parsed out of the SPIFFS file */
static log_line_t *s_prev_lines;
static size_t s_prev_n;
static uint32_t s_prev_boot, s_prev_sync_up;
static int64_t s_prev_sync_wall;

static atomic_bool s_persist_req;
static uint32_t s_persist_next;      /* ring seq at the last write (periodic writes only when wanted lines came) */
static TaskHandle_t s_persist_task;
static int64_t s_last_persist;
static uint32_t s_persist_n, s_persist_ms, s_persist_fail;

/* ── log ring ─────────────────────────────────────────────────────────────── */

static void ring_put(const char *text, size_t len)
{
    if (!s_ring_ok) return;
    while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) len--;
    if (!len) return;
    if (len >= SLOT_TEXT) len = SLOT_TEXT - 1;
    uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL_SAFE(&s_ring_mux);
    uint32_t seq = s_ring.next++;
    slot_t *sl = &s_ring.s[seq % RING_SLOTS];
    sl->seq = seq; sl->ms = ms;
    strcpy(sl->job, log_jobs_current(&s_jobs));
    memcpy(sl->text, text, len); sl->text[len] = 0;
    portEXIT_CRITICAL_SAFE(&s_ring_mux);
}

static void request_persist(void)
{
    atomic_store(&s_persist_req, true);
    if (s_persist_task) xTaskNotifyGive(s_persist_task);
}

/* Flood guard: an error repeating at frame rate must not wash the ring. */
#define LOG_LINES_PER_SEC 40
static uint32_t s_log_sec, s_log_in_sec, s_log_dropped;
#if !CONFIG_ESP_CONSOLE_NONE
/* the console's writer (UART0 on boards with a USB-UART bridge, e.g. the 1.28): lines
 * that pass the flood guard go there too, so a serial monitor shows the app's logs */
static vprintf_like_t s_log_console;
#endif

static int log_vprintf(const char *fmt, va_list ap)
{
    char buf[SLOT_TEXT];
    uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000);
    if (sec != s_log_sec) {
        uint32_t dropped = s_log_dropped;
        s_log_sec = sec; s_log_in_sec = 0; s_log_dropped = 0;
        if (dropped) {
            int k = snprintf(buf, sizeof(buf), "EVT log.flood dropped=%lu lines", (unsigned long)dropped);
            ring_put(buf, (size_t)k);
        }
    }
    if (++s_log_in_sec > LOG_LINES_PER_SEC) { s_log_dropped++; return 0; }
#if !CONFIG_ESP_CONSOLE_NONE
    if (s_log_console) { va_list cp; va_copy(cp, ap); s_log_console(fmt, cp); va_end(cp); }
#endif
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) return n;
    size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
    ring_put(buf, len);
    if (buf[0] == 'E' && buf[1] == ' ') request_persist();
    return n;
}

void mole_diag_event(bool important, const char *fmt, ...)
{
    char buf[SLOT_TEXT];
    memcpy(buf, "EVT ", 4);
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf + 4, sizeof(buf) - 4, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    size_t len = 4 + ((size_t)n < sizeof(buf) - 4 ? (size_t)n : sizeof(buf) - 5);
    ring_put(buf, len);
    if (important) request_persist();
}

static bool ring_valid(const ring_t *r)
{
    return r->magic == RING_MAGIC && r->magic2 == RING_MAGIC && r->nslots == RING_SLOTS && r->next < 0x7fffffff;
}

/* Render a ring's lines with seq >= since, oldest first, newest kept when
 * truncating to max_bytes. */
static char *render(const ring_t *r, uint32_t since, size_t max_bytes, uint32_t *next, bool json)
{
    uint32_t end = r->next;
    uint32_t first = end > RING_SLOTS ? end - RING_SLOTS : 0;
    if (since > first) first = since;
    if (first > end) first = end;
    char *out = heap_caps_malloc(max_bytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) return NULL;
    /* walk newest -> oldest to find where max_bytes is reached */
    size_t total = 0; uint32_t start = end;
    while (start > first) {
        const slot_t *sl = &r->s[(start - 1) % RING_SLOTS];
        size_t l = strnlen(sl->text, SLOT_TEXT) * (json ? 2 : 1) + (json ? 48 : 24) + LOG_TAG_MAX + 12;
        if (total + l > max_bytes) break;
        total += l; start--;
    }
    size_t o = 0;
    for (uint32_t q = start; q < end; q++) {
        slot_t sl;
        portENTER_CRITICAL_SAFE(&s_ring_mux);
        sl = r->s[q % RING_SLOTS];
        portEXIT_CRITICAL_SAFE(&s_ring_mux);
        if (sl.seq != q) continue;                     /* overwritten meanwhile */
        sl.text[SLOT_TEXT - 1] = 0; sl.job[LOG_TAG_MAX] = 0;
        if (!json) {
            /* a job's lines: "[j_<tag>] " before the text (display only) */
            int w = snprintf(out + o, max_bytes + 1 - o, "%lu %lu.%03lu %s%s%s%s\n", (unsigned long)sl.seq,
                             (unsigned long)(sl.ms / 1000), (unsigned long)(sl.ms % 1000),
                             sl.job[0] ? "[j_" : "", sl.job, sl.job[0] ? "] " : "", sl.text);
            if (w < 0 || (size_t)w >= max_bytes + 1 - o) break;
            o += (size_t)w;
        } else {
            int w = snprintf(out + o, max_bytes + 1 - o, "{\"seq\":%lu,\"ms\":%lu,%s%s%s\"msg\":\"",
                             (unsigned long)sl.seq, (unsigned long)sl.ms,
                             sl.job[0] ? "\"job\":\"" : "", sl.job, sl.job[0] ? "\"," : "");
            if (w < 0 || (size_t)w >= max_bytes + 1 - o) break;
            o += (size_t)w;
            for (const char *p = sl.text; *p && o + 8 < max_bytes; p++) {
                unsigned char c = (unsigned char)*p;
                if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
                else if (c < 0x20 || c == 0x7f) o += (size_t)snprintf(out + o, 8, "\\u%04x", c);
                else out[o++] = (char)c;
            }
            if (o + 4 > max_bytes) break;
            out[o++] = '"'; out[o++] = '}'; out[o++] = '\n';
        }
    }
    out[o] = 0;
    if (next) *next = end;
    return out;
}

char *mole_diag_log_text(uint32_t since, size_t max_bytes, uint32_t *next, bool json)
{
    if (!s_ring_ok) return NULL;
    return render(&s_ring, since, max_bytes, next, json);
}

/* ── frames ───────────────────────────────────────────────────────────────── */

typedef struct {
    uint16_t v[FRAMES_N];            /* interval, 0.1 ms units */
    uint32_t n, over50, over100, over250;
    uint32_t max_us, max_at_s;
    int64_t last;
    uint16_t win_max;                /* max interval this 1 s window, ms */
    uint16_t win_n;
} frames_t;
static frames_t s_sim, s_panel;
static uint32_t s_sim_work_us, s_draw_us, s_flush_us;
static atomic_uint s_gap_events;

static void frame_add(frames_t *f, uint32_t us, const char *what)
{
    uint32_t t = us / 100; if (t > 65535) t = 65535;
    f->v[f->n % FRAMES_N] = (uint16_t)t;
    f->n++;
    if (us > 50000) f->over50++;
    if (us > 100000) f->over100++;
    if (us > 250000) {
        f->over250++;
        /* rate-bounded (4/s, sim+panel together): a long OTA must not wash the ring */
        static uint32_t sec, in_sec;
        uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
        if (now_s != sec) { sec = now_s; in_sec = 0; }
        atomic_fetch_add(&s_gap_events, 1);
        if (++in_sec <= 4)
            mole_diag_event(false, "stall.%s gap=%lums", what, (unsigned long)(us / 1000));   /* (never asks for a write) */
    }
    if (us > f->max_us) { f->max_us = us; f->max_at_s = (uint32_t)(esp_timer_get_time() / 1000000); }
    uint32_t ms = us / 1000; if (ms > 65535) ms = 65535;
    if (ms > f->win_max) f->win_max = (uint16_t)ms;
    f->win_n++;
}

void mole_diag_sim_frame(uint32_t interval_us, uint32_t work_us)
{
    s_sim_work_us = work_us;
    frame_add(&s_sim, interval_us, "sim");
}

void mole_diag_panel_frame(uint32_t draw_us, uint32_t flush_us)
{
    int64_t now = esp_timer_get_time();
    s_draw_us = draw_us; s_flush_us = flush_us;
    if (s_panel.last) frame_add(&s_panel, (uint32_t)(now - s_panel.last), "panel");
    s_panel.last = now;
}

static uint16_t s_touch[64];
static uint32_t s_touch_n, s_touch_max;
void mole_diag_touch_latency(uint32_t us)
{
    uint32_t ms10 = us / 100; if (ms10 > 65535) ms10 = 65535;
    s_touch[s_touch_n % 64] = (uint16_t)ms10; s_touch_n++;
    if (us > s_touch_max) s_touch_max = us;
}

static int cmp_u16(const void *a, const void *b) { return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b; }

static cJSON *hist_json(const uint16_t *src, uint32_t total, uint32_t cap)
{
    cJSON *j = cJSON_CreateObject();
    uint32_t n = total < cap ? total : cap;
    if (!n) { cJSON_AddNumberToObject(j, "n", 0); return j; }
    uint16_t *v = heap_caps_malloc(n * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!v) return j;
    memcpy(v, src, n * sizeof(uint16_t));          /* ring order is irrelevant for percentiles */
    uint64_t sum = 0;
    uint32_t b[6] = {0};                            /* <=20, <=34, <=50, <=100, <=250, >250 ms */
    for (uint32_t i = 0; i < n; i++) {
        sum += v[i];
        uint16_t x = v[i];
        b[x <= 200 ? 0 : x <= 340 ? 1 : x <= 500 ? 2 : x <= 1000 ? 3 : x <= 2500 ? 4 : 5]++;
    }
    qsort(v, n, sizeof(uint16_t), cmp_u16);
    cJSON_AddNumberToObject(j, "n", n);
    cJSON_AddNumberToObject(j, "window_s", sum / 10000.0);
    cJSON_AddNumberToObject(j, "fps", sum ? n * 10000.0 / sum : 0);
    cJSON_AddNumberToObject(j, "mean_ms", sum / 10.0 / n);
    cJSON_AddNumberToObject(j, "p50_ms", v[n / 2] / 10.0);
    cJSON_AddNumberToObject(j, "p95_ms", v[(n * 95) / 100] / 10.0);
    cJSON_AddNumberToObject(j, "p99_ms", v[(n * 99) / 100] / 10.0);
    cJSON_AddNumberToObject(j, "max_ms", v[n - 1] / 10.0);
    cJSON *h = cJSON_AddArrayToObject(j, "hist");
    static const char *lbl[6] = {"<=20", "<=34", "<=50", "<=100", "<=250", ">250"};
    for (int i = 0; i < 6; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "le", lbl[i]); cJSON_AddNumberToObject(e, "n", b[i]);
        cJSON_AddItemToArray(h, e);
    }
    heap_caps_free(v);
    return j;
}

static cJSON *frames_json(const frames_t *f)
{
    cJSON *j = hist_json(f->v, f->n, FRAMES_N);
    cJSON_AddNumberToObject(j, "total", f->n);
    cJSON_AddNumberToObject(j, "over50_total", f->over50);
    cJSON_AddNumberToObject(j, "over100_total", f->over100);
    cJSON_AddNumberToObject(j, "over250_total", f->over250);
    cJSON_AddNumberToObject(j, "max_total_ms", f->max_us / 1000.0);
    cJSON_AddNumberToObject(j, "max_total_at_s", f->max_at_s);
    return j;
}

/* ── probes, sampler, timeline ────────────────────────────────────────────── */

static volatile uint32_t s_probe_max[2];         /* ms, max wake gap this window */
static volatile uint32_t s_probe_max_total[2];

static void probe_task(void *arg)
{
    int core = (int)(intptr_t)arg;
    int64_t last = esp_timer_get_time();
    for (;;) {
        vTaskDelay(1);
        int64_t now = esp_timer_get_time();
        uint32_t ms = (uint32_t)((now - last) / 1000);
        last = now;
        if (ms > s_probe_max[core]) s_probe_max[core] = ms;
        if (ms > s_probe_max_total[core]) s_probe_max_total[core] = ms;
    }
}

typedef struct {
    uint32_t t_s, int_min, int_largest, psram_free;
    uint16_t sim_n, panel_n, sim_max, panel_max, probe0, probe1, late;
    int16_t gw_rtt;                  /* max rtt this window, -1 = no reply */
    uint8_t gw_sent, gw_lost;
    int8_t rssi;
    uint8_t flags;                   /* bit0 link connected, bit1 audio busy */
    char top[2][12]; uint8_t top_pct[2];
} tl_t;
static uint32_t (*s_calm_ms)(void);  /* how long the screen has been calm (mole_diag_set_calm): when the costly looks may run */
static bool screen_calm(void) { return !s_calm_ms || s_calm_ms() > 0; }
static void flash_tick(void);
static tl_t *s_tl;                   /* PSRAM [TL_N] */
static uint32_t s_tl_n;

typedef struct { char name[16]; uint8_t pct, core, prio; uint32_t stack_free; } task_pct_t;
static task_pct_t s_tasks[MAX_TASKS];
static int s_ntasks;
static portMUX_TYPE s_task_mux = portMUX_INITIALIZER_UNLOCKED;

static atomic_uint s_ping_sent, s_ping_lost, s_ping_rtt_max, s_ping_rtt_last;
static uint32_t s_ping_lost_total, s_ping_sent_total;
static atomic_uint s_alloc_fail, s_alloc_fail_size, s_alloc_fail_caps;
static const char *volatile s_alloc_fail_fn;
static uint32_t s_alloc_fail_reported;
static uint32_t s_late_max_total;
static esp_ping_handle_t s_ping;
static atomic_bool s_ping_on;          /* the session runs (paused while the station is down) */
static sta_watch_t s_sta;              /* associated: only then is there an AP to ask for its rssi */
static portMUX_TYPE s_sta_mux = portMUX_INITIALIZER_UNLOCKED;
static heap_guard_t s_hg;              /* the sampler's */
static uint32_t s_int_blocks;          /* internal heap: allocated blocks (last sample) */
/* wedge watch (loop_watch.h): the sampler sends a no-op to each shared dispatcher every second */
ESP_EVENT_DEFINE_BASE(MOLE_DIAG_EVENT);
static loop_watch_t s_evt_watch, s_tmr_watch;
static esp_timer_handle_t s_beat_timer;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void evt_beat(void *a, esp_event_base_t b, int32_t id, void *d)
{
    (void)a; (void)b; (void)id; (void)d;
    loop_watch_beat(&s_evt_watch, now_ms());
}
static void tmr_beat(void *a) { (void)a; loop_watch_beat(&s_tmr_watch, now_ms()); }

/* a dispatcher stuck 30 s: abort, so the coredump shows who blocks it and on what */
static void wedge_check(void)
{
    const uint32_t t = now_ms();
    const uint32_t evt = loop_watch_stuck(&s_evt_watch, t), tmr = s_beat_timer ? loop_watch_stuck(&s_tmr_watch, t) : 0;
    if (evt || tmr) {
        mole_diag_event(true, "wedge %s stuck=%lums int=%lu", evt ? "sys_evt" : "esp_timer", (unsigned long)(evt ? evt : tmr),
                        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        esp_system_abort(evt ? "default event loop wedged 30 s" : "esp_timer task wedged 30 s");
    }
    /* no data, no wait: a full queue is a missed beat, not a blocked sampler */
    esp_event_post(MOLE_DIAG_EVENT, 0, NULL, 0, 0);
}

static void alloc_failed(size_t size, uint32_t caps, const char *fn)
{
    atomic_fetch_add(&s_alloc_fail, 1);
    atomic_store(&s_alloc_fail_size, (unsigned)size);
    atomic_store(&s_alloc_fail_caps, caps);
    s_alloc_fail_fn = fn;
}

static void sample_tasks(int64_t elapsed_us)
{
#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    static TaskStatus_t st[MAX_TASKS];
    static struct { UBaseType_t num; configRUN_TIME_COUNTER_TYPE rt; } prev[MAX_TASKS];
    static int nprev;
    configRUN_TIME_COUNTER_TYPE total;
    int n = (int)uxTaskGetSystemState(st, MAX_TASKS, &total);
    task_pct_t out[MAX_TASKS];
    int no = 0;
    for (int i = 0; i < n && no < MAX_TASKS; i++) {
        configRUN_TIME_COUNTER_TYPE d = st[i].ulRunTimeCounter;
        for (int k = 0; k < nprev; k++) if (prev[k].num == st[i].xTaskNumber) { d = st[i].ulRunTimeCounter - prev[k].rt; break; }
        uint32_t pct = elapsed_us > 0 ? (uint32_t)((uint64_t)d * 100 / (uint64_t)elapsed_us) : 0;
        if (pct > 100) pct = 100;
        strlcpy(out[no].name, st[i].pcTaskName, sizeof(out[no].name));
        out[no].pct = (uint8_t)pct;
        out[no].core = st[i].xCoreID > 1 ? 2 : (uint8_t)st[i].xCoreID;
        out[no].prio = (uint8_t)st[i].uxCurrentPriority;
        out[no].stack_free = st[i].usStackHighWaterMark;
        no++;
    }
    for (int i = 0; i < n && i < MAX_TASKS; i++) { prev[i].num = st[i].xTaskNumber; prev[i].rt = st[i].ulRunTimeCounter; }
    nprev = n < MAX_TASKS ? n : MAX_TASKS;
    /* sort by pct desc (tiny n) */
    for (int a = 0; a < no; a++) for (int b = a + 1; b < no; b++)
        if (out[b].pct > out[a].pct) { task_pct_t t = out[a]; out[a] = out[b]; out[b] = t; }
    portENTER_CRITICAL(&s_task_mux);
    memcpy(s_tasks, out, sizeof(task_pct_t) * (size_t)no); s_ntasks = no;
    portEXIT_CRITICAL(&s_task_mux);
#else
    (void)elapsed_us;
#endif
}

static int top_tasks_str(char *buf, size_t cap, int k)
{
    int o = 0;
    portENTER_CRITICAL(&s_task_mux);
    int shown = 0;
    for (int i = 0; i < s_ntasks && shown < k; i++) {
        if (!strncmp(s_tasks[i].name, "IDLE", 4)) continue;
        o += snprintf(buf + o, cap - (size_t)o, "%s%s:%u%%", shown ? "," : "", s_tasks[i].name, s_tasks[i].pct);
        if ((size_t)o >= cap) { o = (int)cap - 1; break; }
        shown++;
    }
    portEXIT_CRITICAL(&s_task_mux);
    return o;
}

static void ping_ok(esp_ping_handle_t h, void *a)
{
    (void)a; uint32_t t = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &t, sizeof(t));
    atomic_fetch_add(&s_ping_sent, 1);
    atomic_store(&s_ping_rtt_last, t);
    if (t > atomic_load(&s_ping_rtt_max)) atomic_store(&s_ping_rtt_max, t);
}
static void ping_timeout(esp_ping_handle_t h, void *a)
{
    (void)h; (void)a;
    atomic_fetch_add(&s_ping_sent, 1); atomic_fetch_add(&s_ping_lost, 1);
}

static void start_ping(void)
{
    if (s_ping) {                                  /* back after a drop: the same gateway, resumed */
        if (!atomic_exchange(&s_ping_on, true)) esp_ping_start(s_ping);
        return;
    }
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (!sta || esp_netif_get_ip_info(sta, &ip) != ESP_OK || !ip.gw.addr) return;
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = 1000; cfg.timeout_ms = 900;
    cfg.target_addr.type = IPADDR_TYPE_V4;
    cfg.target_addr.u_addr.ip4.addr = ip.gw.addr;
    cfg.task_stack_size = 3072; cfg.task_prio = 2;
    cfg.interface = esp_netif_get_netif_impl_index(sta);
    esp_ping_callbacks_t cbs = {.on_ping_success = ping_ok, .on_ping_timeout = ping_timeout};
    if (esp_ping_new_session(&cfg, &cbs, &s_ping) == ESP_OK) { esp_ping_start(s_ping); atomic_store(&s_ping_on, true); }
    else s_ping = NULL;
}

/* the station is gone: no gateway to ping (a send a second into a dead route, and its
 * "ping_sock: send error" line, while the outage lasts) */
static void pause_ping(void)
{
    if (s_ping && atomic_exchange(&s_ping_on, false)) esp_ping_stop(s_ping);
}

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *d = data;
            /* once per outage: a reconnect that fails again (every 1..30 s, net/wifi_retry.h) is not news */
            portENTER_CRITICAL(&s_sta_mux);
            bool news = sta_watch_event(&s_sta, STA_WATCH_EV_DISCONNECTED);
            portEXIT_CRITICAL(&s_sta_mux);
            if (news) mole_diag_event(true, "wifi.disconnected reason=%u rssi=%d", d ? d->reason : 0, d ? d->rssi : 0);
            pause_ping();
        } else if (id == WIFI_EVENT_STA_CONNECTED) {
            portENTER_CRITICAL(&s_sta_mux);
            sta_watch_event(&s_sta, STA_WATCH_EV_CONNECTED);
            portEXIT_CRITICAL(&s_sta_mux);
            mole_diag_event(false, "wifi.connected");
        } else if (id == WIFI_EVENT_STA_BEACON_TIMEOUT) mole_diag_event(true, "wifi.beacon_timeout");
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) { mole_diag_event(false, "wifi.got_ip"); start_ping(); }
        else if (id == IP_EVENT_STA_LOST_IP) { mole_diag_event(true, "wifi.lost_ip"); pause_ping(); }
    }
}

static bool sta_up(void)
{
    portENTER_CRITICAL(&s_sta_mux);
    bool up = sta_watch_up(&s_sta);
    portEXIT_CRITICAL(&s_sta_mux);
    return up;
}

static int8_t rssi_now(void)
{
    /* not associated: 0 without asking — the driver answers that call with a
     * "W wifi: Haven't to connect to a suitable AP now!" line, once a second for an outage */
    if (!sta_up()) return 0;
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

/* The heap guard's report (heap_guard.h): what holds internal RAM that could give some back,
 * and what the link / audio were doing. */
static void heap_low_report(uint32_t free_now, const multi_heap_info_t *in)
{
    char top[64]; top_tasks_str(top, sizeof(top), 3);
    mole_diag_event(true, "heap.low int=%lu largest=%u blk=%u fluid_int=%d wifi=%d link=%d audio=%d top=%s",
                    (unsigned long)free_now, (unsigned)in->largest_free_block, (unsigned)in->allocated_blocks,
                    mole_fluid_internal_fields(), sta_up() ? 1 : 0, link_client_connected() ? 1 : 0,
                    link_audio_busy() ? 1 : 0, top);
}

/* the panel's tearing-effect pulses (molty_display_te): the last second's, and the 10 s the TE log line covers */
static molty_te_t s_te_sec;
static void te_json(cJSON *o)
{
    const molty_te_t t = s_te_sec;
    cJSON_AddBoolToObject(o, "line", t.line);
    cJSON_AddNumberToObject(o, "total", t.total);
    cJSON_AddNumberToObject(o, "per_s", t.n);                     /* (periods seen in the last second: its refreshes) */
    if (!t.n) { cJSON_AddStringToObject(o, "note", t.line ? "no pulses" : "no TE line"); return; }
    cJSON_AddNumberToObject(o, "hz", t.mean_us ? 1e6 / t.mean_us : 0);
    cJSON_AddNumberToObject(o, "mean_us", t.mean_us); cJSON_AddNumberToObject(o, "min_us", t.min_us); cJSON_AddNumberToObject(o, "max_us", t.max_us);
}

static void sampler_task(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    int64_t prev = esp_timer_get_time(), win_start = prev;
    uint32_t int_min = UINT32_MAX, late_max = 0, ticks = 0;
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(100));
        int64_t now = esp_timer_get_time();
        uint32_t late = (uint32_t)((now - prev) / 1000);
        late = late > 100 ? late - 100 : 0;
        prev = now;
        if (late > late_max) late_max = late;
        if (late > s_late_max_total) s_late_max_total = late;
        if (late > 250) mole_diag_event(false, "stall.sampler late=%lums", (unsigned long)late);   /* (a stall's record must not cause the next) */
        flash_tick();
        uint32_t f = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (f < int_min) int_min = f;
        if (++ticks % 10) continue;
        static molty_te_t te10;                      /* (the TE line's 10 s, for its log line) */
        molty_display_te(&s_te_sec, true);
        if (s_te_sec.n) {
            te10.min_us = !te10.n || s_te_sec.min_us < te10.min_us ? s_te_sec.min_us : te10.min_us;
            te10.max_us = s_te_sec.max_us > te10.max_us ? s_te_sec.max_us : te10.max_us;
            te10.mean_us = (uint32_t)(((uint64_t)te10.mean_us * te10.n + (uint64_t)s_te_sec.mean_us * s_te_sec.n) / (te10.n + s_te_sec.n));
            te10.n += s_te_sec.n;
        }

        int64_t elapsed = now - win_start; win_start = now;
        wedge_check();
        tl_t e = {0};
        e.t_s = (uint32_t)(now / 1000000);
        e.int_min = int_min; int_min = UINT32_MAX;
        /* one walk for both: the largest block and the allocated-block count (a leak of small
         * blocks shows as blk climbing while int falls; heap_caps_get_largest_free_block walks too) */
        multi_heap_info_t in;
        heap_caps_get_info(&in, MALLOC_CAP_INTERNAL);
        e.int_largest = (uint32_t)in.largest_free_block;
        s_int_blocks = (uint32_t)in.allocated_blocks;
        e.psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        e.sim_n = s_sim.win_n; e.sim_max = s_sim.win_max; s_sim.win_n = 0; s_sim.win_max = 0;
        e.panel_n = s_panel.win_n; e.panel_max = s_panel.win_max; s_panel.win_n = 0; s_panel.win_max = 0;
        e.probe0 = (uint16_t)s_probe_max[0]; e.probe1 = (uint16_t)s_probe_max[1];
        s_probe_max[0] = s_probe_max[1] = 0;
        e.late = (uint16_t)late_max; late_max = 0;
        uint32_t sent = atomic_exchange(&s_ping_sent, 0), lost = atomic_exchange(&s_ping_lost, 0);
        uint32_t rtt = atomic_exchange(&s_ping_rtt_max, 0);
        s_ping_sent_total += sent; s_ping_lost_total += lost;
        e.gw_sent = (uint8_t)sent; e.gw_lost = (uint8_t)lost;
        e.gw_rtt = sent > lost ? (int16_t)(rtt > 32767 ? 32767 : rtt) : -1;
        e.rssi = rssi_now();
        e.flags = (link_client_connected() ? 1 : 0) | (link_audio_busy() ? 2 : 0);

        /* internal RAM self-protection: warn once per episode, give the fluid's share back */
        const int hg = heap_guard_step(&s_hg, f);
        if (hg & HG_LOW) heap_low_report(f, &in);
        if (hg & HG_SHED) mole_fluid_shed_internal();
        if (hg & HG_OK) mole_diag_event(false, "heap.ok int=%lu after episode %lu", (unsigned long)f, (unsigned long)s_hg.episodes);

        uint32_t fails = atomic_load(&s_alloc_fail);
        if (fails != s_alloc_fail_reported) {
            mole_diag_event(true, "heap.alloc_fail n=%lu last_size=%u caps=0x%x fn=%s int_free=%lu",
                            (unsigned long)(fails - s_alloc_fail_reported), atomic_load(&s_alloc_fail_size),
                            atomic_load(&s_alloc_fail_caps), s_alloc_fail_fn ? s_alloc_fail_fn : "?",
                            (unsigned long)e.int_min);
            s_alloc_fail_reported = fails;
        }
        bool bad = e.sim_max > 250 || e.panel_max > 250 || e.probe0 > 250 || e.probe1 > 250 || e.late > 250 ||
                   (sent && lost == sent);
        /* the tasks' shares (since the last look): on trouble, else with a TL line while the screen is quiet (or a minute
         * on), never each second: the snapshot holds the kernel lock while it measures every task's free stack (~2.6 ms
         * measured, both cores' scheduling held: a frame's hitch each second); the timeline's other seconds carry none */
        static int64_t sampled;
        if (bad || (ticks % 100 == 0 && (screen_calm() || now - sampled > 60000000))) {
            sample_tasks(sampled ? now - sampled : elapsed); sampled = now;
            portENTER_CRITICAL(&s_task_mux);
            for (int i = 0, k = 0; i < s_ntasks && k < 2; i++) {
                if (!strncmp(s_tasks[i].name, "IDLE", 4)) continue;
                strlcpy(e.top[k], s_tasks[i].name, sizeof(e.top[k])); e.top_pct[k] = s_tasks[i].pct; k++;
            }
            portEXIT_CRITICAL(&s_task_mux);
        }
        if (s_tl) s_tl[s_tl_n % TL_N] = e;
        s_tl_n++;
        if (bad || ticks % 100 == 0) {           /* every 10 s, or immediately on trouble */
            char top[96]; top_tasks_str(top, sizeof(top), 3);
            mole_diag_event(false, "%s int=%lu/%lu blk=%lu sim=%u/%ums pan=%u/%ums pr=%u/%ums late=%u gw=%d(%u/%u) rssi=%d l=%u %s",
                            bad ? "TL!" : "TL", (unsigned long)e.int_min, (unsigned long)e.int_largest, (unsigned long)s_int_blocks,
                            e.sim_n, e.sim_max, e.panel_n, e.panel_max, e.probe0, e.probe1, e.late,
                            e.gw_rtt, (unsigned)lost, (unsigned)sent, e.rssi, e.flags, top);
        }
        if (ticks % 100 == 0) {                  /* the panel's refresh, beside the TL line */
            if (te10.n) mole_diag_event(false, "TE %.2f Hz period=%lu/%lu/%lu us (mean/min/max) n=%lu total=%lu", 1e6 / te10.mean_us,
                                        (unsigned long)te10.mean_us, (unsigned long)te10.min_us, (unsigned long)te10.max_us, (unsigned long)te10.n, (unsigned long)s_te_sec.total);
            else mole_diag_event(false, "TE %s (total=%lu)", s_te_sec.line ? "no pulses" : "no line", (unsigned long)s_te_sec.total);
            te10 = (molty_te_t){ 0 };
        }
    }
}

/* ── persistence ──────────────────────────────────────────────────────────── */

/* The file format (log_upload.h): a header with this boot and its time sync, then
 * "<seq> <ms> <tag|-> <text>" lines, oldest first, the newest PERSIST_BYTES. */
static char *render_persist(uint32_t *next)
{
    char *out = heap_caps_malloc(PERSIST_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) return NULL;
    portENTER_CRITICAL_SAFE(&s_ring_mux);
    uint32_t end = s_ring.next, boot = s_ring.boot, sync_up = s_ring.sync_up;
    int64_t sync_wall = s_ring.sync_wall;
    portEXIT_CRITICAL_SAFE(&s_ring_mux);
    int h = log_persist_header(out, PERSIST_BYTES, boot, sync_up, sync_wall);
    if (h < 0) { heap_caps_free(out); return NULL; }
    size_t o = (size_t)h;
    out[o++] = '\n';
    uint32_t first = end > RING_SLOTS ? end - RING_SLOTS : 0;
    size_t total = o; uint32_t start = end;
    while (start > first) {
        const slot_t *sl = &s_ring.s[(start - 1) % RING_SLOTS];
        size_t l = strnlen(sl->text, SLOT_TEXT) + strnlen(sl->job, LOG_TAG_MAX + 1) + 26;
        if (total + l > PERSIST_BYTES) break;
        total += l; start--;
    }
    for (uint32_t q = start; q < end; q++) {
        slot_t sl;
        portENTER_CRITICAL_SAFE(&s_ring_mux);
        sl = s_ring.s[q % RING_SLOTS];
        portEXIT_CRITICAL_SAFE(&s_ring_mux);
        if (sl.seq != q) continue;
        sl.text[SLOT_TEXT - 1] = 0; sl.job[LOG_TAG_MAX] = 0;
        log_line_t l = {.seq = sl.seq, .ms = sl.ms, .job = sl.job, .text = sl.text};
        int w = log_persist_line(out + o, PERSIST_BYTES - o, &l);
        if (w < 0 || o + (size_t)w + 1 > PERSIST_BYTES) break;
        o += (size_t)w;
        out[o++] = '\n';
    }
    out[o] = 0;
    *next = end;
    return out;
}

/* ── the flash's quiet window (flash_gate.h) ─────────────────────────────────
 * Every write and erase of a partition (NVS, SPIFFS, OTA, the PHY's and Wi-Fi's NVS: all go through esp_partition_*;
 * the core dump writes the chip directly and is not seen) passes these wrappers (main/CMakeLists.txt --wrap): counted,
 * and logged a batch a line from the sampler ("flash <task> w= e= r= ops= <ms>ms anim=<writes while animating>"). */
static portMUX_TYPE s_fg_mux = portMUX_INITIALIZER_UNLOCKED;
static fg_stats_t s_fg;                         /* (s_fg_mux) */
static fg_batch_t s_fg_batch, s_fg_done;        /* (s_fg_mux) the batch open; one closed, for the sampler to log */
static bool s_fg_done_set;
static atomic_bool s_fg_force, s_fg_flushed;
static void *s_fg_flusher;
/* the tasks inside mole_flash_file: each op of theirs waits while the screen moves (gated_op); one in flight (s_fg_op)
 * is what a motion starting waits for (mole_flash_motion), the motion's own time (s_fg_motion_ms) what they wait on */
static TaskHandle_t _Atomic s_fg_gated[2];
static atomic_bool s_fg_op;
static atomic_uint s_fg_motion_ms;
#define FG_OP_PAUSE_MAX_MS 5000                 /* an op waits this long in all, a file write's (SPIFFS is locked meanwhile) */
#define FG_MOTION_WAIT_MS  50                   /* a motion waits this long for an op in flight (a sector erase: ~45 ms) */

void mole_diag_set_calm(uint32_t (*calm_ms)(void)) { s_calm_ms = calm_ms; }
static int64_t calm_us(void)
{
    const uint32_t ms = s_calm_ms ? s_calm_ms() : UINT32_MAX;
    return ms == UINT32_MAX ? INT64_MAX : (int64_t)ms * 1000;
}

static void flash_note(int64_t t0, bool anim0, uint32_t w, uint32_t e, uint32_t r)
{
    const int64_t t1 = esp_timer_get_time();
    const bool anim = anim0 || calm_us() == 0;
    const char *who = pcTaskGetName(NULL);
    portENTER_CRITICAL(&s_fg_mux);
    if (w || e) { s_fg.ops++; s_fg.anim_ops += anim; }
    /* (closed straight into the sampler's slot, no copy on this task's stack: it drains it every 100 ms, well inside a gap) */
    if (fg_batch_op(&s_fg_batch, &s_fg_done, t1, who, w, e, r, (uint32_t)(t1 - t0), anim)) s_fg_done_set = fg_batch_worth(&s_fg_done);
    portEXIT_CRITICAL(&s_fg_mux);
}

esp_err_t __real_esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t n);
esp_err_t __real_esp_partition_write_raw(const esp_partition_t *p, size_t off, const void *src, size_t n);
esp_err_t __real_esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t n);
esp_err_t __real_esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t n);
static bool gated_self(void)
{
    const TaskHandle_t me = xTaskGetCurrentTaskHandle();
    return atomic_load(&s_fg_gated[0]) == me || atomic_load(&s_fg_gated[1]) == me;
}
static uint32_t s_fg_paused_ms;                 /* (the gated task's, this file write's: they are serial under SPIFFS's lock) */
/* A gated task's op: not while the screen moves or a motion just began, FG_OP_PAUSE_MAX_MS in all; s_fg_op set first
 * (a motion sets its time first, then looks at s_fg_op: one of the two sees the other) */
static void gated_op(void)
{
    for (;;) {
        atomic_store(&s_fg_op, true);
        const bool moving = calm_us() == 0 || now_ms() - atomic_load(&s_fg_motion_ms) < 100;
        if (!moving || atomic_load(&s_fg_force) || s_fg_paused_ms >= FG_OP_PAUSE_MAX_MS) return;
        atomic_store(&s_fg_op, false);
        vTaskDelay(pdMS_TO_TICKS(10));
        s_fg_paused_ms += 10;
    }
}
void mole_flash_motion(void)
{
    atomic_store(&s_fg_motion_ms, now_ms());
    for (int i = 0; i < FG_MOTION_WAIT_MS / 10 && atomic_load(&s_fg_op); i++) vTaskDelay(pdMS_TO_TICKS(10));   /* (a tick) */
}
#define FLASH_OP(call, w, e, r) do { const bool g = gated_self(); if (g) gated_op(); \
    const int64_t t0 = esp_timer_get_time(); const bool a0 = calm_us() == 0; \
    const esp_err_t res = call; if (g) atomic_store(&s_fg_op, false); flash_note(t0, a0, w, e, r); return res; } while (0)
esp_err_t __wrap_esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t n)
{ FLASH_OP(__real_esp_partition_write(p, off, src, n), n, 0, 0); }
esp_err_t __wrap_esp_partition_write_raw(const esp_partition_t *p, size_t off, const void *src, size_t n)
{ FLASH_OP(__real_esp_partition_write_raw(p, off, src, n), n, 0, 0); }
esp_err_t __wrap_esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t n)
{ FLASH_OP(__real_esp_partition_erase_range(p, off, n), 0, n, 0); }
esp_err_t __wrap_esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t n)
{ FLASH_OP(__real_esp_partition_read(p, off, dst, n), 0, 0, n); }

/* the sampler, every 100 ms: a batch over goes into the log */
static void flash_tick(void)
{
    fg_batch_t b;
    portENTER_CRITICAL(&s_fg_mux);
    bool got = s_fg_done_set;
    if (got) { b = s_fg_done; s_fg_done_set = false; }
    else got = fg_batch_idle(&s_fg_batch, &b, esp_timer_get_time()) && fg_batch_worth(&b);
    portEXIT_CRITICAL(&s_fg_mux);
    if (!got) return;
    mole_diag_event(false, "flash %s%s w=%lu e=%lu r=%lu ops=%lu %lums (reads %lums) anim=%lu", b.who, b.mixed ? "+" : "",
                    (unsigned long)b.wbytes, (unsigned long)b.ebytes, (unsigned long)b.rbytes, (unsigned long)b.ops,
                    (unsigned long)(b.busy_us / 1000), (unsigned long)(b.read_us / 1000), (unsigned long)b.anim_ops);
}

bool mole_flash_may(fg_hold_t *h)
{
    const int64_t c = calm_us();
    portENTER_CRITICAL(&s_fg_mux);
    const bool go = fg_step(h, &s_fg, esp_timer_get_time(), c, atomic_load(&s_fg_force));
    portEXIT_CRITICAL(&s_fg_mux);
    return go;
}

void mole_flash_quiet(void)
{
    fg_hold_t h = {0};
    while (!mole_flash_may(&h)) vTaskDelay(pdMS_TO_TICKS(100));
}

static bool flash_file(const char *path, const char *tmp, const void *data, size_t len)
{
    enum { CHUNK = 2048 };
    mole_flash_quiet();
    esp_spiffs_gc("spiffs", len);                      /* the erases it needs, now while calm (none when there is room) */
    mole_flash_quiet();
    FILE *f = fopen(tmp, "wb");
    bool ok = f != NULL;
    for (size_t o = 0; ok && o < len; o += CHUNK) {
        const size_t c = len - o < CHUNK ? len - o : CHUNK;
        if (o) { vTaskDelay(pdMS_TO_TICKS(20)); mole_flash_quiet(); }   /* (frames between the bursts; one may start) */
        ok = fwrite((const char *)data + o, 1, c, f) == c && fflush(f) == 0;
    }
    if (f) ok = fclose(f) == 0 && ok;
    mole_flash_quiet();
    if (ok) { remove(path); ok = rename(tmp, path) == 0; }   /* (SPIFFS cannot rename over a file) */
    if (!ok) remove(tmp);
    return ok;
}

bool mole_flash_file(const char *path, const char *tmp, const void *data, size_t len)
{
    const TaskHandle_t me = xTaskGetCurrentTaskHandle();
    int slot = -1;
    for (int q = 0; q < 2 && slot < 0; q++) { TaskHandle_t none = NULL; if (atomic_compare_exchange_strong(&s_fg_gated[q], &none, me)) slot = q; }
    s_fg_paused_ms = 0;
    const bool ok = flash_file(path, tmp, data, len);   /* (no slot: its steps are gated, its ops not) */
    if (slot >= 0) atomic_store(&s_fg_gated[slot], NULL);
    return ok;
}

void mole_flash_force(void) { atomic_store(&s_fg_force, true); }
bool mole_flash_forced(void) { return atomic_load(&s_fg_force); }
void mole_flash_flusher(void *task) { s_fg_flusher = task; }
void mole_flash_flushed(void) { atomic_store(&s_fg_flushed, true); }

/* esp_restart's shutdown handler: the flusher writes what a restart loses (on its own task: this one's stack may be small
 * or in PSRAM), 1.5 s at most */
static void flash_shutdown(void)
{
    atomic_store(&s_fg_flushed, false);                /* (first: a flusher that saw the force after it says so after) */
    mole_flash_force();
    TaskHandle_t t = s_fg_flusher;
    if (!t || t == xTaskGetCurrentTaskHandle() || xPortInIsrContext() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return;
    xTaskNotifyGive(t);
    for (int i = 0; i < 30 && !atomic_load(&s_fg_flushed); i++) vTaskDelay(pdMS_TO_TICKS(50));
}

/* ── persistence ── */

static void persist_now(void)
{
    int64_t t0 = esp_timer_get_time();
    uint32_t next;
    char *text = render_persist(&next);
    if (!text) { s_persist_fail++; return; }
    const bool ok = mole_flash_file(PERSIST_PATH, PERSIST_TMP, text, strlen(text));
    heap_caps_free(text);
    if (!ok) { s_persist_fail++; return; }
    s_persist_next = next;
    s_persist_n++;
    s_persist_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
}

/* lines worth uploading logged since the last write (the periodic write skips an idle log) */
static uint32_t wanted_since(uint32_t from)
{
    uint32_t end = s_ring.next, n = 0;
    if (end > RING_SLOTS && from < end - RING_SLOTS) from = end - RING_SLOTS;
    for (uint32_t q = from; q < end; q++) {
        slot_t sl;
        portENTER_CRITICAL_SAFE(&s_ring_mux);
        sl = s_ring.s[q % RING_SLOTS];
        portEXIT_CRITICAL_SAFE(&s_ring_mux);
        sl.text[SLOT_TEXT - 1] = 0; sl.job[LOG_TAG_MAX] = 0;
        if (sl.seq == q && log_upload_wanted(sl.text, sl.job)) n++;
    }
    return n;
}

void mole_diag_flush(uint32_t wait_ms)
{
    if (!s_persist_task) return;
    const uint32_t n = s_persist_n + s_persist_fail;
    mole_flash_force();
    request_persist();
    for (uint32_t t = 0; t < wait_ms && s_persist_n + s_persist_fail == n; t += 50) vTaskDelay(pdMS_TO_TICKS(50));
}

static void persist_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LOG_PERSIST_PERIOD_MS));
        bool requested = atomic_load(&s_persist_req);
        if (!mole_flash_forced() && !log_persist_due(requested, wanted_since(s_persist_next))) continue;
        /* never in the first 90 s, at least 5 min apart: sleep it out, the request stays pending (a flush wakes it) */
        for (int64_t wait; !mole_flash_forced() && (wait = log_persist_delay_us(esp_timer_get_time(), s_last_persist)) > 0;)
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait / 1000 + 10));
        atomic_store(&s_persist_req, false);
        s_last_persist = esp_timer_get_time();
        persist_now();                                 /* (each step only while the screen is calm: mole_flash_file) */
    }
}

/* ── activity log: job tags, time sync, uploads (log_upload.h) ────────────── */

void mole_diag_job_begin(const char *job)
{
    char tag[LOG_TAG_MAX + 1];
    if (!log_job_tag(job, tag)) return;
    portENTER_CRITICAL_SAFE(&s_ring_mux);
    log_jobs_begin(&s_jobs, tag);
    portEXIT_CRITICAL_SAFE(&s_ring_mux);
}

void mole_diag_job_end(const char *job)
{
    char tag[LOG_TAG_MAX + 1];
    if (!log_job_tag(job, tag)) return;
    portENTER_CRITICAL_SAFE(&s_ring_mux);
    log_jobs_end(&s_jobs, tag);
    portEXIT_CRITICAL_SAFE(&s_ring_mux);
    atomic_store(&s_upload_req, true);       /* the link sends the job's lines up */
}

bool mole_diag_log_take_request(void) { return atomic_exchange(&s_upload_req, false); }

void mole_diag_link_sync(uint32_t up_ms, int64_t wall_ms)
{
    portENTER_CRITICAL_SAFE(&s_ring_mux);
    s_ring.sync_up = up_ms; s_ring.sync_wall = wall_ms;
    portEXIT_CRITICAL_SAFE(&s_ring_mux);
}

/* The wanted lines of a ring from *cursor: backlog > 0 first moves the cursor so that at
 * most `backlog` remain; then up to max_lines of them, framed. */
static cJSON *ring_frame(const ring_t *r, bool live, uint32_t *cursor, int max_lines, int backlog,
                         const log_frame_opts_t *o)
{
    uint32_t end = r->next;
    uint32_t first = end > RING_SLOTS ? end - RING_SLOTS : 0;
    if (*cursor < first) *cursor = first;
    if (*cursor >= end) { *cursor = end; return NULL; }
    if (backlog > 0) {
        uint32_t *seqs = heap_caps_malloc(RING_SLOTS * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!seqs) return NULL;
        size_t n = 0;
        for (uint32_t q = *cursor; q < end; q++) {
            slot_t sl;
            if (live) portENTER_CRITICAL_SAFE(&s_ring_mux);
            sl = r->s[q % RING_SLOTS];
            if (live) portEXIT_CRITICAL_SAFE(&s_ring_mux);
            sl.text[SLOT_TEXT - 1] = 0; sl.job[LOG_TAG_MAX] = 0;
            if (sl.seq == q && log_upload_wanted(sl.text, sl.job)) seqs[n++] = q;
        }
        *cursor = log_upload_backlog(seqs, n, *cursor, (size_t)backlog);
        heap_caps_free(seqs);
    }
    int cap = max_lines < 1 ? 1 : max_lines > LOG_UPLOAD_LINES_MAX ? LOG_UPLOAD_LINES_MAX : max_lines;
    slot_t *got = heap_caps_malloc((size_t)(cap + 1) * sizeof(slot_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    log_line_t *lines = heap_caps_malloc((size_t)(cap + 1) * sizeof(log_line_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!got || !lines) { heap_caps_free(got); heap_caps_free(lines); return NULL; }
    /* cap + 1 wanted lines: the extra one tells the framer where the next frame starts */
    size_t n = 0;
    uint32_t q = *cursor;
    for (; q < end && n < (size_t)cap + 1; q++) {
        slot_t *sl = &got[n];
        if (live) portENTER_CRITICAL_SAFE(&s_ring_mux);
        *sl = r->s[q % RING_SLOTS];
        if (live) portEXIT_CRITICAL_SAFE(&s_ring_mux);
        sl->text[SLOT_TEXT - 1] = 0; sl->job[LOG_TAG_MAX] = 0;
        if (sl->seq != q || !log_upload_wanted(sl->text, sl->job)) continue;
        lines[n] = (log_line_t){.seq = sl->seq, .ms = sl->ms, .job = sl->job, .text = sl->text};
        n++;
    }
    cJSON *f = log_upload_frame(lines, n, o, n > (size_t)cap ? lines[cap].seq : q, cursor);
    heap_caps_free(got); heap_caps_free(lines);
    return f;
}

cJSON *mole_diag_log_frame(uint32_t *cursor, int max_lines, int backlog)
{
    if (!s_ring_ok) return NULL;
    log_frame_opts_t o = {.boot = s_ring.boot, .up_ms = (uint32_t)(esp_timer_get_time() / 1000), .max_lines = max_lines};
    return ring_frame(&s_ring, true, cursor, max_lines, backlog, &o);
}

cJSON *mole_diag_log_prev_frame(uint32_t *cursor, int max_lines, int backlog)
{
    log_frame_opts_t o = {.boot = s_prev_boot, .up_ms = (uint32_t)(esp_timer_get_time() / 1000), .prev = true,
                          .has_sync = s_prev_sync_wall > 0, .sync_up_ms = s_prev_sync_up,
                          .sync_wall_ms = s_prev_sync_wall, .max_lines = max_lines};
    if (s_prev) return ring_frame(s_prev, false, cursor, max_lines, backlog, &o);
    if (!s_prev_lines || !s_prev_n) return NULL;
    uint32_t end = s_prev_lines[s_prev_n - 1].seq + 1;
    if (backlog > 0) {
        uint32_t *seqs = heap_caps_malloc(s_prev_n * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!seqs) return NULL;
        size_t n = 0;
        for (size_t i = 0; i < s_prev_n; i++)
            if (log_upload_wanted(s_prev_lines[i].text, s_prev_lines[i].job)) seqs[n++] = s_prev_lines[i].seq;
        *cursor = log_upload_backlog(seqs, n, *cursor, (size_t)backlog);
        heap_caps_free(seqs);
    }
    return log_upload_frame(s_prev_lines, s_prev_n, &o, end, cursor);
}

/* After power loss: the SPIFFS copy's lines, parsed in place (s_prev_file stays the raw text
 * /api/log/prev serves, so parse a second copy). An older file without the header: no upload. */
static void parse_prev_file(void)
{
    if (!s_prev_file) return;
    char *copy = heap_caps_malloc(s_prev_file_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) return;
    memcpy(copy, s_prev_file, s_prev_file_len + 1);
    char *nl = strchr(copy, '\n');
    if (!nl) { heap_caps_free(copy); return; }
    *nl = 0;
    if (!log_persist_parse_header(copy, &s_prev_boot, &s_prev_sync_up, &s_prev_sync_wall)) { heap_caps_free(copy); return; }
    size_t cap = 0;
    for (char *p = nl + 1; *p; p++) if (*p == '\n') cap++;
    s_prev_lines = heap_caps_malloc((cap + 1) * sizeof(log_line_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_prev_lines) { heap_caps_free(copy); return; }
    for (char *line = nl + 1; *line; ) {
        char *e = strchr(line, '\n');
        if (e) *e = 0;
        log_line_t l;
        if (log_persist_parse_line(line, &l) && (!s_prev_n || l.seq > s_prev_lines[s_prev_n - 1].seq)) s_prev_lines[s_prev_n++] = l;
        if (!e) break;
        line = e + 1;
    }
    /* `copy` stays: the lines point into it */
}

/* ── boot ─────────────────────────────────────────────────────────────────── */

static const char *reset_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_UNKNOWN:    return "unknown";
    case ESP_RST_POWERON:    return "poweron";
    case ESP_RST_EXT:        return "ext";
    case ESP_RST_SW:         return "sw";          /* esp_restart(), e.g. after OTA */
    case ESP_RST_PANIC:      return "panic";
    case ESP_RST_INT_WDT:    return "int_wdt";
    case ESP_RST_TASK_WDT:   return "task_wdt";
    case ESP_RST_WDT:        return "wdt";
    case ESP_RST_DEEPSLEEP:  return "deepsleep";
    case ESP_RST_BROWNOUT:   return "brownout";
    case ESP_RST_SDIO:       return "sdio";
    case ESP_RST_USB:        return "usb";
    case ESP_RST_JTAG:       return "jtag";
    case ESP_RST_EFUSE:      return "efuse";
    case ESP_RST_PWR_GLITCH: return "pwr_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default:                 return "other";
    }
}

const char *mole_diag_reset_reason(void) { return reset_str(esp_reset_reason()); }

/* Boot counter, NVS "sys"/"boot_count": tells a reboot happened even between polls. */
static uint32_t s_boot_count;

void mole_diag_boot_count_bump(void)
{
    nvs_handle_t h;
    if (nvs_open("sys", NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t count = 0;
    nvs_get_u32(h, "boot_count", &count);   /* NOT_FOUND leaves 0 (first boot) */
    count++;
    if (nvs_set_u32(h, "boot_count", count) == ESP_OK) nvs_commit(h);
    nvs_close(h);
    s_boot_count = count;
    s_ring.boot = count;                    /* the ring (and its persisted copy) know their boot */
}

uint32_t mole_diag_boot_count(void) { return s_boot_count; }

static char s_core_summary[200];
static void *psram_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

void mole_diag_early_init(void)
{
    if (ring_valid(&s_ring)) {
        s_prev = heap_caps_malloc(sizeof(ring_t), MALLOC_CAP_SPIRAM);
        if (s_prev) {
            memcpy(s_prev, &s_ring, sizeof(ring_t)); s_prev_src = "ram";
            s_prev_boot = s_prev->boot; s_prev_sync_up = s_prev->sync_up; s_prev_sync_wall = s_prev->sync_wall;
        }
    }
    memset(&s_ring, 0, sizeof(s_ring));
    s_ring.magic = RING_MAGIC; s_ring.magic2 = RING_MAGIC; s_ring.nslots = RING_SLOTS;
    s_ring_ok = true;
    /* cJSON trees (link frames, /api/creature/state, diag) are transient and
     * large: keep them out of the internal heap. */
    static cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
#if !CONFIG_ESP_CONSOLE_NONE
    /* keep the console when there is one: chain is cheap enough for dev boards */
    s_log_console =
#endif
    esp_log_set_vprintf(log_vprintf);
    heap_caps_register_failed_alloc_callback(alloc_failed);
    mole_diag_event(true, "boot reset=%s fw=%s prev_log=%s", reset_str(esp_reset_reason()),
                    esp_app_get_description()->version, s_prev_src);
}

void mole_diag_start(void)
{
    if (!s_prev) {                                     /* power loss: fall back to the persisted copy */
        FILE *f = fopen(PERSIST_PATH, "rb");
        if (f) {
            struct stat st;
            if (stat(PERSIST_PATH, &st) == 0 && st.st_size > 0 && st.st_size <= PERSIST_BYTES + 1024) {
                s_prev_file = heap_caps_malloc((size_t)st.st_size + 1, MALLOC_CAP_SPIRAM);
                if (s_prev_file) {
                    s_prev_file_len = fread(s_prev_file, 1, (size_t)st.st_size, f);
                    s_prev_file[s_prev_file_len] = 0;
                    s_prev_src = "spiffs";
                    parse_prev_file();             /* its lines go up to the relay once (prev:true) */
                }
            }
            fclose(f);
        }
    }
    esp_core_dump_summary_t *sum = heap_caps_malloc(sizeof(*sum), MALLOC_CAP_SPIRAM);
    if (sum && esp_core_dump_image_check() == ESP_OK && esp_core_dump_get_summary(sum) == ESP_OK) {
        char cur[17] = {0};
        esp_app_get_elf_sha256(cur, sizeof(cur));
        int o = snprintf(s_core_summary, sizeof(s_core_summary), "task=%s pc=0x%08lx app=%.8s%s bt=",
                         sum->exc_task, (unsigned long)sum->exc_pc, (const char *)sum->app_elf_sha256,
                         strncmp(cur, (const char *)sum->app_elf_sha256, 8) ? "(other build)" : "(this build)");
        for (unsigned i = 0; i < sum->exc_bt_info.depth && i < 8 && o < (int)sizeof(s_core_summary) - 12; i++)
            o += snprintf(s_core_summary + o, sizeof(s_core_summary) - (size_t)o, "%s0x%08lx", i ? "," : "",
                          (unsigned long)sum->exc_bt_info.bt[i]);
        mole_diag_event(false, "coredump %s", s_core_summary);
    }
    heap_caps_free(sum);
    s_tl = heap_caps_calloc(TL_N, sizeof(tl_t), MALLOC_CAP_SPIRAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, wifi_evt, NULL);
    loop_watch_init(&s_evt_watch, now_ms());
    loop_watch_init(&s_tmr_watch, now_ms());
    esp_event_handler_register(MOLE_DIAG_EVENT, 0, evt_beat, NULL);
    const esp_timer_create_args_t beat = {.callback = tmr_beat, .name = "diag_beat"};
    if (esp_timer_create(&beat, &s_beat_timer) == ESP_OK) esp_timer_start_periodic(s_beat_timer, 1000000);
    else s_beat_timer = NULL;
    /* the boot connect usually associated before these handlers existed: start from network.c's
     * state (it listens from before esp_wifi_start), or rssi reads 0 until the first reconnect */
    bool assoc = net_wifi_associated();
    portENTER_CRITICAL(&s_sta_mux);
    sta_watch_seed(&s_sta, assoc);
    portEXIT_CRITICAL(&s_sta_mux);
    start_ping();                                      /* if already associated */
    /* stacks in PSRAM: internal RAM is the device's scarcest resource. Only the
     * persist task (SPIFFS = flash ops = caches off) needs an internal stack. */
    xTaskCreatePinnedToCoreWithCaps(probe_task, "diag_p0", 1536, (void *)0, 1, NULL, 0, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCoreWithCaps(probe_task, "diag_p1", 1536, (void *)1, 1, NULL, 1, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCoreWithCaps(sampler_task, "diag", 4096, NULL, 15, NULL, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);
    xTaskCreate(persist_task, "diag_persist", 3072, NULL, 1, &s_persist_task);
    if (esp_register_shutdown_handler(flash_shutdown) != ESP_OK) mole_diag_event(false, "flash: no shutdown handler (a restart may lose the battery history's last minutes)");
}

/* ── JSON / HTTP ──────────────────────────────────────────────────────────── */

/* the flash's writes since boot, or since `from` (a bench: hold_max_ms is then its own, reset at its start) */
static void flash_json(cJSON *o, const fg_stats_t *from)
{
    portENTER_CRITICAL(&s_fg_mux);
    fg_stats_t d = s_fg;
    portEXIT_CRITICAL(&s_fg_mux);
    if (from) { d.ops -= from->ops; d.anim_ops -= from->anim_ops; d.held -= from->held; d.flushed -= from->flushed;
                d.capped -= from->capped; d.forced -= from->forced; }
    cJSON_AddNumberToObject(o, "writes", d.ops);                    /* (writes + erases) */
    cJSON_AddNumberToObject(o, "writes_animating", d.anim_ops);    /* (must be 0) */
    cJSON_AddNumberToObject(o, "held", d.held);
    cJSON_AddNumberToObject(o, "flushed", d.flushed);
    cJSON_AddNumberToObject(o, "capped", d.capped);
    cJSON_AddNumberToObject(o, "forced", d.forced);
    cJSON_AddNumberToObject(o, "hold_max_ms", d.hold_max_ms);
    cJSON_AddBoolToObject(o, "gate", s_calm_ms != NULL);         /* (false: the fluid never said when it is calm) */
}

static cJSON *heap_json(uint32_t caps)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "free", heap_caps_get_free_size(caps));
    cJSON_AddNumberToObject(j, "min_free", heap_caps_get_minimum_free_size(caps));
    cJSON_AddNumberToObject(j, "largest", heap_caps_get_largest_free_block(caps));
    return j;
}

cJSON *mole_diag_json(int timeline_secs)
{
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
    cJSON_AddNumberToObject(j, "uptime_ms", esp_timer_get_time() / 1000);
    cJSON_AddStringToObject(j, "reset_reason", reset_str(esp_reset_reason()));
    cJSON_AddStringToObject(j, "fw", esp_app_get_description()->version);
    cJSON *fr = cJSON_AddObjectToObject(j, "frames");
    cJSON *sim = frames_json(&s_sim);
    cJSON_AddNumberToObject(sim, "work_ms", s_sim_work_us / 1000.0);
    cJSON_AddItemToObject(fr, "sim", sim);
    cJSON *pan = frames_json(&s_panel);
    cJSON_AddNumberToObject(pan, "draw_ms", s_draw_us / 1000.0);
    cJSON_AddNumberToObject(pan, "flush_ms", s_flush_us / 1000.0);
    cJSON_AddItemToObject(fr, "panel", pan);
    cJSON *t = hist_json(s_touch, s_touch_n, 64);
    cJSON_AddNumberToObject(t, "total", s_touch_n);
    cJSON_AddNumberToObject(t, "max_total_ms", s_touch_max / 1000.0);
    cJSON_AddItemToObject(fr, "touch_to_panel", t);
    te_json(cJSON_AddObjectToObject(fr, "te"));
    cJSON *h = cJSON_AddObjectToObject(j, "heap");
    cJSON_AddItemToObject(h, "internal", heap_json(MALLOC_CAP_INTERNAL));
    cJSON_AddItemToObject(h, "dma", heap_json(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    cJSON_AddItemToObject(h, "psram", heap_json(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(h, "alloc_failures", atomic_load(&s_alloc_fail));
    cJSON_AddNumberToObject(h, "last_fail_size", atomic_load(&s_alloc_fail_size));
    cJSON_AddNumberToObject(h, "last_fail_caps", atomic_load(&s_alloc_fail_caps));
    cJSON *stalls = cJSON_AddObjectToObject(j, "stalls");
    cJSON_AddNumberToObject(stalls, "probe_core0_max_ms", s_probe_max_total[0]);
    cJSON_AddNumberToObject(stalls, "probe_core1_max_ms", s_probe_max_total[1]);
    cJSON_AddNumberToObject(stalls, "sampler_late_max_ms", s_late_max_total);
    cJSON_AddNumberToObject(stalls, "gap_events", atomic_load(&s_gap_events));
    cJSON *net = cJSON_AddObjectToObject(j, "net");
    cJSON_AddNumberToObject(net, "rssi", rssi_now());
    wifi_ps_type_t ps = WIFI_PS_NONE; esp_wifi_get_ps(&ps);
    cJSON_AddStringToObject(net, "wifi_ps", ps == WIFI_PS_NONE ? "none" : ps == WIFI_PS_MIN_MODEM ? "min" : "max");
    int8_t tx_qdbm = 0;
    if (esp_wifi_get_max_tx_power(&tx_qdbm) == ESP_OK) cJSON_AddNumberToObject(net, "tx_dbm", tx_qdbm / 4.0);
    cJSON_AddNumberToObject(net, "gw_ping_sent", s_ping_sent_total);
    cJSON_AddNumberToObject(net, "gw_ping_lost", s_ping_lost_total);
    cJSON_AddNumberToObject(net, "gw_rtt_last_ms", atomic_load(&s_ping_rtt_last));
    cJSON_AddBoolToObject(net, "link_connected", link_client_connected());
    cJSON_AddBoolToObject(net, "audio_busy", link_audio_busy());
    cJSON *tasks = cJSON_AddArrayToObject(j, "tasks");
    task_pct_t snap[MAX_TASKS]; int n;
    portENTER_CRITICAL(&s_task_mux);
    n = s_ntasks; memcpy(snap, s_tasks, sizeof(task_pct_t) * (size_t)n);
    portEXIT_CRITICAL(&s_task_mux);
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", snap[i].name);
        cJSON_AddNumberToObject(e, "cpu", snap[i].pct);
        cJSON_AddNumberToObject(e, "core", snap[i].core == 2 ? -1 : snap[i].core);
        cJSON_AddNumberToObject(e, "prio", snap[i].prio);
        cJSON_AddNumberToObject(e, "stack_free", snap[i].stack_free);
        cJSON_AddItemToArray(tasks, e);
    }
    cJSON *lg = cJSON_AddObjectToObject(j, "log");
    cJSON_AddNumberToObject(lg, "next_seq", s_ring.next);
    cJSON_AddNumberToObject(lg, "slots", RING_SLOTS);
    cJSON_AddStringToObject(lg, "prev_source", s_prev_src);
    cJSON_AddNumberToObject(lg, "persists", s_persist_n);
    cJSON_AddNumberToObject(lg, "persist_fail", s_persist_fail);
    cJSON_AddNumberToObject(lg, "persist_last_ms", s_persist_ms);
    flash_json(cJSON_AddObjectToObject(j, "flash"), NULL);
    if (s_core_summary[0]) cJSON_AddStringToObject(j, "coredump", s_core_summary);
    console_store_fs_json(cJSON_AddObjectToObject(j, "fs"), 12);   /* SPIFFS: what fills it */
    if (timeline_secs > 0 && s_tl) {
        if (timeline_secs > TL_N) timeline_secs = TL_N;
        uint32_t n_avail = s_tl_n < TL_N ? s_tl_n : TL_N;
        uint32_t k = (uint32_t)timeline_secs < n_avail ? (uint32_t)timeline_secs : n_avail;
        cJSON *tl = cJSON_AddArrayToObject(j, "timeline");
        cJSON_AddStringToObject(j, "timeline_cols",
            "t_s,int_min,int_largest,psram_free,sim_n,sim_max_ms,panel_n,panel_max_ms,probe0_ms,probe1_ms,late_ms,gw_rtt_ms,gw_lost,rssi,flags,top1,top1_pct,top2,top2_pct");
        for (uint32_t q = s_tl_n - k; q < s_tl_n; q++) {
            const tl_t *e = &s_tl[q % TL_N];
            cJSON *row = cJSON_CreateArray();
            double v[] = {e->t_s, e->int_min, e->int_largest, e->psram_free, e->sim_n, e->sim_max, e->panel_n,
                          e->panel_max, e->probe0, e->probe1, e->late, e->gw_rtt, e->gw_lost, e->rssi, e->flags};
            for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) cJSON_AddItemToArray(row, cJSON_CreateNumber(v[i]));
            for (int i = 0; i < 2; i++) {
                cJSON_AddItemToArray(row, cJSON_CreateString(e->top[i]));
                cJSON_AddItemToArray(row, cJSON_CreateNumber(e->top_pct[i]));
            }
            cJSON_AddItemToArray(tl, row);
        }
    }
    return j;
}

static uint32_t query_u32(httpd_req_t *r, const char *key, uint32_t dflt)
{
    char q[96], v[16];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK) return dflt;
    if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) return dflt;
    return (uint32_t)strtoul(v, NULL, 10);
}

static bool query_is(httpd_req_t *r, const char *key, const char *val)
{
    char q[96], v[16];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) return false;
    return !strcmp(v, val);
}

static esp_err_t http_diag(httpd_req_t *r)
{
    cJSON *j = mole_diag_json((int)query_u32(r, "timeline", 0));
    char *body = j ? cJSON_PrintUnformatted(j) : NULL;
    cJSON_Delete(j);
    if (!body) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(r, body);
    free(body);
    return e;
}

static esp_err_t send_text(httpd_req_t *r, const char *text, uint32_t next, bool json)
{
    char nx[16]; snprintf(nx, sizeof(nx), "%lu", (unsigned long)next);
    httpd_resp_set_type(r, json ? "application/x-ndjson" : "text/plain; charset=utf-8");
    httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(r, "Access-Control-Expose-Headers", "X-Next-Seq");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "X-Next-Seq", nx);
    size_t len = strlen(text);
    for (size_t o = 0; o < len; o += 4096) {         /* chunked: no second big copy */
        size_t c = len - o < 4096 ? len - o : 4096;
        if (httpd_resp_send_chunk(r, text + o, (ssize_t)c) != ESP_OK) return ESP_FAIL;
    }
    return httpd_resp_send_chunk(r, NULL, 0);
}

static esp_err_t http_log(httpd_req_t *r)
{
    uint32_t since = query_u32(r, "since", 0), max = query_u32(r, "max", 32768), next = 0;
    if (max < 1024) max = 1024;
    if (max > 65536) max = 65536;
    bool json = query_is(r, "fmt", "json");
    char *text = mole_diag_log_text(since, max, &next, json);
    if (!text) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    esp_err_t e = send_text(r, text, next, json);
    heap_caps_free(text);
    return e;
}

static esp_err_t http_log_prev(httpd_req_t *r)
{
    if (s_prev) {
        uint32_t next;
        char *text = render(s_prev, 0, 65536, &next, query_is(r, "fmt", "json"));
        if (!text) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        esp_err_t e = send_text(r, text, next, query_is(r, "fmt", "json"));
        heap_caps_free(text);
        return e;
    }
    if (s_prev_file) return send_text(r, s_prev_file, 0, false);
    httpd_resp_set_status(r, "404 Not Found");
    httpd_resp_set_type(r, "text/plain");
    return httpd_resp_sendstr(r, "no previous-boot log\n");
}

static esp_err_t http_wifi_ps(httpd_req_t *r)
{
    char q[64], v[8];
    if (r->method == HTTP_POST && httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "mode", v, sizeof(v)) == ESP_OK) {
        wifi_ps_type_t m = !strcmp(v, "none") ? WIFI_PS_NONE : !strcmp(v, "max") ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM;
        esp_err_t e = esp_wifi_set_ps(m);
        mole_diag_event(false, "wifi.ps set=%s -> %s", v, esp_err_to_name(e));
    }
    wifi_ps_type_t ps = WIFI_PS_NONE; esp_wifi_get_ps(&ps);
    char body[64];
    snprintf(body, sizeof(body), "{\"ok\":true,\"wifi_ps\":\"%s\"}",
             ps == WIFI_PS_NONE ? "none" : ps == WIFI_PS_MIN_MODEM ? "min" : "max");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, body);
}

/* dev: force a Wi-Fi drop (the disconnect handlers, the back-off, RAM while down). POST only. */
static esp_err_t http_wifi_drop(httpd_req_t *r)
{
    const uint32_t hold = query_u32(r, "hold", 0);
    esp_err_t e = net_wifi_drop(hold);
    mole_diag_event(false, "wifi.drop_forced hold=%lus -> %s", (unsigned long)hold, esp_err_to_name(e));
    char body[64];
    snprintf(body, sizeof(body), "{\"ok\":%s,\"hold_s\":%lu}", e == ESP_OK ? "true" : "false", (unsigned long)hold);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, body);
}

#ifdef MOLTY_DIAG_RING
/* debug builds only: POST /api/diag/ring?ms=<ms> shows the listening ring for ms (default 3000) without a listen */
static esp_err_t http_ring(httpd_req_t *r)
{
    const uint32_t ms = query_u32(r, "ms", 3000);
    char q[64] = "", lv[16] = "";
    const bool voice = httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK &&
                       httpd_query_key_value(q, "level", lv, sizeof(lv)) == ESP_OK && !strcmp(lv, "sine");
    mole_fluid_diag_ring(ms, voice);
    char body[48];
    snprintf(body, sizeof(body), "{\"ok\":true,\"ms\":%lu}", (unsigned long)ms);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, body);
}
#endif

/* ── the on-device benchmark: POST /api/diag/bench?runs=3 ──────────────────────
 * Untouched, `runs` times (1..5): the boot word to the cat, the cat to the astrology chart (a fixed sky: bench_chart.h),
 * a tap on Leo (the chart to its close-up), a tap beside it (back), the chart to the cat; then on the chart again a tap
 * in its open middle and a tap on the Moon (its reading). Each case's animations as the drawer records them
 * (mole_anim_rec_t, the `anim` log lines) and one `bench` log line a case; the answer is the whole table. It holds this
 * server meanwhile (about 50 s a run). */
#define BENCH_RECS 160
enum { BC_BOOT, BC_CHART, BC_SIGN, BC_BACK, BC_CAT, BC_MIDDLE, BC_PLANET, BC_N };
static const char *const BENCH_CASE[BC_N] = { "boot-cat", "cat-chart", "chart-sign", "sign-chart", "chart-cat", "tap-middle", "tap-planet" };
static const char *const BENCH_ANIM[4] = { "wave", "grow", "word", "water" };
static mole_anim_rec_t *s_brec;                       /* [BENCH_RECS] (PSRAM, while it runs) */

static int bench_n(void) { return mole_fluid_bench_capture(s_brec, BENCH_RECS); }
/* nothing in mask (bits of the animations, 1: a change) for calm ms; ms at most. True: it was */
static bool bench_calm(unsigned mask, uint32_t calm, uint32_t ms)
{
    const int64_t t0 = esp_timer_get_time();
    int64_t since = 0;
    for (int64_t now = t0; now - t0 < (int64_t)ms * 1000; now = esp_timer_get_time()) {
        if (!mole_fluid_bench_quiet(mask)) since = 0;
        else if (!since) since = now;
        else if (now - since >= (int64_t)calm * 1000) return true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}
/* no flash write or erase for 300 ms and no file write open (8 s at most): a case and its set-up start on a flash at
 * rest (a write held for calm goes in this wait: the screen is calm) */
static void bench_flash_idle(void)
{
    for (int q = 0; q < 160; q++) {
        portENTER_CRITICAL(&s_fg_mux);
        const bool busy = (s_fg_batch.ops && (s_fg_batch.wbytes || s_fg_batch.ebytes) && esp_timer_get_time() - s_fg_batch.last_us < 300000) ||
                          atomic_load(&s_fg_gated[0]) || atomic_load(&s_fg_gated[1]);
        portEXIT_CRITICAL(&s_fg_mux);
        if (!busy) return;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
/* a record of anim taken since `from`; ms at most */
static bool bench_rec(int from, int anim, uint32_t ms)
{
    const int64_t t0 = esp_timer_get_time();
    for (;;) {
        for (int q = from, n = bench_n(); q < n; q++) if (s_brec[q].anim == anim) return true;
        if (esp_timer_get_time() - t0 >= (int64_t)ms * 1000) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
/* the chart (its fixed sky), or none: the cat */
static const char *bench_scene(bool chart)
{
    cJSON *args = cJSON_CreateObject(), *out = NULL;
    if (!args) return "oom";
    esp_err_t e;
    if (chart) {
        cJSON_AddStringToObject(args, "pkg", "astrology"); cJSON_AddStringToObject(args, "layout", "chart");
        cJSON_AddItemToObject(args, "params", cJSON_Parse(BENCH_CHART)); cJSON_AddNumberToObject(args, "ttl_s", 120);
        e = pkg_cmd_scene_play(args, &out);
    } else e = pkg_cmd_scene_clear(args, &out);
    cJSON_Delete(args); cJSON_Delete(out);
    return e == ESP_OK ? NULL : esp_err_to_name(e);
}
static double r1(float v) { return (double)lroundf(v * 10.f) / 10.0; }
static cJSON *bench_rec_json(const mole_anim_rec_t *b)
{
    cJSON *o = cJSON_CreateObject(), *a;
    cJSON_AddStringToObject(o, "anim", BENCH_ANIM[b->anim & 3]);
    cJSON_AddNumberToObject(o, "ms", b->ms); cJSON_AddNumberToObject(o, "frames", b->f); cJSON_AddNumberToObject(o, "sent", b->s);
    a = cJSON_AddArrayToObject(o, "kpx"); cJSON_AddItemToArray(a, cJSON_CreateNumber(r1(b->kpx))); cJSON_AddItemToArray(a, cJSON_CreateNumber(r1(b->kpx_max)));
    cJSON_AddNumberToObject(o, "win", r1(b->win));
    a = cJSON_AddArrayToObject(o, "pan"); for (int c = 0; c < 3; c++) cJSON_AddItemToArray(a, cJSON_CreateNumber(b->pan[c]));
    a = cJSON_AddArrayToObject(o, "sim"); for (int c = 0; c < 3; c++) cJSON_AddItemToArray(a, cJSON_CreateNumber(b->sim[c]));
    a = cJSON_AddArrayToObject(o, "pres"); for (int c = 0; c < 3; c++) cJSON_AddItemToArray(a, cJSON_CreateNumber(b->pres[c]));
    if (b->refr[1] || b->refr[2] || b->refr[3]) {       /* (the panel's refreshes from a frame's first window to the next's) */
        cJSON *rf = cJSON_AddObjectToObject(o, "refreshes");
        static const char *const RN[4] = { "0", "1", "2", "3+" };
        for (int q = 0; q < 4; q++) cJSON_AddNumberToObject(rf, RN[q], b->refr[q]);
    } else cJSON_AddStringToObject(o, "refreshes", b->refr[0] ? "no TE pulses" : "no two frames reached the panel");
    a = cJSON_AddArrayToObject(o, "worst"); cJSON_AddItemToArray(a, cJSON_CreateNumber(r1(b->worst))); cJSON_AddItemToArray(a, cJSON_CreateNumber(r1(b->worst_cells)));
    cJSON *p = cJSON_AddObjectToObject(o, "parts");    /* (mean, max ms a frame; the parts it had) */
    for (int q = 0; q < MOLE_ANIM_PARTS; q++) {
        if (b->max[q] < 0.05f) continue;
        a = cJSON_AddArrayToObject(p, MOLE_ANIM_PART_NAMES[q]);
        cJSON_AddItemToArray(a, cJSON_CreateNumber(r1(b->mean[q]))); cJSON_AddItemToArray(a, cJSON_CreateNumber(r1(b->max[q])));
    }
    if (b->anim == 0) {
        cJSON *c = cJSON_AddObjectToObject(o, "change");
        cJSON_AddNumberToObject(c, "comp_kpx", r1(b->comp)); cJSON_AddBoolToObject(c, "forced", b->forced); cJSON_AddBoolToObject(c, "dust_after", b->keep);
        cJSON_AddNumberToObject(c, "ready_at", b->ready_at); cJSON_AddNumberToObject(c, "held", b->held);
        cJSON_AddNumberToObject(c, "grains_old", b->na); cJSON_AddNumberToObject(c, "grains_new", b->nb); cJSON_AddNumberToObject(c, "pairs", b->pairs);
    } else cJSON_AddNumberToObject(o, "drawn_kpx", r1(b->comp));   /* (changed cells drawn into the kept glass a frame: cmp / this, us a px) */
    return o;
}
/* one case: what it starts from, then it, till its animations are over */
static cJSON *bench_case(int c, int run)
{
    mole_fluid_screen(true);
    bench_flash_idle();
    switch (c) {                                       /* (from: the cat; the chart, its dust in; the last change over) */
    case BC_BOOT:   bench_scene(false); bench_calm(1, 1500, 15000); break;
    case BC_MIDDLE: bench_scene(true); bench_calm(1, 1500, 15000);
                    for (int q = 0; q < 200 && !mole_fluid_bench_dust(); q++) vTaskDelay(pdMS_TO_TICKS(50));
                    bench_calm(0xF, 1000, 8000); break;
    case BC_PLANET: bench_calm(0xF, 1000, 8000); break;
    default:        bench_calm(1, 1000, 10000); break;
    }
    bench_flash_idle();
    const int from = bench_n();
    const int64_t t0 = esp_timer_get_time();
    const char *err = NULL;
    float x = 0.f, y = 0.f;
    switch (c) {
    case BC_BOOT:   mole_fluid_bench_boot(); break;
    case BC_CHART:  err = bench_scene(true); break;
    case BC_SIGN:
        if (!mole_fluid_bench_spot("leo", &x, &y)) { x = 0.76f * cosf(302.f * 0.017453293f); y = 0.76f * sinf(302.f * 0.017453293f); }
        if (!mole_fluid_bench_tap(x, y)) err = "no spot at leo";
        break;
    case BC_BACK:   if (mole_fluid_bench_tap(0.f, -0.8f) != MOLE_SPOT_BACK) err = "not a way back"; break;
    case BC_CAT:    err = bench_scene(false); break;
    case BC_MIDDLE: mole_fluid_bench_tap(0.15f, 0.3f); break;
    case BC_PLANET: if (!mole_fluid_bench_spot("moon", &x, &y)) err = "no moon"; else mole_fluid_bench_tap(x, y); break;
    }
    /* its end: a change recorded and none for 1.5 s (a scene's dust may come after as a change of its own); a tap's water
     * (and the Moon's reading) recorded and every tap animation over */
    const int want = c == BC_MIDDLE ? 3 : c == BC_PLANET ? 2 : 0;
    if (!err && !bench_rec(from, want, c == BC_PLANET ? 40000 : 25000)) err = "timeout";
    const uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (!err) bench_calm(want ? 0xE : 1, want ? 500 : 1500, 10000);
    cJSON *o = cJSON_CreateObject(), *a = cJSON_AddArrayToObject(o, "anims");
    cJSON_AddStringToObject(o, "case", BENCH_CASE[c]); cJSON_AddNumberToObject(o, "run", run);
    cJSON_AddNumberToObject(o, "ms", ms);
    if (err) cJSON_AddStringToObject(o, "error", err);
    const int n = bench_n();
    for (int q = from; q < n; q++) cJSON_AddItemToArray(a, bench_rec_json(&s_brec[q]));
    mole_diag_event(false, "bench %s run=%d ms=%u anims=%d%s%s", BENCH_CASE[c], run, (unsigned)ms, n - from, err ? " error=" : "", err ? err : "");
    return o;
}
static esp_err_t http_bench(httpd_req_t *r)
{
    static atomic_bool busy;
    if (atomic_exchange(&busy, true)) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "a bench runs already");
    uint32_t runs = query_u32(r, "runs", 3);
    runs = runs < 1 ? 1 : runs > 5 ? 5 : runs;
    s_brec = heap_caps_calloc(BENCH_RECS, sizeof(mole_anim_rec_t), MALLOC_CAP_SPIRAM);
    cJSON *j = cJSON_CreateObject();
    if (!s_brec || !j) { heap_caps_free(s_brec); s_brec = NULL; cJSON_Delete(j); atomic_store(&busy, false); return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); }
    const bool astro = pkg_store_find(PKG_ROOT, "astrology", NULL);
    cJSON_AddStringToObject(j, "astrology", astro ? "installed" : "not installed: only boot-cat runs");
    mole_fluid_screen(true);
    bench_n();
    portENTER_CRITICAL(&s_fg_mux);
    const fg_stats_t fg0 = s_fg;
    s_fg.hold_max_ms = 0;
    portEXIT_CRITICAL(&s_fg_mux);
    cJSON *cases = cJSON_AddArrayToObject(j, "cases");
    for (int run = 1; run <= (int)runs; run++)
        for (int c = 0; c < (astro ? BC_N : 1); c++) cJSON_AddItemToArray(cases, bench_case(c, run));
    mole_fluid_bench_capture(NULL, 0);
    heap_caps_free(s_brec); s_brec = NULL;
    if (astro) bench_scene(false);
    cJSON *h = cJSON_AddObjectToObject(j, "heap");
    cJSON_AddNumberToObject(h, "int_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(h, "int_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(h, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(h, "psram_largest", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    te_json(cJSON_AddObjectToObject(j, "te"));              /* (the panel's refresh, the last second) */
    flash_json(cJSON_AddObjectToObject(j, "flash"), &fg0);  /* (the bench's own: writes while animating must be 0) */
    cJSON *st = cJSON_AddObjectToObject(j, "stack_free");    /* (high-water marks, bytes, as the sampler last saw them) */
    portENTER_CRITICAL(&s_task_mux);
    int sd = -1, ss = -1, sx = -1;
    for (int i = 0; i < s_ntasks; i++) {
        if (!strcmp(s_tasks[i].name, "fluid_draw")) sd = (int)s_tasks[i].stack_free;
        if (!strcmp(s_tasks[i].name, "fluid_task")) ss = (int)s_tasks[i].stack_free;
        if (!strcmp(s_tasks[i].name, "fluid_tx")) sx = (int)s_tasks[i].stack_free;
    }
    portEXIT_CRITICAL(&s_task_mux);
    cJSON_AddNumberToObject(st, "fluid_draw", sd); cJSON_AddNumberToObject(st, "fluid_task", ss); cJSON_AddNumberToObject(st, "fluid_tx", sx);
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    atomic_store(&busy, false);
    if (!body) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = ESP_OK;
    const size_t len = strlen(body);
    for (size_t o = 0; o < len && e == ESP_OK; o += 4096) e = httpd_resp_send_chunk(r, body + o, (ssize_t)(len - o < 4096 ? len - o : 4096));
    if (e == ESP_OK) e = httpd_resp_send_chunk(r, NULL, 0);
    free(body);
    return e;
}

esp_err_t mole_diag_http_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/api/diag", .method = HTTP_GET, .handler = http_diag},
        {.uri = "/api/log", .method = HTTP_GET, .handler = http_log},
        {.uri = "/api/log/prev", .method = HTTP_GET, .handler = http_log_prev},
        {.uri = "/api/diag/wifi_ps", .method = HTTP_GET, .handler = http_wifi_ps},
        {.uri = "/api/diag/wifi_ps", .method = HTTP_POST, .handler = http_wifi_ps},
        {.uri = "/api/diag/wifi_drop", .method = HTTP_POST, .handler = http_wifi_drop},
        {.uri = "/api/diag/bench", .method = HTTP_POST, .handler = http_bench},
#ifdef MOLTY_DIAG_RING
        {.uri = "/api/diag/ring", .method = HTTP_POST, .handler = http_ring},
#endif
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t e = httpd_register_uri_handler(server, &routes[i]);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}
