/* update_core.c on the host: the self-update's argument rules (https + the baked repo +
 * this board, sha256 hex, size bounds), the confirm flow and its events (exact frames),
 * progress throttling, the download's checks and their order (board tag, size, sha256
 * before the boot partition is ever set), and the mark-valid rule. */
#include "update_core.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPO  "apresmoi/molty.uno"
#define BOARD "waveshare-esp32-s3-touch-lcd-1.46"
#define SHA   "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
#define URL   "https://github.com/" REPO "/releases/download/v0.2.0/molty-0.2.0-" BOARD "-ota.bin"
static const upd_policy_t P = { REPO, BOARD, 0x3c0000 };

static char *print(cJSON *j) { char *t = cJSON_PrintUnformatted(j); cJSON_Delete(j); return t; }
static bool frame_is(cJSON *j, const char *want) {
    if (!j) return !want;
    char *t = print(j); bool ok = want && !strcmp(t, want);
    if (!ok) fprintf(stderr, "got  %s\nwant %s\n", t, want ? want : "(none)");
    free(t); return ok;
}

/* args with one field replaced (NULL value: removed) */
static const char *parse_with(bool link, const char *key, const char *json_value, upd_target_t *t) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "{\"job\":\"molty-x.j_1\",\"version\":\"0.2.0\",\"url\":\"%s\",\"sha256\":\"%s\",\"size\":1234567,\"timeout_ms\":60000}", URL, SHA);
    cJSON *a = cJSON_Parse(buf); assert(a);
    if (key) {
        cJSON_DeleteItemFromObject(a, key);
        if (json_value) { cJSON *v = cJSON_Parse(json_value); assert(v); cJSON_AddItemToObject(a, key, v); }
    }
    upd_target_t tmp; const char *e = upd_parse(a, link, &P, t ? t : &tmp);
    cJSON_Delete(a);
    return e;
}
#define OK(e) (!(e))
#define IS(e, code) ((e) && !strcmp((e), (code)))

