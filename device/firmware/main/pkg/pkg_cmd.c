/*
 * SPDX-License-Identifier: MIT
 *
 * pkg_cmd.c: packages and scenes on the link and the local API (pkg_cmd.h).
 */
#include "pkg_cmd.h"
#include "pkg_store.h"
#include "pkg_creature.h"
#include "creature_player.h"
#include "scene_player.h"
#include "link_ask.h"
#include "link_cmd.h"
#include "link_client.h"
#include "mole_creature.h"
#include "mole_fluid.h"
#include "mole_diag.h"
#include "molty_board.h"
#include "http.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_spiffs.h"
#include "esp_tls_errors.h"
#include "mbedtls/sha256.h"

#define DL_STACK  8192                 /* the TLS handshake runs on this task */
#define SPARE     16384                /* SPIFFS bytes left free beside a package */
#define COLOR_YES 0x3ccf96             /* green, as the self-update's question */
#define COLOR_NO  0xf1f3f5

/* One writer of the store at a time, and the parsed-package cache. Created on first use. */
static _Atomic(SemaphoreHandle_t) s_lock_h;
static pkg_t *s_cache;                 /* the last package a scene came from (one reference) */
static atomic_bool s_installing;       /* from the question until the install ends */
static pkg_req_t s_req;                /* that install (link task, then its download task) */

static void lock(void)
{
    SemaphoreHandle_t m = atomic_load(&s_lock_h);
    if (!m) {
        SemaphoreHandle_t fresh = xSemaphoreCreateMutex(), none = NULL;
        configASSERT(fresh);
        if (atomic_compare_exchange_strong(&s_lock_h, &none, fresh)) m = fresh;
        else { vSemaphoreDelete(fresh); m = none; }
    }
    xSemaphoreTake(m, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(atomic_load(&s_lock_h)); }

static void drop_cache(const char *id)
{
    lock();
    if (s_cache && !strcmp(s_cache->id, id)) { pkg_unref(s_cache); s_cache = NULL; }
    unlock();
}

static esp_err_t refuse(const char *e) { return !strcmp(e, "invalid_args") ? ESP_ERR_INVALID_ARG : link_cmd_refuse(e); }

/* {"t":"evt","name":"pkg","data":{job?, id, state, <key>: <val>}} while the link is up */
static void send_evt(const char *state, const char *key, const char *val)
{
    cJSON *j = cJSON_CreateObject(), *d = cJSON_CreateObject();
    if (!j || !d) { cJSON_Delete(j); cJSON_Delete(d); return; }
    cJSON_AddStringToObject(j, "t", "evt"); cJSON_AddStringToObject(j, "name", "pkg");
    if (s_req.job[0]) cJSON_AddStringToObject(d, "job", s_req.job);
    cJSON_AddStringToObject(d, "id", s_req.id); cJSON_AddStringToObject(d, "state", state);
    if (key) cJSON_AddStringToObject(d, key, val);
    cJSON_AddItemToObject(j, "data", d);
    if (link_client_connected()) link_send_json(j);
    cJSON_Delete(j);
    mole_diag_event(strcmp(state, "installed") != 0, "pkg.%s %s %s", state, s_req.id, val ? val : "");
}

/* ── the download: esp_http_client over verified TLS, mbedtls SHA-256 ──────── */

typedef struct { esp_http_client_handle_t http; mbedtls_sha256_context sha; char err[16]; } dl_t;

static const char *io_open(void *ctx, const char *url, int64_t *len)
{
    dl_t *d = ctx;
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .user_agent = "molty-firmware",
        .keep_alive_enable = false,
        .disable_auto_redirect = true,   /* the bytes come from the allowlisted URL, or not at all */
    };
    if (!(d->http = esp_http_client_init(&cfg))) return "oom";
    if (esp_http_client_open(d->http, 0) != ESP_OK) {
        int code = 0, flags = 0;
        esp_http_client_get_and_clear_last_tls_error(d->http, &code, &flags);
        return flags || code >= ESP_ERR_MBEDTLS_CERT_PARTLY_OK ? "tls" : "download";
    }
    if (esp_http_client_fetch_headers(d->http) < 0) return "download";
    int st = esp_http_client_get_status_code(d->http);
    if (st != 200) { snprintf(d->err, sizeof(d->err), "http_%d", st); return d->err; }
    int64_t cl = esp_http_client_is_chunked_response(d->http) ? -1 : esp_http_client_get_content_length(d->http);
    *len = cl > 0 ? cl : -1;
    return NULL;
}
static int io_read(void *ctx, uint8_t *buf, int cap) { return esp_http_client_read(((dl_t *)ctx)->http, (char *)buf, cap); }
static void io_close(void *ctx)
{
    dl_t *d = ctx;
    if (!d->http) return;
    esp_http_client_close(d->http);
    esp_http_client_cleanup(d->http);
    d->http = NULL;
}
static void io_sha_start(void *ctx) { dl_t *d = ctx; mbedtls_sha256_init(&d->sha); mbedtls_sha256_starts(&d->sha, 0); }
static void io_sha_update(void *ctx, const uint8_t *b, size_t n) { mbedtls_sha256_update(&((dl_t *)ctx)->sha, b, n); }
static void io_sha_finish(void *ctx, uint8_t out[32]) { dl_t *d = ctx; mbedtls_sha256_finish(&d->sha, out); mbedtls_sha256_free(&d->sha); }

