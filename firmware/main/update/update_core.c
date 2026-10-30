/*
 * SPDX-License-Identifier: MIT
 *
 * update_core.c — firmware self-update rules (update_core.h). Plain C + cJSON.
 */
#include "update_core.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ── target ──────────────────────────────────────────────────────────────── */

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

bool upd_hex32(const char *hex, uint8_t out[32])
{
    if (!hex || strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

static bool safe_chars(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_'))
            return false;
    }
    return true;
}

static bool version_ok(const char *v)
{
    size_t n = strlen(v);
    return n >= 1 && n < UPD_VERSION_MAX && safe_chars(v, n) && !strchr(v, '_');
}

bool upd_confirm_label(const char *version, char out[UPD_LABEL_MAX + 1])
{
    size_t n = version ? strlen(version) : 0;
    if (n == 0 || n > UPD_LABEL_MAX) return false;
    snprintf(out, UPD_LABEL_MAX + 1, n < UPD_LABEL_MAX ? "v%s" : "%s", version);
    return true;
}

/* https://github.com/<repo>/releases/download/<tag>/molty-<version>-<board>-ota.bin */
static const char *url_check(const char *url, const char *version, const upd_policy_t *p)
{
    char prefix[160];
    int k = snprintf(prefix, sizeof(prefix), "https://github.com/%s/releases/download/", p->repo ? p->repo : "");
    if (!p->repo || !p->repo[0] || k <= 0 || (size_t)k >= sizeof(prefix)) return "bad_url";
    if (strlen(url) >= UPD_URL_MAX || strncmp(url, prefix, (size_t)k)) return "bad_url";
    const char *tag = url + k, *slash = strchr(tag, '/');
    if (!slash) return "bad_url";
    size_t tn = (size_t)(slash - tag);
    if (tn < 1 || tn > UPD_TAG_MAX || !safe_chars(tag, tn) || (tn <= 2 && strspn(tag, ".") == tn)) return "bad_url";
    const char *file = slash + 1;
    size_t fn = strlen(file);
    if (!safe_chars(file, fn)) return "bad_url";          /* also: no more '/', '?', '#', '%' */
    char head[8 + UPD_VERSION_MAX];
    int h = snprintf(head, sizeof(head), "molty-%s-", version);
    static const char tail[] = "-ota.bin";
    size_t tl = sizeof(tail) - 1;
    if (h <= 0 || fn <= (size_t)h + tl || strncmp(file, head, (size_t)h) || strcmp(file + fn - tl, tail)) return "bad_url";
    size_t bn = fn - (size_t)h - tl;                      /* the board between them */
    if (!p->board || strlen(p->board) != bn || strncmp(file + h, p->board, bn)) return "wrong_board";
    return NULL;
}

static bool whole(const cJSON *v, double lo, double hi)
{
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) && floor(v->valuedouble) == v->valuedouble &&
           v->valuedouble >= lo && v->valuedouble <= hi;
}

const char *upd_parse(const cJSON *args, bool link, const upd_policy_t *p, upd_target_t *t)
{
    const cJSON *job = cJSON_GetObjectItemCaseSensitive(args, "job");
    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(args, "version");
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(args, "url");
    const cJSON *sha = cJSON_GetObjectItemCaseSensitive(args, "sha256");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(args, "size");
    const cJSON *to = cJSON_GetObjectItemCaseSensitive(args, "timeout_ms");
    upd_target_t n;
    memset(&n, 0, sizeof(n));
    if (!cJSON_IsObject(args) || !p || !t) return "invalid_args";
    if (link) {
        if (!cJSON_IsString(job) || !job->valuestring[0] || strlen(job->valuestring) >= UPD_JOB_MAX) return "invalid_args";
        if (to && !whole(to, UPD_TIMEOUT_MIN, UPD_TIMEOUT_MAX)) return "invalid_args";
        strcpy(n.job, job->valuestring);
        n.timeout_ms = to ? (unsigned)to->valuedouble : UPD_TIMEOUT_DEF;
    }
    if (!cJSON_IsString(ver) || !version_ok(ver->valuestring)) return "invalid_args";
    char label[UPD_LABEL_MAX + 1];
    if (link && !upd_confirm_label(ver->valuestring, label)) return "version_too_long";
    if (!cJSON_IsString(sha) || !upd_hex32(sha->valuestring, n.sha256)) return "invalid_args";
    if (!whole(size, 1, 4294967295.0)) return "invalid_args";
    if (!cJSON_IsString(url)) return "invalid_args";
    const char *e = url_check(url->valuestring, ver->valuestring, p);
    if (e) return e;
    if (size->valuedouble < UPD_IMAGE_HEAD) return "invalid_args";   /* not even an image header */
    if (size->valuedouble > p->slot_size) return "too_big";
    strcpy(n.version, ver->valuestring);
    strcpy(n.url, url->valuestring);
    n.size = (uint32_t)size->valuedouble;
    *t = n;
    return NULL;
}