static void test_parse(void) {
    upd_target_t t;
    assert(OK(parse_with(true, NULL, NULL, &t)));
    assert(!strcmp(t.job, "molty-x.j_1") && !strcmp(t.version, "0.2.0") && !strcmp(t.url, URL) && t.size == 1234567 && t.timeout_ms == 60000);
    for (int i = 0; i < 32; i++) assert(t.sha256[i] == 0xdd);
    /* the confirm question shows the version it installs, within the ask's 8-char labels */
    char l[UPD_LABEL_MAX + 1];
    assert(upd_confirm_label("0.2.0", l) && !strcmp(l, "v0.2.0"));
    assert(upd_confirm_label("0.10.12", l) && !strcmp(l, "v0.10.12"));
    assert(upd_confirm_label("10.10.10", l) && !strcmp(l, "10.10.10"));   /* 8: no room for the v */
    assert(!upd_confirm_label("0.2.0-rc.1", l) && !upd_confirm_label("", l));
    /* a version the question cannot show: the link's cmd is refused (consent always names
     * the version); the console's POST (the click is the consent, its page shows it) is not */
    assert(IS(parse_with(true, "version", "\"0.2.0-rc.1\"", NULL), "version_too_long"));
    assert(!IS(parse_with(false, "version", "\"0.2.0-rc.1\"", NULL), "version_too_long"));
    /* sha256: exactly 64 hex digits, either case */
    assert(OK(parse_with(true, "sha256", "\"" "0123456789ABCDEFabcdef0123456789ABCDEFabcdef0123456789abcdef0123" "\"", &t)) && t.sha256[0] == 0x01 && t.sha256[5] == 0xab && t.sha256[31] == 0x23);
    assert(IS(parse_with(true, "sha256", "\"" "ddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd" "\"", NULL), "invalid_args"));   /* 63 */
    assert(IS(parse_with(true, "sha256", "\"" SHA "d\"", NULL), "invalid_args"));
    assert(IS(parse_with(true, "sha256", "\"" "gddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd" "\"", NULL), "invalid_args"));
    assert(IS(parse_with(true, "sha256", NULL, NULL), "invalid_args"));
    assert(IS(parse_with(true, "sha256", "64", NULL), "invalid_args"));
    /* size: a whole number, at least an image header, at most the OTA slot */
    assert(IS(parse_with(true, "size", "0", NULL), "invalid_args"));
    assert(IS(parse_with(true, "size", "-5", NULL), "invalid_args"));
    assert(IS(parse_with(true, "size", "1000.5", NULL), "invalid_args"));
    assert(IS(parse_with(true, "size", "\"1234\"", NULL), "invalid_args"));
    assert(IS(parse_with(true, "size", "351", NULL), "invalid_args"));
    assert(OK(parse_with(true, "size", "352", NULL)));
    assert(OK(parse_with(true, "size", "3932160", NULL)));                  /* = the slot */
    assert(IS(parse_with(true, "size", "3932161", NULL), "too_big"));
    assert(IS(parse_with(true, "size", "1e12", NULL), "invalid_args"));
    /* job + timeout: required / bounded from the link, ignored from the console */
    assert(IS(parse_with(true, "job", NULL, NULL), "invalid_args"));
    assert(IS(parse_with(true, "job", "\"\"", NULL), "invalid_args"));
    char longjob[160]; memset(longjob, 'j', sizeof(longjob)); longjob[0] = '"'; longjob[130] = '"'; longjob[131] = 0;   /* 129 chars */
    assert(IS(parse_with(true, "job", longjob, NULL), "invalid_args"));
    assert(OK(parse_with(true, "timeout_ms", NULL, &t)) && t.timeout_ms == UPD_TIMEOUT_DEF);
    assert(IS(parse_with(true, "timeout_ms", "999", NULL), "invalid_args"));
    assert(IS(parse_with(true, "timeout_ms", "120001", NULL), "invalid_args"));
    assert(IS(parse_with(true, "timeout_ms", "1500.5", NULL), "invalid_args"));
    assert(OK(parse_with(false, "job", NULL, &t)) && !t.job[0] && t.timeout_ms == 0);
    assert(OK(parse_with(false, "timeout_ms", "5", NULL)));
    /* version */
    assert(IS(parse_with(true, "version", "\"\"", NULL), "invalid_args"));
    assert(IS(parse_with(true, "version", "\"0.2/0\"", NULL), "invalid_args"));
    assert(IS(parse_with(true, "version", "020", NULL), "invalid_args"));
    /* url: https, github.com, the baked repo, /releases/download/<tag>/, this version, this board */
    const char *bad[] = {
        "\"http://github.com/" REPO "/releases/download/v0.2.0/molty-0.2.0-" BOARD "-ota.bin\"",
        "\"https://evil.example/" REPO "/releases/download/v0.2.0/molty-0.2.0-" BOARD "-ota.bin\"",
        "\"https://github.com.evil.example/" REPO "/releases/download/v0.2.0/molty-0.2.0-" BOARD "-ota.bin\"",
        "\"https://github.com/someone/molty.uno/releases/download/v0.2.0/molty-0.2.0-" BOARD "-ota.bin\"",
        "\"https://github.com/" REPO "x/releases/download/v0.2.0/molty-0.2.0-" BOARD "-ota.bin\"",
        "\"https://github.com/" REPO "/releases/latest/download/molty-0.2.0-" BOARD "-ota.bin\"",
        "\"https://github.com/" REPO "/releases/download/../molty-0.2.0-" BOARD "-ota.bin\"",
        "\"https://github.com/" REPO "/releases/download//molty-0.2.0-" BOARD "-ota.bin\"",
        "\"https://github.com/" REPO "/releases/download/v0.2.0/x/molty-0.2.0-" BOARD "-ota.bin\"",
        "\"https://github.com/" REPO "/releases/download/v0.2.0/molty-0.2.0-" BOARD "-ota.bin?x=1\"",
        "\"https://github.com/" REPO "/releases/download/v0.2.0/molty-0.2.0-" BOARD "-ota.bin#x\"",
        "\"https://github.com/" REPO "/releases/download/v0.2.0/molty-0.2.0-" BOARD "%2Dota.bin\"",
        "\"https://github.com/" REPO "/releases/download/v0.2.0/molty-0.3.0-" BOARD "-ota.bin\"",   /* another version */
        "\"https://github.com/" REPO "/releases/download/v0.2.0/molty-0.2.0-" BOARD "-ota.elf\"",
        "\"https://github.com/" REPO "/releases/download/v0.2.0/molty-0.2.0--ota.bin\"",
        "\"https://user@github.com/" REPO "/releases/download/v0.2.0/molty-0.2.0-" BOARD "-ota.bin\"",
        "\"HTTPS://github.com/" REPO "/releases/download/v0.2.0/molty-0.2.0-" BOARD "-ota.bin\"",
        "17",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        const char *e = parse_with(true, "url", bad[i], NULL);
        if (!(IS(e, "bad_url") || (i == sizeof(bad) / sizeof(bad[0]) - 1 && IS(e, "invalid_args")))) {
            fprintf(stderr, "url %s → %s\n", bad[i], e ? e : "ok"); assert(0);
        }
    }
    assert(IS(parse_with(true, "url", "\"https://github.com/" REPO "/releases/download/v0.2.0/molty-0.2.0-waveshare-esp32-s3-touch-lcd-1.28-ota.bin\"", NULL), "wrong_board"));
    assert(IS(parse_with(false, "url", "\"https://github.com/" REPO "/releases/download/v0.2.0/molty-0.2.0-waveshare-esp32-s3-touch-lcd-1.4-ota.bin\"", NULL), "wrong_board"));
    assert(OK(parse_with(true, "url", "\"https://github.com/" REPO "/releases/download/0.2.0/molty-0.2.0-" BOARD "-ota.bin\"", NULL)));   /* any safe tag */
    /* a prerelease version */
    cJSON *a = cJSON_Parse("{\"version\":\"0.3.0-rc.1\",\"url\":\"https://github.com/" REPO "/releases/download/v0.3.0-rc.1/molty-0.3.0-rc.1-" BOARD "-ota.bin\",\"sha256\":\"" SHA "\",\"size\":5000}");
    assert(OK(upd_parse(a, false, &P, &t)) && !strcmp(t.version, "0.3.0-rc.1"));
    cJSON_Delete(a);
    /* nothing to update into */
    upd_policy_t none = P; none.slot_size = 0;
    a = cJSON_Parse("{\"version\":\"0.2.0\",\"url\":\"" URL "\",\"sha256\":\"" SHA "\",\"size\":5000}");
    assert(IS(upd_parse(a, false, &none, &t), "too_big"));
    upd_policy_t norepo = P; norepo.repo = "";
    assert(IS(upd_parse(a, false, &norepo, &t), "bad_url"));
    cJSON_Delete(a);
}

