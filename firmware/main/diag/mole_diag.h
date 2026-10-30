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
 *  HTTP: GET /api/diag, GET /api/log?since=<seq>[&fmt=json], GET /api/log/prev,
 *        GET|POST /api/diag/wifi_ps?mode=none|min|max.
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
 * a rate-limited SPIFFS persist. Callable from any task (not ISR). */
void mole_diag_event(bool important, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

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