/* ── progress ────────────────────────────────────────────────────────────── */

bool upd_progress_due(upd_progress_t *p, uint32_t done, uint32_t total, int64_t now_us, int *pct)
{
    if (!total) return false;
    int step = (int)((uint64_t)(done > total ? total : done) * 10 / total) * 10;   /* 0..100 */
    if (p->next < 10) p->next = 10;
    if (step < p->next) return false;
    if (p->any && now_us - p->last_us < 1000000) return false;
    p->any = true; p->last_us = now_us; p->next = step + 10;
    if (pct) *pct = step;
    return true;
}

/* ── session ─────────────────────────────────────────────────────────────── */

const char *upd_state_name(upd_state_t s)
{
    static const char *const names[] = { "idle", "confirm", "accepted", "downloading", "flashing", "rebooting", "failed" };
    return (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "idle";
}

bool upd_busy(const upd_session_t *s) { return s->state >= UPD_CONFIRM && s->state <= UPD_REBOOTING; }

const char *upd_request(upd_session_t *s, const cJSON *args, bool link, const upd_policy_t *p)
{
    if (upd_busy(s)) return "busy";
    upd_target_t t;
    const char *e = upd_parse(args, link, p, &t);
    if (e) return e;
    memset(s, 0, sizeof(*s));
    s->t = t;
    s->state = link ? UPD_CONFIRM : UPD_ACCEPTED;
    return NULL;
}

cJSON *upd_evt_frame(const char *job, const char *state, double progress, const char *reason, const char *error)
{
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
    cJSON_AddStringToObject(j, "t", "evt");
    cJSON_AddStringToObject(j, "name", "update");
    cJSON *d = cJSON_AddObjectToObject(j, "data");
    if (!d) { cJSON_Delete(j); return NULL; }
    cJSON_AddStringToObject(d, "job", job ? job : "");
    cJSON_AddStringToObject(d, "state", state);
    if (progress >= 0) cJSON_AddNumberToObject(d, "progress", round(progress * 100) / 100);
    if (reason) cJSON_AddStringToObject(d, "reason", reason);
    if (error) cJSON_AddStringToObject(d, "error", error);
    return j;
}

static cJSON *evt(const upd_session_t *s, const char *state, double progress, const char *reason, const char *error)
{
    return s->t.job[0] ? upd_evt_frame(s->t.job, state, progress, reason, error) : NULL;
}

bool upd_answer(upd_session_t *s, int index, const char *reason, cJSON **out)
{
    *out = NULL;
    if (s->state != UPD_CONFIRM) return false;
    if (index == 0) {
        s->state = UPD_ACCEPTED;
        *out = evt(s, "accepted", -1, NULL, NULL);
        return true;
    }
    s->state = UPD_IDLE;
    *out = evt(s, "declined", -1, index < 0 ? (reason && !strcmp(reason, "timeout") ? "timeout" : "cancelled") : NULL, NULL);
    return false;
}

void upd_step(upd_session_t *s, upd_state_t st, uint32_t done, uint32_t total, int64_t now_us,
              const char *error, cJSON **out)
{
    *out = NULL;
    if (s->state < UPD_ACCEPTED || s->state > UPD_FLASHING) return;   /* not running (or already rebooting) */
    switch (st) {
    case UPD_DOWNLOADING: {
        bool first = s->state != UPD_DOWNLOADING;
        s->state = UPD_DOWNLOADING;
        s->progress = total ? (double)(done > total ? total : done) / total : 0;
        int pct;
        if (first) *out = evt(s, "downloading", 0, NULL, NULL);
        else if (upd_progress_due(&s->prog, done, total, now_us, &pct)) *out = evt(s, "downloading", pct / 100.0, NULL, NULL);
        if (first) { s->prog.any = true; s->prog.last_us = now_us; }
        break;
    }
    case UPD_FLASHING:  s->state = UPD_FLASHING;  *out = evt(s, "flashing", -1, NULL, NULL); break;
    case UPD_REBOOTING: s->state = UPD_REBOOTING; *out = evt(s, "rebooting", -1, NULL, NULL); break;
    case UPD_FAILED:
        s->state = UPD_FAILED;
        snprintf(s->error, sizeof(s->error), "%s", error && error[0] ? error : "failed");
        *out = evt(s, "failed", -1, NULL, s->error);
        break;
    default: break;
    }
}

cJSON *upd_status_json(const upd_session_t *s)
{
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "state", upd_state_name(s->state));
    if (s->state != UPD_IDLE) cJSON_AddStringToObject(j, "version", s->t.version);
    if (s->state == UPD_DOWNLOADING) cJSON_AddNumberToObject(j, "progress", round(s->progress * 1000) / 1000);
    if (s->state == UPD_FAILED) cJSON_AddStringToObject(j, "error", s->error);
    return j;
}