static void test_progress(void) {
    upd_progress_t p = {0}; int pct = -1;
    assert(!upd_progress_due(&p, 0, 1000, 0, &pct));
    assert(!upd_progress_due(&p, 99, 1000, 5000000, &pct));
    assert(upd_progress_due(&p, 100, 1000, 5000000, &pct) && pct == 10);
    assert(!upd_progress_due(&p, 150, 1000, 9000000, &pct));            /* no new 10 % */
    assert(!upd_progress_due(&p, 350, 1000, 5999999, &pct));            /* 30 %, but < 1 s */
    assert(upd_progress_due(&p, 350, 1000, 6000000, &pct) && pct == 30); /* skips to the step reached */
    assert(!upd_progress_due(&p, 399, 1000, 9000000, &pct));
    assert(upd_progress_due(&p, 1000, 1000, 9000000, &pct) && pct == 100);
    assert(!upd_progress_due(&p, 1000, 1000, 20000000, &pct));
    assert(!upd_progress_due(&p, 5, 0, 20000000, &pct));                /* no total */
    /* a fast download (everything in 1 s) reports at most twice: the at-most-1/s floor */
    upd_progress_t q = {0}; int n = 0;
    for (uint32_t d = 0; d <= 1000; d += 10) n += upd_progress_due(&q, d, 1000, (int64_t)d * 1000, &pct);
    assert(n == 1);
}

static cJSON *args_json(void) {
    return cJSON_Parse("{\"job\":\"molty-x.j_1\",\"version\":\"0.2.0\",\"url\":\"" URL "\",\"sha256\":\"" SHA "\",\"size\":1234567,\"timeout_ms\":60000}");
}
#define EV(state_extra) "{\"t\":\"evt\",\"name\":\"update\",\"data\":{\"job\":\"molty-x.j_1\"," state_extra "}}"

