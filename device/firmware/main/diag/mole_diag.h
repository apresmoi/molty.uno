/* mole_diag — always-on, cheap device diagnostics.
 *
 *  - Log ring: every ESP_LOG line (esp_log_set_vprintf hook) + structured
 *    events (mole_diag_event) land in a 64 KB ring in no-init PSRAM, so the
 *    previous boot's log survives panics / WDT / OTA resets. The tail is also
 *    persisted to SPIFFS (rate-limited) so it survives power loss.
 *  - Perf: sim + panel frame-interval histograms, touch->panel latency,
 *    per-task CPU % (FreeRTOS run-time stats), heap free + low-water,
 *    alloc-failure hook, per-core starvation probes, gateway ping, and a
 *    1 Hz timeline (last 180 s) to correlate stalls with what ran.
 *  HTTP: GET /api/diag[?timeline=<s>][&fs=1 (the files by size: reads flash)], GET /api/log?since=<seq>[&fmt=json], GET /api/log/prev,
 *        GET|POST /api/diag/wifi_ps?mode=none|min|max,
 *        POST /api/diag/wifi_drop?hold=<s> (dev: drop the station, no reconnect for <s>, max 600).
 *  Wedge watch: the default event loop or the esp_timer task idle 30 s aborts (loop_watch.h).
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"

/* As early as possible in app_main: captures the previous boot's no-init ring,
 * starts the new one, hooks ESP_LOG. Safe before NVS/SPIFFS. */
void mole_diag_early_init(void);
/* After SPIFFS + Wi-Fi are up: loads the persisted previous log (if the RAM
 * copy did not survive), starts the sampler/persist tasks + Wi-Fi hooks. */
void mole_diag_start(void);

/* Structured event: one line "EVT <text>". `important` lines (errors) trigger
 * a rate-limited SPIFFS persist, once the screen is calm (flash_gate.h); a stall's
 * record is never important (its write would stall the next frames). Callable from
 * any task (not ISR). */
void mole_diag_event(bool important, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/** What tells the flash's writes how long the screen has been calm, ms (0: it animates now; flash_gate.h): none set,
 *  always calm (every write goes at once, as before the gate). */
void mole_diag_set_calm(uint32_t (*calm_ms)(void));

/* The flash's quiet window (flash_gate.h), for writes that can wait. Call on a task with an internal stack. */
#include "flash_gate.h"
/** A write due: true = it may go now (the screen calm, held FG_HOLD_MAX_US, or a restart / power off next). h: the write's
 *  own hold (zeroed to start). Never blocks. */
bool mole_flash_may(fg_hold_t *h);
/** How long the screen has been calm, ms (0: it moves now; UINT32_MAX: no gate, or the screen off). Never blocks. */
uint32_t mole_flash_calm_ms(void);
/** Blocks till mole_flash_may says go (100 ms looks). */
void mole_flash_quiet(void);
/** path becomes the len bytes at data (written to tmp, then renamed over path), a write that can wait: the erases SPIFFS
 *  needs for it first, then each 2 KB, each step only while the screen is calm. False: it failed, path kept what it had. */
bool mole_flash_file(const char *path, const char *tmp, const void *data, size_t len);
/** The screen starts to move (a change of picture, a ripple), from rest: a gated op in flight ends first (50 ms at most:
 *  a sector erase), and none starts for 100 ms (the caller marks its motion meanwhile). Call from the fluid's tasks. */
void mole_flash_motion(void);
/** A restart or power off is next: every hold lets go from now on. */
void mole_flash_force(void);
bool mole_flash_forced(void);
/** t holds writes that a restart would lose (the battery history): esp_restart wakes it, forced, and waits for
 *  mole_flash_flushed (1.5 s at most) before the restart goes on. */
void mole_flash_flusher(void *task);
void mole_flash_flushed(void);
/** Before a power off: the log ring written now (it does not survive one), wait_ms at most. */
void mole_diag_flush(uint32_t wait_ms);

/* Frame hooks (fluid creature). */
void mole_diag_sim_frame(uint32_t interval_us, uint32_t work_us);
void mole_diag_panel_frame(uint32_t draw_us, uint32_t flush_us);
void mole_diag_touch_latency(uint32_t us);

/* JSON snapshot (caller deletes). timeline_secs>0 adds that much of the 1 Hz timeline (max 180). */
cJSON *mole_diag_json(int timeline_secs);
/* Log lines with seq >= since, newest-last, at most max_bytes of text.
 * Returns malloc'd (PSRAM) text; *next = seq to pass next time. */
char *mole_diag_log_text(uint32_t since, size_t max_bytes, uint32_t *next, bool json);

/* Activity log (docs/AGENT-LINK.md "Activity log"; rules in log_upload.h).
 * Job tags: every line logged between begin and end of a relay job carries the job id's
 * tag (the part after ".j_"); ids without one are ignored. Each begin needs one end, on
 * every path (done, failed, cancelled). An end also asks the link to upload. */
void mole_diag_job_begin(const char *job_id);
void mole_diag_job_end(const char *job_id);
/* true once after a job ended (the link task polls it) */
bool mole_diag_log_take_request(void);
/* The relay's welcome: this uptime (ms) is that relay wall time (unix ms). Kept with the
 * ring (no-init RAM and the persisted copy) so the previous boot's lines get wall times. */
void mole_diag_link_sync(uint32_t up_ms, int64_t wall_ms);
/* Next upload frame ({"t":"log",...}, caller deletes) of this boot's uploadable lines from
 * *cursor (advanced past what the frame holds), NULL when there is none. backlog > 0:
 * first skip ahead so at most that many lines remain (a long offline spell). */
cJSON *mole_diag_log_frame(uint32_t *cursor, int max_lines, int backlog);
/* The same for the previous boot's lines (prev:true, with its sync when known). */
cJSON *mole_diag_log_prev_frame(uint32_t *cursor, int max_lines, int backlog);

/* Boot bookkeeping for /api/info: bump once per boot right after NVS init. */
void        mole_diag_boot_count_bump(void);
uint32_t    mole_diag_boot_count(void);
const char *mole_diag_reset_reason(void);   /* "poweron", "panic", "task_wdt", ... */

esp_err_t mole_diag_http_register(httpd_handle_t server);