static void download_task(void *arg)
{
    (void)arg;
    static dl_t d;
    const pkg_io_t io = { &d, io_open, io_read, io_close, io_sha_start, io_sha_update, io_sha_finish };
    uint8_t *buf = heap_caps_malloc(s_req.size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pkg_entry_t ent;
    const char *e = buf ? pkg_fetch(&s_req, &io, buf) : "oom";
    if (!e) {
        lock();
        e = pkg_store_put(PKG_ROOT, &s_req, buf, &ent);
        unlock();
    }
    heap_caps_free(buf);
    drop_cache(s_req.id);
    if (e) send_evt("failed", "error", e);
    else send_evt("installed", "version", ent.version);
    atomic_store(&s_installing, false);
    vTaskDelete(NULL);
}

/* the question ended (link task): yes downloads, anything else declines */
static void on_answer(int index, const char *reason)
{
    if (index == 0 && xTaskCreate(download_task, "pkg_install", DL_STACK, NULL, 1, NULL) == pdPASS) return;
    if (index == 0) send_evt("failed", "error", "oom");
    else send_evt("declined", "reason", index == 1 ? "declined" : reason);
    atomic_store(&s_installing, false);
}

/* SPIFFS itself must hold it too (the console page, the creature and the log share it) */
static const char *room(const pkg_req_t *r)
{
    size_t total = 0, used = 0;
    if (esp_spiffs_info("spiffs", &total, &used) != ESP_OK || used + r->size + SPARE > total) return "no_space";
    return pkg_store_room(PKG_ROOT, r->id, r->size);
}

esp_err_t pkg_cmd_install(const cJSON *args, cJSON **out)
{
    if (!molty_board_has(MOLTY_CAP_TOUCH)) return ESP_ERR_NOT_SUPPORTED;   /* nobody could say yes */
    pkg_req_t r;
    const char *e = pkg_req_parse(args, &r);
    if (!e) e = room(&r);
    bool idle = false;
    if (!e && !atomic_compare_exchange_strong(&s_installing, &idle, true)) e = "busy";
    if (e) { mole_diag_event(true, "pkg.refused %s", e); return refuse(e); }
    s_req = r;
    /* the question names what is installed, in the ask's 8 letters */
    char label[9];
    snprintf(label, sizeof(label), "%.8s", r.name);
    for (int n = (int)strlen(label); n > 0 && label[n - 1] == ' '; n--) label[n - 1] = 0;
    if (!label[0]) snprintf(label, sizeof(label), "%.8s", r.id);
    const char *labels[2] = { label, "no" };
    const int32_t colors[2] = { COLOR_YES, COLOR_NO };
    cJSON *j = cJSON_CreateObject();
    esp_err_t err = j ? link_ask_open(r.job[0] ? r.job : r.id, labels, colors, 2, r.timeout_ms, on_answer) : ESP_ERR_NO_MEM;
    if (err != ESP_OK) { atomic_store(&s_installing, false); cJSON_Delete(j); return err; }
    if (r.job[0]) cJSON_AddStringToObject(j, "job", r.job);
    cJSON_AddStringToObject(j, "id", r.id);
    *out = j;
    return ESP_OK;
}

static const char *id_arg(const cJSON *args)
{
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "id"));
    return pkg_id_ok(id) ? id : NULL;
}

static const char *remove_pkg(const char *id)
{
    lock();
    const char *e = pkg_store_remove(PKG_ROOT, id);
    unlock();
    drop_cache(id);
    return e;
}

esp_err_t pkg_cmd_remove(const cJSON *args, cJSON **out)
{
    const char *id = id_arg(args);
    if (!id) return ESP_ERR_INVALID_ARG;
    const char *e = remove_pkg(id);
    if (e) return refuse(e);
    if (!(*out = cJSON_CreateObject())) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(*out, "removed", id);
    return ESP_OK;
}