static void test_session(void) {
    upd_session_t s; memset(&s, 0, sizeof(s));
    cJSON *a = args_json(), *ev;
    assert(!upd_busy(&s) && frame_is(upd_status_json(&s), "{\"ok\":true,\"state\":\"idle\"}"));
    /* refused args leave it idle */
    cJSON *bad = cJSON_Parse("{\"job\":\"j\",\"version\":\"0.2.0\",\"url\":\"http://x\",\"sha256\":\"" SHA "\",\"size\":5000}");
    assert(IS(upd_request(&s, bad, true, &P), "bad_url") && s.state == UPD_IDLE);
    cJSON_Delete(bad);

    /* later → declined, no reason */
    assert(OK(upd_request(&s, a, true, &P)) && s.state == UPD_CONFIRM && upd_busy(&s));
    assert(frame_is(upd_status_json(&s), "{\"ok\":true,\"state\":\"confirm\",\"version\":\"0.2.0\"}"));
    assert(IS(upd_request(&s, a, true, &P), "busy") && IS(upd_request(&s, a, false, &P), "busy"));
    assert(!upd_answer(&s, 1, NULL, &ev) && frame_is(ev, EV("\"state\":\"declined\"")) && s.state == UPD_IDLE);
    assert(!upd_answer(&s, 0, NULL, &ev) && !ev && s.state == UPD_IDLE);   /* outside confirm: ignored */
    /* timeout / cancelled */
    assert(OK(upd_request(&s, a, true, &P)));
    assert(!upd_answer(&s, -1, "timeout", &ev) && frame_is(ev, EV("\"state\":\"declined\",\"reason\":\"timeout\"")));
    assert(OK(upd_request(&s, a, true, &P)));
    assert(!upd_answer(&s, -1, "cancelled", &ev) && frame_is(ev, EV("\"state\":\"declined\",\"reason\":\"cancelled\"")));
    /* steps before an accept are ignored */
    assert(OK(upd_request(&s, a, true, &P)));
    upd_step(&s, UPD_DOWNLOADING, 0, 100, 0, NULL, &ev); assert(!ev && s.state == UPD_CONFIRM);
    /* update → accepted, then the download's steps */
    assert(upd_answer(&s, 0, NULL, &ev) && frame_is(ev, EV("\"state\":\"accepted\"")) && s.state == UPD_ACCEPTED && upd_busy(&s));
    upd_step(&s, UPD_DOWNLOADING, 0, 1000, 1000000, NULL, &ev);
    assert(frame_is(ev, EV("\"state\":\"downloading\",\"progress\":0")));
    upd_step(&s, UPD_DOWNLOADING, 150, 1000, 1500000, NULL, &ev); assert(!ev);          /* 10 % but < 1 s */
    assert(frame_is(upd_status_json(&s), "{\"ok\":true,\"state\":\"downloading\",\"version\":\"0.2.0\",\"progress\":0.15}"));
    upd_step(&s, UPD_DOWNLOADING, 160, 1000, 2000000, NULL, &ev);
    assert(frame_is(ev, EV("\"state\":\"downloading\",\"progress\":0.1")));
    upd_step(&s, UPD_DOWNLOADING, 190, 1000, 9000000, NULL, &ev); assert(!ev);
    upd_step(&s, UPD_DOWNLOADING, 777, 1000, 9000000, NULL, &ev);
    assert(frame_is(ev, EV("\"state\":\"downloading\",\"progress\":0.7")));
    int events = 0;   /* whatever the chunking: ≤ 1 per 10 % (+ the first) */
    for (uint32_t d = 777; d <= 1000; d++) { upd_step(&s, UPD_DOWNLOADING, d, 1000, 9000000 + (int64_t)d * 100000, NULL, &ev); if (ev) { events++; cJSON_Delete(ev); } }
    assert(events == 3);   /* 80, 90, 100 */
    upd_step(&s, UPD_FLASHING, 0, 0, 0, NULL, &ev); assert(frame_is(ev, EV("\"state\":\"flashing\"")));
    upd_step(&s, UPD_REBOOTING, 0, 0, 0, NULL, &ev); assert(frame_is(ev, EV("\"state\":\"rebooting\"")));
    assert(frame_is(upd_status_json(&s), "{\"ok\":true,\"state\":\"rebooting\",\"version\":\"0.2.0\"}"));
    upd_step(&s, UPD_FAILED, 0, 0, 0, "late", &ev); assert(!ev && s.state == UPD_REBOOTING);   /* nothing after rebooting */
    assert(IS(upd_request(&s, a, true, &P), "busy"));

    /* a failure: event with the error, then not busy (a retry may start) */
    memset(&s, 0, sizeof(s));
    assert(OK(upd_request(&s, a, true, &P)) && upd_answer(&s, 0, NULL, &ev)); cJSON_Delete(ev);
    upd_step(&s, UPD_FAILED, 0, 0, 0, "sha256_mismatch", &ev);
    assert(frame_is(ev, EV("\"state\":\"failed\",\"error\":\"sha256_mismatch\"")) && !upd_busy(&s));
    assert(frame_is(upd_status_json(&s), "{\"ok\":true,\"state\":\"failed\",\"version\":\"0.2.0\",\"error\":\"sha256_mismatch\"}"));
    assert(OK(upd_request(&s, a, true, &P)) && s.state == UPD_CONFIRM && !s.error[0]);

    /* the console: starts accepted (the click is the consent), no relay job → no events */
    memset(&s, 0, sizeof(s));
    cJSON *c = cJSON_Parse("{\"version\":\"0.2.0\",\"url\":\"" URL "\",\"sha256\":\"" SHA "\",\"size\":1234567}");
    assert(OK(upd_request(&s, c, false, &P)) && s.state == UPD_ACCEPTED && upd_busy(&s));
    assert(!upd_answer(&s, 0, NULL, &ev) && !ev);          /* no question to answer */
    upd_step(&s, UPD_DOWNLOADING, 0, 10, 0, NULL, &ev); assert(!ev && s.state == UPD_DOWNLOADING);
    upd_step(&s, UPD_DOWNLOADING, 10, 10, 5000000, NULL, &ev); assert(!ev);
    upd_step(&s, UPD_FAILED, 0, 0, 0, NULL, &ev); assert(!ev && !strcmp(s.error, "failed"));
    cJSON_Delete(c);
    cJSON_Delete(a);
}

