/* log_upload — see log_upload.h. Pure: no ESP-IDF, no globals. */
#include "log_upload.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── job tags ─────────────────────────────────────────────────────────────── */

static bool tag_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool log_job_tag(const char *job_id, char out[LOG_TAG_MAX + 1])
{
    out[0] = 0;
    if (!job_id) return false;
    const char *p = strstr(job_id, ".j_");
    if (!p) return false;
    p += 3;
    size_t n = 0;
    while (p[n]) {
        if (!tag_char(p[n]) || n >= LOG_TAG_MAX) return false;
        n++;
    }
    if (!n) return false;
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

void log_jobs_begin(log_jobs_t *j, const char *tag)
{
    if (!tag || !tag[0] || strlen(tag) > LOG_TAG_MAX) return;
    j->clock++;
    for (int i = 0; i < 2; i++)
        if (j->refs[i] && !strcmp(j->tag[i], tag)) {
            if (j->refs[i] < 255) j->refs[i]++;
            j->at[i] = j->clock;
            return;
        }
    int k = !j->refs[0] ? 0 : !j->refs[1] ? 1 : (j->at[0] <= j->at[1] ? 0 : 1);
    strcpy(j->tag[k], tag);
    j->refs[k] = 1;
    j->at[k] = j->clock;
}

void log_jobs_end(log_jobs_t *j, const char *tag)
{
    if (!tag || !tag[0]) return;
    for (int i = 0; i < 2; i++)
        if (j->refs[i] && !strcmp(j->tag[i], tag)) {
            if (--j->refs[i] == 0) j->tag[i][0] = 0;
            return;
        }
}

const char *log_jobs_current(const log_jobs_t *j)
{
    int best = -1;
    for (int i = 0; i < 2; i++)
        if (j->refs[i] && (best < 0 || j->at[i] > j->at[best])) best = i;
    return best < 0 ? "" : j->tag[best];
}

/* ── upload ───────────────────────────────────────────────────────────────── */

bool log_upload_wanted(const char *text, const char *job)
{
    if (job && job[0]) return true;
    if (!text) return false;
    /* ESP_LOG with colours: "\033[0;31mE (123) tag: ..." */
    if (text[0] == '\033' && text[1] == '[') {
        const char *m = strchr(text, 'm');
        if (!m || m - text > 12) return false;
        text = m + 1;
    }
    if ((text[0] == 'E' || text[0] == 'W') && text[1] == ' ' && text[2] == '(') return true;
    if (!strncmp(text, "EVT ", 4)) return strncmp(text + 4, "TL ", 3) != 0;
    return false;
}

/* bytes cJSON prints for this string, quotes included */
static size_t json_len(const char *s)
{
    size_t n = 2;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        n += (*p == '"' || *p == '\\' || *p == '\b' || *p == '\f' || *p == '\n' || *p == '\r' || *p == '\t') ? 2
           : *p < 0x20 ? 6 : 1;
    return n;
}

/* text clipped to LOG_UPLOAD_TEXT_MAX bytes, never mid UTF-8 sequence */
static void clip(const char *text, char out[LOG_UPLOAD_TEXT_MAX + 1])
{
    size_t n = strlen(text);
    if (n > LOG_UPLOAD_TEXT_MAX) {
        n = LOG_UPLOAD_TEXT_MAX;
        while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) n--;
    }
    memcpy(out, text, n);
    out[n] = 0;
}

#define FRAME_HEAD_RESERVE 192   /* {"t":"log","boot":…,"up":…,"prev":true,"sync":[…,…],"lines":[]} */