static cJSON *list_json(void)
{
    pkg_entry_t *e = heap_caps_malloc(sizeof(pkg_entry_t) * PKG_INSTALLED_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    cJSON *j = cJSON_CreateObject(), *a = cJSON_AddArrayToObject(j, "packages");
    int n = e ? pkg_store_list(PKG_ROOT, e, PKG_INSTALLED_MAX) : 0;
    for (int i = 0; a && i < n; i++) cJSON_AddItemToArray(a, pkg_entry_json(&e[i]));
    heap_caps_free(e);
    return j;
}

esp_err_t pkg_cmd_list(const cJSON *args, cJSON **out)
{
    (void)args;
    return (*out = list_json()) ? ESP_OK : ESP_ERR_NO_MEM;
}

/* ── scenes ────────────────────────────────────────────────────────────────── */

/* the installed package, parsed (one reference for the caller): the cache, else the file */
static const char *load(const char *id, pkg_t **out)
{
    lock();
    if (!(s_cache && !strcmp(s_cache->id, id) && pkg_store_find(PKG_ROOT, id, NULL))) {
        size_t len = 0;
        char *buf = pkg_store_read(PKG_ROOT, id, &len);
        const bool stored = buf != NULL;
        const char *err;
        pkg_t *p = stored ? pkg_parse(buf, len, &err) : NULL;
        pkg_store_free(buf);
        if (!p) { unlock(); return stored ? "bad_package" : "not_installed"; }
        pkg_unref(s_cache);
        s_cache = p;
    }
    pkg_ref(s_cache);
    *out = s_cache;
    unlock();
    return NULL;
}

static const char *scene_start(const cJSON *args, mole_scene_op_t op, cJSON **out)
{
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "pkg"));
    const char *layout = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "layout"));
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(args, "params"), *ttl = cJSON_GetObjectItemCaseSensitive(args, "ttl_s");
    if (!pkg_id_ok(id) || !layout || !layout[0] || strlen(layout) > PKG_KEY_MAX || (params && !cJSON_IsObject(params)) ||
        (ttl && (!cJSON_IsNumber(ttl) || ttl->valuedouble < 1 || ttl->valuedouble > SCENE_TTL_MAX)))
        return "invalid_args";
    pkg_t *p;
    const char *e = load(id, &p);
    if (e) return e;
    scene_t *sc = scene_build(p, layout, params, ttl ? (float)ttl->valuedouble : 0.f, &e);
    pkg_unref(p);
    if (!sc) return e;
    const float ttl_s = sc->ttl;
    if (mole_creature_select("fluid") != ESP_OK || mole_fluid_scene(op, sc) != ESP_OK) { scene_free(sc); return "busy"; }
    if (op == MOLE_SCENE_PLAY) link_ask_cancel();   /* it took the screen (a queued one waits its turn) */
    cJSON *j = cJSON_CreateObject();
    if (!j) return "oom";
    cJSON_AddStringToObject(j, "pkg", id); cJSON_AddStringToObject(j, "layout", layout);
    cJSON_AddNumberToObject(j, "ttl_s", ttl_s);
    *out = j;
    return NULL;
}

static esp_err_t scene_cmd(const cJSON *args, cJSON **out, mole_scene_op_t op)
{
    const char *e = scene_start(args, op, out);
    return !e ? ESP_OK : !strcmp(e, "oom") ? ESP_ERR_NO_MEM : refuse(e);
}
esp_err_t pkg_cmd_scene_play(const cJSON *args, cJSON **out) { return scene_cmd(args, out, MOLE_SCENE_PLAY); }
esp_err_t pkg_cmd_scene_queue(const cJSON *args, cJSON **out) { return scene_cmd(args, out, MOLE_SCENE_QUEUE); }

esp_err_t pkg_cmd_scene_clear(const cJSON *args, cJSON **out)
{
    (void)args; (void)out;
    esp_err_t e = mole_creature_select("fluid");
    return e != ESP_OK ? e : mole_fluid_scene(MOLE_SCENE_CLEAR, NULL);
}

/* ── creatures: the built-in and installed creature packages (pkg_creature.h) ── */

/* {current, creatures: [{id, name, version, source}]} */
static cJSON *creature_list_json(void)
{
    pkgc_entry_t *e = heap_caps_malloc(sizeof(pkgc_entry_t) * (PKG_INSTALLED_MAX + 8), MALLOC_CAP_SPIRAM);
    cJSON *j = cJSON_CreateObject(), *a = cJSON_CreateArray();
    if (!e || !j || !a) { heap_caps_free(e); cJSON_Delete(j); cJSON_Delete(a); return NULL; }
    char cur[PKG_ID_MAX + 1];
    mole_fluid_creature_id(cur, sizeof(cur));
    cJSON_AddStringToObject(j, "current", cur);
    const int n = pkgc_list(e, PKG_INSTALLED_MAX + 8);
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        if (!o) break;
        cJSON_AddStringToObject(o, "id", e[i].id); cJSON_AddStringToObject(o, "name", e[i].name);
        cJSON_AddStringToObject(o, "version", e[i].version); cJSON_AddStringToObject(o, "source", e[i].builtin ? "builtin" : "installed");
        cJSON_AddItemToArray(a, o);
    }
    cJSON_AddItemToObject(j, "creatures", a);
    heap_caps_free(e);
    return j;
}