/* ── the download through fake I/O ─────────────────────────────────────────── */
static struct {
    uint8_t *img; size_t len, pos;
    const char *open_err; int64_t content_len; int chunk; long fail_read_at;
    char log[512];
    size_t written; bool began, aborted, ended, boot_set; const char *end_err, *write_err;
    uint8_t digest[32]; uint32_t h;
} F;
static void logf_(const char *s) { strncat(F.log, s, sizeof(F.log) - strlen(F.log) - 1); }
static const char *f_open(void *c, const char *u, int64_t *len) { (void)c; (void)u; logf_("open "); *len = F.content_len; return F.open_err; }
static int f_read(void *c, uint8_t *b, int cap) {
    (void)c;
    if (F.fail_read_at >= 0 && (long)F.pos >= F.fail_read_at) return -1;
    size_t n = F.len - F.pos; if (n > (size_t)cap) n = (size_t)cap; if (n > (size_t)F.chunk) n = (size_t)F.chunk;
    memcpy(b, F.img + F.pos, n); F.pos += n; return (int)n;
}
static void f_close(void *c) { (void)c; logf_("close "); }
static const char *f_begin(void *c) { (void)c; logf_("begin "); F.began = true; return NULL; }
static const char *f_write(void *c, const uint8_t *b, size_t n) { (void)c; (void)b; F.written += n; return F.write_err; }
static const char *f_end(void *c) { (void)c; logf_("end "); F.ended = true; return F.end_err; }
static const char *f_boot(void *c) { (void)c; logf_("boot "); F.boot_set = true; return NULL; }
static void f_abort(void *c) { (void)c; logf_("abort "); F.aborted = true; }
/* a stand-in digest (FNV-1a spread over 32 bytes): only equality matters here */
static void f_sha_start(void *c) { (void)c; F.h = 2166136261u; }
static void f_sha_update(void *c, const uint8_t *b, size_t n) { (void)c; for (size_t i = 0; i < n; i++) { F.h ^= b[i]; F.h *= 16777619u; } }
static void f_sha_finish(void *c, uint8_t out[32]) { (void)c; for (int i = 0; i < 32; i++) out[i] = (uint8_t)(F.h >> (8 * (i % 4))) ^ (uint8_t)i; }
static int f_board(void *c, const uint8_t *head, size_t len) { (void)c; assert(len == UPD_IMAGE_HEAD); return head[UPD_IMAGE_HEAD - 1] == 'X' ? 0 : 1; }
static unsigned f_steps, f_flashing; static uint32_t f_last_done;
static void f_step(void *c, upd_state_t st, uint32_t done, uint32_t total) {
    (void)c; (void)total;
    if (st == UPD_FLASHING) { f_flashing++; logf_("flashing "); assert(!F.boot_set); }
    else { assert(done >= f_last_done); f_last_done = done; f_steps++; }
}
static const upd_io_t IO = { NULL, f_open, f_read, f_close, f_begin, f_write, f_end, f_boot, f_abort,
                             f_sha_start, f_sha_update, f_sha_finish, f_board, f_step };

