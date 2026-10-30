/*
 * SPDX-License-Identifier: MIT
 *
 * update_core.h — the device side of "update Molty's firmware" (docs/AGENT-LINK.md,
 * "Firmware updates"), as plain C + cJSON: no FreeRTOS, no ESP-IDF. self_update.c wires
 * it to the link, the screen, esp_http_client and esp_ota_*; test/host/update_test.c
 * checks every rule here.
 *
 *   target    what to install: {version, url, sha256, size} (+ {job, timeout_ms} from the
 *             relay's `cmd update`). The url must be an official release asset of the repo
 *             baked into the firmware, for this board:
 *               https://github.com/<repo>/releases/download/<tag>/molty-<version>-<board>-ota.bin
 *             refusals: "invalid_args" (a field's type / format), "bad_url", "wrong_board"
 *             (the asset is another board's), "too_big" (size > the OTA slot),
 *             "version_too_long" (link only: the version does not fit the question's label).
 *   session   idle → confirm (link: the question is on screen) → accepted → downloading
 *             → flashing → rebooting; declined / failed end it. The console's POST
 *             /api/update starts at accepted (the click is the consent). Busy while
 *             confirm..rebooting. Events `{"t":"evt","name":"update","data":{job,state,…}}`
 *             only for a relay job; downloading progress at most once per 10 % and 1 s.
 *   download  upd_download(): stream into the spare slot through upd_io_t, checking the
 *             board tag, the exact size and the sha256 of the whole image before the
 *             boot partition is ever set.
 *   boot      the mark-valid rule (upd_boot_rule / upd_guard_step), see below.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

#define UPD_JOB_MAX       129            /* = LINK_STREAM_MAX: 1..128 chars          */
#define UPD_VERSION_MAX   32             /* 1..31 chars of [0-9A-Za-z.-]              */
#define UPD_URL_MAX       384
#define UPD_TAG_MAX       64             /* the release tag path segment              */
#define UPD_IMAGE_HEAD    (0x20 + 256 + 64)  /* = MOLTY_IMAGE_TAG_OFFSET + _BYTES: the board tag is in here */
#define UPD_TIMEOUT_MIN   1000           /* confirm question, ms (as ask)             */
#define UPD_TIMEOUT_MAX   120000
#define UPD_TIMEOUT_DEF   60000          /* the relay always sends one; the default is for completeness */
#define UPD_LABEL_MAX     8              /* = LINK_ASK_LABEL_MAX / FLUID_ASK_LABEL_MAX */
#define UPD_LABEL_LATER   "later"        /* option 1 (right half); option 0 (left half) is
                                            the version itself, upd_confirm_label        */
#define UPD_COLOR_UPDATE  0x3ccf96       /* its colour: green (relay COLORS.green)    */
#define UPD_COLOR_LATER   0xf1f3f5       /* neutral white (relay COLORS.white)        */

typedef struct {
    char job[UPD_JOB_MAX];               /* "" for a console update: no events          */
    char version[UPD_VERSION_MAX];
    char url[UPD_URL_MAX];
    uint8_t sha256[32];
    uint32_t size;
    unsigned timeout_ms;
} upd_target_t;

typedef struct {
    const char *repo;                    /* "owner/name" (CONFIG_MOLTY_RELEASES_REPO)  */
    const char *board;                   /* board.json "name" (molty_board_id())        */
    uint32_t slot_size;                  /* the spare OTA partition's size              */
} upd_policy_t;

/* NULL: *t filled. Else the refusal code (static string). link: job + timeout_ms are
 * read (job required); otherwise they are ignored and job stays "". */
const char *upd_parse(const cJSON *args, bool link, const upd_policy_t *p, upd_target_t *t);
/* The confirm question's "update" option names the version it installs (the screen has
 * no room for a header): "v<version>" when that fits UPD_LABEL_MAX chars, else the bare
 * version; false when even that does not fit (upd_parse refuses the link's cmd with
 * "version_too_long": consent is never asked without showing the version). */
bool upd_confirm_label(const char *version, char out[UPD_LABEL_MAX + 1]);
/* exactly 64 hex digits (either case) → 32 bytes */
bool upd_hex32(const char *hex, uint8_t out[32]);

/* ── progress throttle ───────────────────────────────────────────────────── */
typedef struct { int next; int64_t last_us; bool any; } upd_progress_t;
/* true when a progress report is due: the download crossed the next 10 % step and at
 * least 1 s passed since the last report (the first one needs only the 10 %). *pct =
 * the 10 % step reached (10..100). */
bool upd_progress_due(upd_progress_t *p, uint32_t done, uint32_t total, int64_t now_us, int *pct);

/* ── session ─────────────────────────────────────────────────────────────── */
typedef enum {
    UPD_IDLE, UPD_CONFIRM, UPD_ACCEPTED, UPD_DOWNLOADING, UPD_FLASHING, UPD_REBOOTING, UPD_FAILED,
} upd_state_t;

typedef struct {
    upd_state_t state;
    upd_target_t t;
    double progress;                     /* 0..1 while downloading (exact, for GET)   */
    char error[32];                      /* the last failure (state failed)           */
    upd_progress_t prog;
} upd_session_t;