/* creature `id` becomes the creature (checked here: the answer is the outcome) and the choice */
static const char *creature_select(const char *id, cJSON **out)
{
    if (!pkg_id_ok(id)) return "invalid_args";
    size_t len = 0;
    char *json = pkgc_bytes(id, &len);
    if (!json) return "not_installed";
    const char *err = NULL;
    cp_t *c = cp_create(json, len, &err);
    if (!c) { heap_caps_free(json); mole_diag_event(true, "creature.select %s: %s", id, err); return "bad_package"; }
    cJSON *j = cJSON_CreateObject();
    if (j) { cJSON_AddStringToObject(j, "id", id); cJSON_AddStringToObject(j, "name", cp_name(c)); }
    cp_destroy(c);
    if (!j) { heap_caps_free(json); return "oom"; }
    if (mole_creature_select("fluid") != ESP_OK || mole_fluid_creature_package(id, json, len) != ESP_OK) { cJSON_Delete(j); return "busy"; }
    pkgc_choose(id);
    *out = j;
    return NULL;
}

esp_err_t pkg_cmd_creature_select(const cJSON *args, cJSON **out)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(args, "id");
    if (!cJSON_IsObject(args) || cJSON_GetArraySize(args) != 1 || !cJSON_IsString(id)) return ESP_ERR_INVALID_ARG;
    const char *e = creature_select(id->valuestring, out);
    return e ? refuse(e) : ESP_OK;
}

esp_err_t pkg_cmd_creature_list(const cJSON *args, cJSON **out)
{
    (void)args;
    return (*out = creature_list_json()) ? ESP_OK : ESP_ERR_NO_MEM;
}

/* ── the local API (the console) ───────────────────────────────────────────── */

static esp_err_t send_err(httpd_req_t *r, const char *e)
{
    char b[64];
    snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"%s\"}", e);
    return http_send_json(r, b);
}

static esp_err_t send_obj(httpd_req_t *r, cJSON *j)
{
    if (!j) return send_err(r, "oom");
    cJSON_AddBoolToObject(j, "ok", true);
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!body) return send_err(r, "oom");
    esp_err_t e = http_send_json(r, body);
    cJSON_free(body);
    return e;
}

static esp_err_t http_list(httpd_req_t *r) { return send_obj(r, list_json()); }

static esp_err_t http_remove(httpd_req_t *r)
{
    char q[64] = "", id[PKG_ID_MAX + 2] = "";
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK || httpd_query_key_value(q, "id", id, sizeof(id)) != ESP_OK || !pkg_id_ok(id))
        return send_err(r, "invalid_args");
    const char *e = remove_pkg(id);
    if (e) return send_err(r, e);
    cJSON *j = cJSON_CreateObject();
    if (j) cJSON_AddStringToObject(j, "removed", id);
    return send_obj(r, j);
}

static esp_err_t http_scene(httpd_req_t *r)
{
    if (r->content_len <= 0 || r->content_len > 16384) return send_err(r, "invalid_args");
    char *body = heap_caps_malloc((size_t)r->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) return send_err(r, "oom");
    int got = http_recv_full(r, body, r->content_len);
    body[got > 0 ? got : 0] = 0;
    cJSON *args = got == r->content_len ? cJSON_Parse(body) : NULL;
    heap_caps_free(body);
    cJSON *out = NULL;
    const char *e = cJSON_IsObject(args) ? scene_start(args, MOLE_SCENE_PLAY, &out) : "invalid_args";
    cJSON_Delete(args);
    return e ? send_err(r, e) : send_obj(r, out);
}

static esp_err_t http_creature_list(httpd_req_t *r) { return send_obj(r, creature_list_json()); }

static esp_err_t http_creature_select(httpd_req_t *r)
{
    char q[64] = "", id[PKG_ID_MAX + 2] = "";
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK || httpd_query_key_value(q, "id", id, sizeof(id)) != ESP_OK)
        return send_err(r, "invalid_args");
    cJSON *out = NULL;
    const char *e = creature_select(id, &out);
    return e ? send_err(r, e) : send_obj(r, out);
}

esp_err_t pkg_http_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/api/pkg",   .method = HTTP_GET,    .handler = http_list},
        {.uri = "/api/pkg",   .method = HTTP_DELETE, .handler = http_remove},
        {.uri = "/api/scene", .method = HTTP_POST,   .handler = http_scene},
        {.uri = "/api/creature/list",   .method = HTTP_GET,  .handler = http_creature_list},
        {.uri = "/api/creature/select", .method = HTTP_POST, .handler = http_creature_select},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t e = httpd_register_uri_handler(server, &routes[i]);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}