static void digest_of(const uint8_t *b, size_t n, uint8_t out[32]) { f_sha_start(NULL); f_sha_update(NULL, b, n); f_sha_finish(NULL, out); }
static const char *run(size_t img_len, upd_target_t *t) {
    static uint8_t buf[700];
    F.pos = 0; F.written = 0; F.began = F.aborted = F.ended = F.boot_set = false; F.log[0] = 0;
    F.len = img_len; f_steps = f_flashing = 0; f_last_done = 0;
    return upd_download(t, &IO, buf, sizeof(buf));
}
static void reset_fake(uint8_t *img, size_t len) {
    memset(&F, 0, sizeof(F)); F.img = img; F.len = len; F.content_len = (int64_t)len; F.chunk = 1000; F.fail_read_at = -1;
}

static void test_download(void) {
    size_t N = 5000;
    uint8_t *img = malloc(N + 100); assert(img);
    for (size_t i = 0; i < N + 100; i++) img[i] = (uint8_t)(i * 7 + 3);
    img[UPD_IMAGE_HEAD - 1] = 'B';   /* this board */
    upd_target_t t; memset(&t, 0, sizeof(t)); t.size = (uint32_t)N; strcpy(t.url, URL);
    digest_of(img, N, t.sha256);

    /* the good path: board tag, size, sha256 checked; flashing; then the boot partition */
    reset_fake(img, N);
    assert(OK(run(N, &t)));
    assert(!strcmp(F.log, "open begin close flashing end boot ") && F.written == N && !F.aborted && f_steps >= 2);
    /* any chunking, including ones that split the board tag's head */
    for (int ch = 1; ch <= 700; ch = ch * 3 + 1) { reset_fake(img, N); F.chunk = ch; assert(OK(run(N, &t)) && F.boot_set); }
    /* no Content-Length (chunked): the exact size is still enforced */
    reset_fake(img, N); F.content_len = -1; assert(OK(run(N, &t)));

    /* sha256 mismatch: nothing set to boot, the slot aborted */
    reset_fake(img, N); t.sha256[7] ^= 1;
    assert(IS(run(N, &t), "sha256_mismatch") && !F.boot_set && F.aborted && !F.ended && !f_flashing);
    t.sha256[7] ^= 1;
    reset_fake(img, N); img[N - 1] ^= 0x40;                         /* a flipped bit in the last byte */
    assert(IS(run(N, &t), "sha256_mismatch") && !F.boot_set);
    img[N - 1] ^= 0x40;

    /* size: the announced length, more bytes than size, fewer bytes than size */
    reset_fake(img, N); F.content_len = (int64_t)N + 1;
    assert(IS(run(N, &t), "size_mismatch") && !F.began && !strcmp(F.log, "open close "));
    reset_fake(img, N + 100); F.content_len = -1;
    assert(IS(run(N + 100, &t), "size_mismatch") && !F.boot_set && F.aborted && F.written <= N);
    reset_fake(img, N - 1); F.content_len = -1;
    assert(IS(run(N - 1, &t), "size_mismatch") && !F.boot_set && F.aborted);

    /* another board's image: refused as soon as its head is in, before more is written */
    img[UPD_IMAGE_HEAD - 1] = 'X';
    reset_fake(img, N); F.chunk = 100; digest_of(img, N, t.sha256);
    assert(IS(run(N, &t), "wrong_board") && !F.boot_set && F.aborted && F.written < UPD_IMAGE_HEAD);
    img[UPD_IMAGE_HEAD - 1] = 'B'; digest_of(img, N, t.sha256);

    /* transport + flash failures pass through; never a boot partition */
    reset_fake(img, N); F.open_err = "http_404";
    assert(IS(run(N, &t), "http_404") && !F.began && !F.boot_set);
    reset_fake(img, N); F.open_err = "tls";
    assert(IS(run(N, &t), "tls") && !F.boot_set);
    reset_fake(img, N); F.fail_read_at = 2000;
    assert(IS(run(N, &t), "read") && F.aborted && !F.boot_set);
    reset_fake(img, N); F.write_err = "write";
    assert(IS(run(N, &t), "write") && F.aborted && !F.boot_set);
    reset_fake(img, N); F.end_err = "verify";
    assert(IS(run(N, &t), "verify") && F.ended && !F.boot_set && f_flashing == 1);
    free(img);
}

