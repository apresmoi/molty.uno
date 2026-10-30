/* log_upload — the pure rules behind the device half of the activity log
 * (docs/AGENT-LINK.md "Activity log"). Plain C + cJSON, host-tested
 * (test/host/log_upload_test.c); mole_diag.c owns the ring, the locks and the
 * file, link_client.c the socket.
 *
 *  - job tags: lines logged while a relay job (say / listen / ask / update) runs carry
 *    the job id's random part, so the relay can file them under that job;
 *  - the upload filter: which ring lines go up to the relay;
 *  - the upload frame: {"t":"log", boot, up, lines:[[seq, ms, tag, text]...], prev?, sync?};
 *  - the persisted copy: its line/header format and when it is written.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

/* ── job tags ─────────────────────────────────────────────────────────────── */

#define LOG_TAG_MAX 32           /* the relay's job ids carry 16 chars after ".j_" */

/* "<device>.j_<tag>" → tag (1..LOG_TAG_MAX chars of [A-Za-z0-9_-]). Anything else
 * (no ".j_", empty, too long, other characters) → false and out = "". */
bool log_job_tag(const char *job_id, char out[LOG_TAG_MAX + 1]);

/* The jobs running now. Two at once: an ask without speech can overlap a listen, and an
 * update's download starts while its confirmation is closing. Each begin is matched by one
 * end (counted, so begin/begin/end keeps the tag); lines are tagged with the most recently
 * begun job still running. A third concurrent job replaces the least recent one. */
typedef struct {
    char tag[2][LOG_TAG_MAX + 1];
    uint8_t refs[2];
    uint32_t at[2];              /* begin order */
    uint32_t clock;
} log_jobs_t;
void log_jobs_begin(log_jobs_t *j, const char *tag);
void log_jobs_end(log_jobs_t *j, const char *tag);
/* "" when no job runs */
const char *log_jobs_current(const log_jobs_t *j);

/* ── upload ───────────────────────────────────────────────────────────────── */

#define LOG_UPLOAD_TEXT_MAX   150       /* bytes of a line's text that go up */
#define LOG_UPLOAD_LINES_MAX  64        /* hard cap per frame, whatever the relay asks */
#define LOG_UPLOAD_FRAME_MAX  (12 * 1024)
#define LOG_UPLOAD_GAP_MS     2000      /* at most one frame per 2 s */

typedef struct {
    uint32_t seq, ms;
    const char *job;              /* "" or NULL: none */
    const char *text;
} log_line_t;

/* A line worth uploading: tagged with a job, an ESP_LOG error / warning ("E (" / "W (",
 * after an optional ANSI colour escape), or a structured "EVT " line — except the routine
 * 10 s timeline "EVT TL " (its trouble form "EVT TL!" goes up). */
bool log_upload_wanted(const char *text, const char *job);

typedef struct {
    uint32_t boot;                /* the boot the lines belong to */
    uint32_t up_ms;               /* device uptime when the frame is built */
    bool prev;                    /* lines of the previous boot */
    bool has_sync;                /* prev: that boot's last welcome sync is known */
    uint32_t sync_up_ms;
    int64_t sync_wall_ms;
    int max_lines;                /* the relay's welcome.log.max_lines (clamped 1..LOG_UPLOAD_LINES_MAX) */
} log_frame_opts_t;

/* One frame from lines[0..n) (oldest first, seq ascending): the wanted lines with
 * seq >= *cursor, at most max_lines and LOG_UPLOAD_FRAME_MAX bytes of JSON. *cursor
 * moves to the first wanted line left out (cut by a cap), else to end_seq. NULL when no
 * line is included (the cursor still moves to end_seq: nothing to send). */
cJSON *log_upload_frame(const log_line_t *lines, size_t n, const log_frame_opts_t *o,
                        uint32_t end_seq, uint32_t *cursor);

/* Backlog bound: given the seqs of the wanted lines (ascending), the cursor that leaves at
 * most `keep` of them at or after it (never moves the cursor back). */
uint32_t log_upload_backlog(const uint32_t *wanted_seqs, size_t n, uint32_t cursor, size_t keep);

/* ── the persisted copy (SPIFFS, survives power loss) ─────────────────────── */
/*
 * File: one header line, then one line per ring line, oldest first, newest LOG_PERSIST_BYTES:
 *   #molty-log 2 boot=<boot> sync_up=<uptime ms> sync_wall=<relay unix ms>\n
 *   <seq> <ms> <tag|-> <text>\n
 * sync_up=0 sync_wall=0: that boot never got a welcome (no wall time).
 *
 * When it is written (flash wear): SPIFFS writes stall the other core (~150 ms hitches
 * measured over a 2 s 24 KB write) and wear the flash, and soft resets (panic, WDT,
 * esp_restart after an OTA) are already covered by the no-init RAM ring — the file only has
 * to survive power loss. So: on an error line / important event (rate-limited), and on the
 * 30-minute tick only if a line worth uploading arrived since the last write; never in the
 * first 90 s (boot loops must not hammer the flash); at least 5 min apart. Worst case
 * (errors repeating all day) ≤ 288 writes/day of ≤ 12 KB (~3.5 MB/day, which SPIFFS wear
 * levelling spreads over the 1+ MB partition: decades at 100k cycles); typical: a few a day.
 */
#define LOG_PERSIST_BYTES         (12 * 1024)
#define LOG_PERSIST_MIN_GAP_US    (300LL * 1000000)
#define LOG_PERSIST_PERIOD_MS     (30 * 60 * 1000)
#define LOG_PERSIST_NOT_BEFORE_US (90LL * 1000000)

/* How long to wait before a write may happen (0 = now): never before NOT_BEFORE after
 * boot, never within MIN_GAP of the last write (last_us 0 = none yet). */
int64_t log_persist_delay_us(int64_t now_us, int64_t last_us);
/* Whether a wake-up writes: an explicit request (error / important event) always; the
 * periodic tick only when wanted lines arrived since the last write. */
bool log_persist_due(bool requested, uint32_t wanted_since_last);

int log_persist_header(char *out, size_t cap, uint32_t boot, uint32_t sync_up_ms, int64_t sync_wall_ms);
/* false: not a header of this format */
bool log_persist_parse_header(const char *line, uint32_t *boot, uint32_t *sync_up_ms, int64_t *sync_wall_ms);
/* "<seq> <ms> <tag|-> <text>" (no newline; newlines in text become spaces). Returns the
 * length written, or -1 when it does not fit. */
int log_persist_line(char *out, size_t cap, const log_line_t *l);
/* Parses one line in place (`line` has no newline): out->job / out->text point into it. */
bool log_persist_parse_line(char *line, log_line_t *out);