/* ── download ────────────────────────────────────────────────────────────── */

const char *upd_download(const upd_target_t *t, const upd_io_t *io, uint8_t *buf, size_t cap)
{
    int64_t len = -1;
    const char *e = io->open(io->ctx, t->url, &len);
    if (e) { io->close(io->ctx); return e; }
    if (len >= 0 && len != (int64_t)t->size) { io->close(io->ctx); return "size_mismatch"; }
    if ((e = io->ota_begin(io->ctx))) { io->close(io->ctx); return e; }
    io->sha_start(io->ctx);
    uint8_t head[UPD_IMAGE_HEAD];
    size_t have = 0;
    uint32_t total = 0;
    io->step(io->ctx, UPD_DOWNLOADING, 0, t->size);
    for (;;) {
        int n = io->read(io->ctx, buf, (int)cap);
        if (n < 0) { e = "read"; break; }
        if (n == 0) break;
        if ((uint32_t)n > t->size - total) { e = "size_mismatch"; break; }   /* more than announced */
        if (have < sizeof(head)) {
            size_t k = sizeof(head) - have < (size_t)n ? sizeof(head) - have : (size_t)n;
            memcpy(head + have, buf, k);
            have += k;
            /* the board tag sits in the first bytes: refuse another board's image before
             * any more of it is written (-1, not an app image: esp_ota_end judges it) */
            if (have == sizeof(head) && io->board_check(io->ctx, head, have) == 0) { e = "wrong_board"; break; }
        }
        io->sha_update(io->ctx, buf, (size_t)n);
        if ((e = io->ota_write(io->ctx, buf, (size_t)n))) break;
        total += (uint32_t)n;
        io->step(io->ctx, UPD_DOWNLOADING, total, t->size);
    }
    io->close(io->ctx);
    if (!e && total != t->size) e = "size_mismatch";
    if (!e) {
        uint8_t got[32];
        io->sha_finish(io->ctx, got);
        if (memcmp(got, t->sha256, 32)) e = "sha256_mismatch";
    }
    if (e) { io->ota_abort(io->ctx); return e; }
    io->step(io->ctx, UPD_FLASHING, total, t->size);
    if ((e = io->ota_end(io->ctx))) return e;               /* esp_ota_end frees the handle on failure too */
    return io->ota_set_boot(io->ctx);
}

/* ── the mark-valid rule ─────────────────────────────────────────────────── */

upd_boot_t upd_boot_rule(bool pending_verify, bool armed)
{
    if (!pending_verify) return UPD_BOOT_NOTHING;
    return armed ? UPD_BOOT_AWAIT_WELCOME : UPD_BOOT_VALID_NOW;
}

upd_guard_t upd_guard_step(upd_boot_t *r, bool welcomed, int64_t since_boot_us)
{
    upd_guard_t g;
    switch (*r) {
    case UPD_BOOT_VALID_NOW: g = UPD_GUARD_MARK_VALID; break;
    case UPD_BOOT_AWAIT_WELCOME:
        if (welcomed) g = UPD_GUARD_MARK_VALID;
        else g = since_boot_us >= UPD_WELCOME_WINDOW_US ? UPD_GUARD_ROLLBACK : UPD_GUARD_WAIT;
        break;
    default: return UPD_GUARD_DONE;
    }
    if (g != UPD_GUARD_WAIT) *r = UPD_BOOT_NOTHING;   /* decided: nobody decides again */
    return g;
}

bool upd_should_arm(bool link_welcomed) { return link_welcomed; }
