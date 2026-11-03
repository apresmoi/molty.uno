/*
 * SPDX-License-Identifier: MIT
 *
 * pkg_store.h: installed packages on flash (docs/PACKAGES.md sections 2 and 3).
 *
 * Plain C + cJSON + stdio, no FreeRTOS / ESP-IDF: pkg_cmd.c wires it to the link, the
 * ask screen, esp_http_client and mbedtls (root "/spiffs/pkg"); test/host/pkg_test.c runs
 * it against a temporary directory and a fake download.
 *
 *   request   pkg.install {id, url, sha256, size, name, job?, timeout_ms?}. The url must start
 *             with PKG_URL_PREFIX (the only host the firmware names), use only [A-Za-z0-9._/-]
 *             after it, no "..", and end in ".json". Refusals: "invalid_args", "bad_url",
 *             "too_big" (size > PKG_FILE_MAX).
 *   install   pkg_fetch then pkg_store_put: download (verified TLS on the device) at most `size` bytes, check
 *             the SHA-256 and the exact size ("too_big" / "hash_mismatch"), parse it strictly
 *             ("bad_package": also when its id is not the one asked for), keep it within the
 *             quota ("no_space"), write it, then the index. Nothing is kept on any failure.
 *   files     <root>/<first 8 hex of its sha256>.json (SPIFFS names stay short), and
 *             <root>/index.json: [{id, name, version, kind, size, sha256, file}]. The index is
 *             rewritten through index.tmp; a lost index means nothing is installed. A file
 *             that vanished (a factory flash rewrites SPIFFS) is not installed.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "pkg_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The one place a host is named: packages come from the project repo, nowhere else. */
#define PKG_URL_PREFIX   "https://raw.githubusercontent.com/apresmoi/molty.uno/"
#define PKG_URL_MAX      384
#define PKG_QUOTA        (1024 * 1024)  /* bytes of packages, all together                 */
#define PKG_INSTALLED_MAX 32
#define PKG_JOB_MAX      129            /* = LINK_STREAM_MAX: 1..128 chars                 */
#define PKG_ASK_MS       60000          /* the install question, by default               */

typedef struct {
    char id[PKG_ID_MAX + 1], name[PKG_TEXT_MAX + 1], version[PKG_VERSION_MAX + 1];
    char sha256[65], file[16];
    uint8_t kind;                       /* PKG_SCENES / PKG_CREATURE                      */
    uint32_t size;
} pkg_entry_t;

typedef struct {
    char job[PKG_JOB_MAX];              /* "" = none                                       */
    char id[PKG_ID_MAX + 1], name[PKG_TEXT_MAX + 1], url[PKG_URL_MAX];
    uint8_t sha256[32];
    uint32_t size;
    unsigned timeout_ms;
} pkg_req_t;

/* NULL: *r filled; else the refusal code (static). */
const char *pkg_req_parse(const cJSON *args, pkg_req_t *r);
bool pkg_url_ok(const char *url);

/* The download (esp_http_client + mbedtls on the device, fakes on the host). */
typedef struct {
    void *ctx;
    /* GET over verified TLS; *len = Content-Length or -1. NULL or an error code. */
    const char *(*open)(void *ctx, const char *url, int64_t *len);
    int (*read)(void *ctx, uint8_t *buf, int cap);              /* >0 bytes, 0 end, <0 error */
    void (*close)(void *ctx);
    void (*sha_start)(void *ctx);
    void (*sha_update)(void *ctx, const uint8_t *buf, size_t n);
    void (*sha_finish)(void *ctx, uint8_t out[32]);
} pkg_io_t;

/* Download r's package into buf (r->size + 1 bytes), checking its size and SHA-256. NULL,
 * or "download" | "too_big" | "hash_mismatch" (open's own code for a connection failure).
 * No lock needed: nothing is written. */
const char *pkg_fetch(const pkg_req_t *r, const pkg_io_t *io, uint8_t *buf);
/* Parse and store fetched bytes (r->size of buf) into root. NULL: installed, *out its index
 * entry. Else "bad_package" | "no_space". One writer at a time (pkg_cmd.c's lock). */
const char *pkg_store_put(const char *root, const pkg_req_t *r, const uint8_t *buf, pkg_entry_t *out);
/* "no_space" when size more bytes (replacing id's) would pass the quota; else NULL. */
const char *pkg_store_room(const char *root, const char *id, uint32_t size);
/* Installed packages (their files present), up to max. */
int  pkg_store_list(const char *root, pkg_entry_t *out, int max);
bool pkg_store_find(const char *root, const char *id, pkg_entry_t *out);
/* NULL or "not_installed". */
const char *pkg_store_remove(const char *root, const char *id);
/* The file's bytes (malloc / PSRAM; free with pkg_store_free), NULL when not installed. */
char *pkg_store_read(const char *root, const char *id, size_t *len);
void  pkg_store_free(char *buf);
/* {id, name, version, kind, size} */
cJSON *pkg_entry_json(const pkg_entry_t *e);
bool pkg_hex32(const char *hex, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