const char *upd_state_name(upd_state_t s);
bool upd_busy(const upd_session_t *s);
/* A new update: "busy" while one runs, else upd_parse's verdict. OK → confirm (link) or
 * accepted (console, the click is the consent). */
const char *upd_request(upd_session_t *s, const cJSON *args, bool link, const upd_policy_t *p);
/* The confirm question ended: index 0 ("update") → accepted, returns true (start the
 * download); index 1 ("later") → declined; index < 0 → declined with reason ("timeout" |
 * "cancelled"). *evt = the event to send (NULL: none). Ignored outside confirm. */
bool upd_answer(upd_session_t *s, int index, const char *reason, cJSON **evt);
/* A step of the download: downloading (progress 0..1, throttled: *evt only when due),
 * flashing, rebooting, failed (error). *evt NULL when no event is due / no relay job. */
void upd_step(upd_session_t *s, upd_state_t st, uint32_t done, uint32_t total, int64_t now_us,
              const char *error, cJSON **evt);
/* {"t":"evt","name":"update","data":{"job","state",progress?|reason?|error?}};
 * progress < 0: none. */
cJSON *upd_evt_frame(const char *job, const char *state, double progress, const char *reason, const char *error);
/* GET /api/update: {"ok":true,"state","version"?,"progress"?,"error"?} */
cJSON *upd_status_json(const upd_session_t *s);

/* ── download ────────────────────────────────────────────────────────────── */
/* The device's I/O (esp_http_client + esp_ota_* + sha256 on the device; fakes on the
 * host). Errors are short ASCII codes, reported as the `failed` event's error. */
typedef struct {
    void *ctx;
    /* GET the url following redirects over verified TLS; *len = Content-Length or -1 */
    const char *(*open)(void *ctx, const char *url, int64_t *len);
    int (*read)(void *ctx, uint8_t *buf, int cap);             /* >0 bytes, 0 end, <0 error */
    void (*close)(void *ctx);
    const char *(*ota_begin)(void *ctx);
    const char *(*ota_write)(void *ctx, const uint8_t *buf, size_t n);
    const char *(*ota_end)(void *ctx);                          /* the image's own checks */
    const char *(*ota_set_boot)(void *ctx);
    void (*ota_abort)(void *ctx);
    void (*sha_start)(void *ctx);
    void (*sha_update)(void *ctx, const uint8_t *buf, size_t n);
    void (*sha_finish)(void *ctx, uint8_t out[32]);
    /* molty_board_image_check: 1 this board, 0 another, -1 not an app image */
    int (*board_check)(void *ctx, const uint8_t *head, size_t len);
    /* a step: downloading (done of total) or flashing */
    void (*step)(void *ctx, upd_state_t st, uint32_t done, uint32_t total);
} upd_io_t;
/* NULL: the image is written, verified and set to boot (send `rebooting`, restart).
 * Else the failure: "http_<code>", "tls", "connect", "read", "size_mismatch",
 * "wrong_board", "sha256_mismatch", "write", "verify", "set_boot", "begin". The slot is
 * aborted on every failure; the boot partition is set only after every check passed. */
const char *upd_download(const upd_target_t *t, const upd_io_t *io, uint8_t *buf, size_t cap);

/* ── the mark-valid rule ─────────────────────────────────────────────────── */
/*
 * A new image boots PENDING_VERIFY (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE); a reset
 * before it is marked valid boots the previous one again.
 *   armed     the installing firmware wrote NVS "selfupd"/"armed" = the new slot's label
 *             right before setting it to boot, when its own link was welcomed then (it
 *             reached the relay, so the new image must too). Read and erased once at boot;
 *             counts only for that slot.
 *   rule      PENDING_VERIFY + armed: valid on the link's first `welcome`; no welcome
 *             within UPD_WELCOME_WINDOW_US of boot → restart → the bootloader rolls back.
 *             PENDING_VERIFY, not armed (POST /api/ota, an update while the device had
 *             no relay link — no relay configured, no Wi-Fi, auth failing): valid once
 *             the HTTP server is up, as before, so such devices are never rolled back
 *             forever for lack of a relay. Not pending: nothing to do.
 */
#define UPD_WELCOME_WINDOW_US (180LL * 1000000)   /* 3 min: Wi-Fi + TLS + backoff, inside the relay's 5 min */
typedef enum { UPD_BOOT_NOTHING, UPD_BOOT_VALID_NOW, UPD_BOOT_AWAIT_WELCOME } upd_boot_t;
upd_boot_t upd_boot_rule(bool pending_verify, bool armed);
typedef enum { UPD_GUARD_WAIT, UPD_GUARD_MARK_VALID, UPD_GUARD_ROLLBACK, UPD_GUARD_DONE } upd_guard_t;
/*
 * What to do now under rule *r: welcomed = the link got its welcome; since_boot_us.
 * A decision (MARK_VALID / ROLLBACK) is returned exactly once: it consumes the rule
 * (*r becomes UPD_BOOT_NOTHING), so racing callers (the welcome vs the window's timer)
 * that call this under one lock get one decision and the loser gets DONE. The caller
 * performs the decision's side effect without dropping that claim (self_update.c).
 */
upd_guard_t upd_guard_step(upd_boot_t *r, bool welcomed, int64_t since_boot_us);
/* Arm only when the link is up at install time. */
bool upd_should_arm(bool link_welcomed);