cJSON *log_upload_frame(const log_line_t *lines, size_t n, const log_frame_opts_t *o,
                        uint32_t end_seq, uint32_t *cursor)
{
    int max = o->max_lines < 1 ? 1 : o->max_lines > LOG_UPLOAD_LINES_MAX ? LOG_UPLOAD_LINES_MAX : o->max_lines;
    uint32_t from = *cursor;
    *cursor = end_seq;
    cJSON *arr = NULL;
    int taken = 0;
    size_t bytes = FRAME_HEAD_RESERVE;
    char text[LOG_UPLOAD_TEXT_MAX + 1];
    for (size_t i = 0; i < n; i++) {
        const log_line_t *l = &lines[i];
        if (l->seq < from || !log_upload_wanted(l->text, l->job)) continue;
        const char *job = l->job ? l->job : "";
        clip(l->text ? l->text : "", text);
        size_t need = 24 + json_len(job) + json_len(text);   /* [seq,ms,"tag","text"], */
        if (taken >= max || bytes + need > LOG_UPLOAD_FRAME_MAX) { *cursor = l->seq; break; }
        if (!arr && !(arr = cJSON_CreateArray())) { *cursor = from; return NULL; }
        cJSON *row = cJSON_CreateArray();
        if (!row) break;
        cJSON_AddItemToArray(row, cJSON_CreateNumber(l->seq));
        cJSON_AddItemToArray(row, cJSON_CreateNumber(l->ms));
        cJSON_AddItemToArray(row, cJSON_CreateString(job));
        cJSON_AddItemToArray(row, cJSON_CreateString(text));
        cJSON_AddItemToArray(arr, row);
        bytes += need;
        taken++;
    }
    if (!taken) { cJSON_Delete(arr); return NULL; }
    cJSON *f = cJSON_CreateObject();
    if (!f) { cJSON_Delete(arr); *cursor = from; return NULL; }
    cJSON_AddStringToObject(f, "t", "log");
    cJSON_AddNumberToObject(f, "boot", o->boot);
    cJSON_AddNumberToObject(f, "up", o->up_ms);
    if (o->prev) {
        cJSON_AddBoolToObject(f, "prev", true);
        if (o->has_sync) {
            cJSON *s = cJSON_AddArrayToObject(f, "sync");
            cJSON_AddItemToArray(s, cJSON_CreateNumber(o->sync_up_ms));
            cJSON_AddItemToArray(s, cJSON_CreateNumber((double)o->sync_wall_ms));
        }
    }
    cJSON_AddItemToObject(f, "lines", arr);
    return f;
}

uint32_t log_upload_backlog(const uint32_t *wanted_seqs, size_t n, uint32_t cursor, size_t keep)
{
    size_t left = 0, first = n;
    for (size_t i = 0; i < n; i++)
        if (wanted_seqs[i] >= cursor) { if (first == n) first = i; left++; }
    if (left <= keep) return cursor;
    return wanted_seqs[first + (left - keep)];
}

/* ── persistence ──────────────────────────────────────────────────────────── */

int64_t log_persist_delay_us(int64_t now_us, int64_t last_us)
{
    int64_t wait = LOG_PERSIST_NOT_BEFORE_US - now_us;
    if (last_us) {
        int64_t gap = last_us + LOG_PERSIST_MIN_GAP_US - now_us;
        if (gap > wait) wait = gap;
    }
    return wait > 0 ? wait : 0;
}

bool log_persist_due(bool requested, uint32_t wanted_since_last)
{
    return requested || wanted_since_last > 0;
}

#define HEADER_TAG "#molty-log 2 "

int log_persist_header(char *out, size_t cap, uint32_t boot, uint32_t sync_up_ms, int64_t sync_wall_ms)
{
    int w = snprintf(out, cap, HEADER_TAG "boot=%" PRIu32 " sync_up=%" PRIu32 " sync_wall=%" PRId64,
                     boot, sync_up_ms, sync_wall_ms);
    return w < 0 || (size_t)w >= cap ? -1 : w;
}

bool log_persist_parse_header(const char *line, uint32_t *boot, uint32_t *sync_up_ms, int64_t *sync_wall_ms)
{
    if (strncmp(line, HEADER_TAG, sizeof(HEADER_TAG) - 1)) return false;
    unsigned long b, u;
    long long w;
    if (sscanf(line + sizeof(HEADER_TAG) - 1, "boot=%lu sync_up=%lu sync_wall=%lld", &b, &u, &w) != 3) return false;
    *boot = (uint32_t)b; *sync_up_ms = (uint32_t)u; *sync_wall_ms = (int64_t)w;
    return true;
}

int log_persist_line(char *out, size_t cap, const log_line_t *l)
{
    const char *job = l->job && l->job[0] ? l->job : "-";
    int w = snprintf(out, cap, "%" PRIu32 " %" PRIu32 " %s %s", l->seq, l->ms, job, l->text ? l->text : "");
    if (w < 0 || (size_t)w >= cap) return -1;
    for (int i = 0; i < w; i++) if (out[i] == '\n' || out[i] == '\r') out[i] = ' ';
    return w;
}

bool log_persist_parse_line(char *line, log_line_t *out)
{
    char *p = line, *end;
    unsigned long seq = strtoul(p, &end, 10);
    if (end == p || *end != ' ') return false;
    p = end + 1;
    unsigned long ms = strtoul(p, &end, 10);
    if (end == p || *end != ' ') return false;
    p = end + 1;
    char *sp = strchr(p, ' ');
    if (!sp || sp == p || sp - p > LOG_TAG_MAX) return false;
    *sp = 0;
    out->seq = (uint32_t)seq;
    out->ms = (uint32_t)ms;
    out->job = strcmp(p, "-") ? p : "";
    out->text = sp + 1;
    return true;
}