static void test_boot(void) {
    assert(upd_boot_rule(false, false) == UPD_BOOT_NOTHING && upd_boot_rule(false, true) == UPD_BOOT_NOTHING);
    assert(upd_boot_rule(true, false) == UPD_BOOT_VALID_NOW);       /* /api/ota, no relay link at install */
    assert(upd_boot_rule(true, true) == UPD_BOOT_AWAIT_WELCOME);   /* a self-update while linked */
    /* await: valid on welcome, rollback only after the window, wait in between */
    upd_boot_t r = UPD_BOOT_AWAIT_WELCOME;
    assert(upd_guard_step(&r, false, 0) == UPD_GUARD_WAIT && r == UPD_BOOT_AWAIT_WELCOME);
    assert(upd_guard_step(&r, false, UPD_WELCOME_WINDOW_US - 1) == UPD_GUARD_WAIT && r == UPD_BOOT_AWAIT_WELCOME);
    assert(upd_guard_step(&r, false, UPD_WELCOME_WINDOW_US) == UPD_GUARD_ROLLBACK);
    r = UPD_BOOT_AWAIT_WELCOME;
    assert(upd_guard_step(&r, true, 5) == UPD_GUARD_MARK_VALID);
    r = UPD_BOOT_AWAIT_WELCOME;
    assert(upd_guard_step(&r, true, UPD_WELCOME_WINDOW_US * 2) == UPD_GUARD_MARK_VALID);   /* welcome wins */
    /* one decision per boot: the window's timer rolled back, then the welcome lands -> nothing */
    r = UPD_BOOT_AWAIT_WELCOME;
    assert(upd_guard_step(&r, false, UPD_WELCOME_WINDOW_US) == UPD_GUARD_ROLLBACK);
    assert(upd_guard_step(&r, true, UPD_WELCOME_WINDOW_US + 1) == UPD_GUARD_DONE);
    /* the welcome kept it, then the timer fires -> nothing (no rollback of a kept image) */
    r = UPD_BOOT_AWAIT_WELCOME;
    assert(upd_guard_step(&r, true, UPD_WELCOME_WINDOW_US - 1) == UPD_GUARD_MARK_VALID);
    assert(upd_guard_step(&r, true, UPD_WELCOME_WINDOW_US) == UPD_GUARD_DONE);
    assert(upd_guard_step(&r, false, UPD_WELCOME_WINDOW_US * 2) == UPD_GUARD_DONE);
    /* not armed: valid at once (once), welcome or not, never a rollback */
    r = UPD_BOOT_VALID_NOW;
    assert(upd_guard_step(&r, false, UPD_WELCOME_WINDOW_US * 10) == UPD_GUARD_MARK_VALID);
    assert(upd_guard_step(&r, true, 0) == UPD_GUARD_DONE);
    r = UPD_BOOT_NOTHING;
    assert(upd_guard_step(&r, false, UPD_WELCOME_WINDOW_US * 10) == UPD_GUARD_DONE);
    assert(upd_guard_step(&r, true, 0) == UPD_GUARD_DONE);
    /* armed only when the installing image was itself linked */
    assert(upd_should_arm(true) && !upd_should_arm(false));
    /* the window fits inside the relay's 5 min after the accept */
    assert(UPD_WELCOME_WINDOW_US < 300LL * 1000000);
}

int main(void) {
    /* the confirm question's fixed colours: update green, later neutral (the relay's palette) */
    assert(UPD_COLOR_UPDATE == 0x3ccf96 && UPD_COLOR_LATER == 0xf1f3f5);
    test_parse();
    test_progress();
    test_session();
    test_download();
    test_boot();
    puts("UPDATE PASS: args (https + baked repo + version + board, sha256 hex, size ≤ slot, job/timeout), confirm flow (version label ≤ 8 / version_too_long, update/later/timeout/cancelled → exact events), busy, console path (no events), progress ≤ 1 per 10 % and 1/s, download order (board tag, size, sha256 before set_boot; abort on every failure), mark-valid rule");
}
