/*
 * SPDX-License-Identifier: MIT
 *
 * pkg_store.c — installed packages on flash (pkg_store.h).
 * Plain C: built into the firmware and by test/host/pkg_test.c.
 */
#include "pkg_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define STORE_MALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define STORE_FREE(p)   heap_caps_free(p)
#else
#define STORE_MALLOC(n) malloc(n)
#define STORE_FREE(p)   free(p)
#endif

static const char *const KINDS[] = { "scenes", "creature" };

/* ── the request ───────────────────────────────────────────────────────────── */

bool pkg_hex32(const char *hex, uint8_t out[32]) {
    if (!hex || strlen(hex) != 64) return false;
    for (int i = 0; i < 64; i++) {
        char c = hex[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) return false;
        out[i / 2] = (uint8_t)(i & 1 ? out[i / 2] | v : v << 4);
    }
    return true;
}

bool pkg_url_ok(const char *url) {
    const size_t pre = strlen(PKG_URL_PREFIX), n = url ? strlen(url) : 0;
    if (n <= pre + 5 || n >= PKG_URL_MAX || strncmp(url, PKG_URL_PREFIX, pre) || strcmp(url + n - 5, ".json")) return false;
    for (const char *c = url + pre; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '.' || *c == '_' || *c == '/' || *c == '-'))
            return false;
    return !strstr(url + pre, "..") && !strstr(url + pre, "//") && url[pre] != '/';
}

const char *pkg_req_parse(const cJSON *args, pkg_req_t *r) {
    memset(r, 0, sizeof(*r));
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(args, "id"), *url = cJSON_GetObjectItemCaseSensitive(args, "url");
    const cJSON *sha = cJSON_GetObjectItemCaseSensitive(args, "sha256"), *size = cJSON_GetObjectItemCaseSensitive(args, "size");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(args, "name"), *job = cJSON_GetObjectItemCaseSensitive(args, "job");
    const cJSON *to = cJSON_GetObjectItemCaseSensitive(args, "timeout_ms");
    if (!cJSON_IsString(id) || !pkg_id_ok(id->valuestring) || !cJSON_IsString(url) || !pkg_hex32(cJSON_GetStringValue(sha), r->sha256) ||
        !cJSON_IsString(name) || !pkg_text_ok(name->valuestring, PKG_TEXT_MAX) ||
        !cJSON_IsNumber(size) || size->valuedouble < 1 || size->valuedouble > 1e9 || size->valuedouble != (double)(uint32_t)size->valuedouble ||
        (job && (!cJSON_IsString(job) || !job->valuestring[0] || strlen(job->valuestring) >= PKG_JOB_MAX)) ||
        (to && (!cJSON_IsNumber(to) || to->valuedouble < 1000 || to->valuedouble > 120000)))
        return "invalid_args";
    if (!pkg_url_ok(url->valuestring)) return "bad_url";
    r->size = (uint32_t)size->valuedouble;
    if (r->size > PKG_FILE_MAX) return "too_big";
    strcpy(r->id, id->valuestring); strcpy(r->name, name->valuestring); strcpy(r->url, url->valuestring);
    if (job) strcpy(r->job, job->valuestring);
    r->timeout_ms = to ? (unsigned)to->valuedouble : PKG_ASK_MS;
    return NULL;
}

/* ── the index ─────────────────────────────────────────────────────────────── */

static void path(char *out, size_t n, const char *root, const char *name) { snprintf(out, n, "%s/%s", root, name); }

static bool exists(const char *root, const char *name) {
    char p[96]; struct stat st;
    path(p, sizeof(p), root, name);
    return stat(p, &st) == 0;
}

static char *slurp(const char *p, size_t max, size_t *len) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    char *buf = STORE_MALLOC(max + 1);
    size_t n = buf ? fread(buf, 1, max + 1, f) : 0;
    fclose(f);
    if (!buf || n > max) { STORE_FREE(buf); return NULL; }
    buf[n] = 0;
    *len = n;
    return buf;
}

static bool entry_from(const cJSON *j, pkg_entry_t *e) {
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "id")), *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "name"));
    const char *ver = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "version")), *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "kind"));
    const char *sha = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "sha256")), *file = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "file"));
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(j, "size");
    uint8_t tmp[32];
    if (!pkg_id_ok(id) || !pkg_text_ok(name, PKG_TEXT_MAX) || !pkg_text_ok(ver, PKG_VERSION_MAX) || !kind || !pkg_hex32(sha, tmp) ||
        !pkg_text_ok(file, sizeof(e->file) - 1) || strchr(file, '/') || !cJSON_IsNumber(size) || size->valuedouble < 1 || size->valuedouble > PKG_FILE_MAX)
        return false;
    memset(e, 0, sizeof(*e));
    strcpy(e->id, id); strcpy(e->name, name); strcpy(e->version, ver); strcpy(e->sha256, sha); strcpy(e->file, file);
    e->kind = !strcmp(kind, KINDS[PKG_CREATURE]) ? PKG_CREATURE : PKG_SCENES;
    e->size = (uint32_t)size->valuedouble;
    return true;
}

/* every entry of the index (files present or not); a lost index (or index.tmp left by a
 * reset mid-write) is read back */
static int load(const char *root, pkg_entry_t *out, int max) {
    char p[96]; size_t len = 0;
    path(p, sizeof(p), root, "index.json");
    char *buf = slurp(p, 16384, &len);
    if (!buf) { path(p, sizeof(p), root, "index.tmp"); buf = slurp(p, 16384, &len); }
    cJSON *j = buf ? cJSON_ParseWithLength(buf, len) : NULL;
    STORE_FREE(buf);
    int n = 0;
    for (const cJSON *c = cJSON_IsArray(j) ? j->child : NULL; c && n < max; c = c->next) if (entry_from(c, &out[n])) n++;
    cJSON_Delete(j);
    return n;
}

cJSON *pkg_entry_json(const pkg_entry_t *e) {
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
    cJSON_AddStringToObject(j, "id", e->id); cJSON_AddStringToObject(j, "name", e->name);
    cJSON_AddStringToObject(j, "version", e->version); cJSON_AddStringToObject(j, "kind", KINDS[e->kind]);
    cJSON_AddNumberToObject(j, "size", e->size);
    return j;
}

static bool save(const char *root, const pkg_entry_t *e, int n) {
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; a && i < n; i++) {
        cJSON *j = pkg_entry_json(&e[i]);
        if (!j) break;
        cJSON_AddStringToObject(j, "sha256", e[i].sha256); cJSON_AddStringToObject(j, "file", e[i].file);
        cJSON_AddItemToArray(a, j);
    }
    char *txt = a && cJSON_GetArraySize(a) == n ? cJSON_PrintUnformatted(a) : NULL;
    cJSON_Delete(a);
    if (!txt) return false;
    char tmp[96], dst[96];
    path(tmp, sizeof(tmp), root, "index.tmp"); path(dst, sizeof(dst), root, "index.json");
    FILE *f = fopen(tmp, "wb");
    size_t len = strlen(txt);
    bool ok = f && fwrite(txt, 1, len, f) == len;
    if (f && fclose(f) != 0) ok = false;
    cJSON_free(txt);
    if (!ok) { remove(tmp); return false; }
    remove(dst);                                   /* SPIFFS cannot rename over a file */
    return rename(tmp, dst) == 0;
}

static int find(const pkg_entry_t *e, int n, const char *id) {
    for (int i = 0; i < n; i++) if (!strcmp(e[i].id, id)) return i;
    return -1;
}

/* drop entry i's file unless another entry shares it (the same bytes under two ids) */
static void drop_file(const char *root, const pkg_entry_t *e, int n, int i) {
    for (int q = 0; q < n; q++) if (q != i && !strcmp(e[q].file, e[i].file)) return;
    char p[96];
    path(p, sizeof(p), root, e[i].file);
    remove(p);
}

/* ── install / remove / list ───────────────────────────────────────────────── */

/* the index's entries, on the heap (link, httpd and download stacks stay small) */
static pkg_entry_t *entries(void) { return STORE_MALLOC(sizeof(pkg_entry_t) * (PKG_INSTALLED_MAX + 1)); }

const char *pkg_store_room(const char *root, const char *id, uint32_t size) {
    pkg_entry_t *e = entries();
    if (!e) return "no_space";
    int n = load(root, e, PKG_INSTALLED_MAX);
    uint32_t used = 0;
    for (int i = 0; i < n; i++) if (strcmp(e[i].id, id)) used += e[i].size;
    const bool full = find(e, n, id) < 0 && n >= PKG_INSTALLED_MAX;
    STORE_FREE(e);
    return full || used + size > PKG_QUOTA ? "no_space" : NULL;
}

/* at most r->size bytes into buf, hashed as they come */
static const char *fetch(const pkg_req_t *r, const pkg_io_t *io, uint8_t *buf, uint8_t digest[32]) {
    int64_t cl = -1;
    const char *e = io->open(io->ctx, r->url, &cl);
    if (e) { io->close(io->ctx); return e; }
    if (cl > (int64_t)r->size) { io->close(io->ctx); return "too_big"; }
    io->sha_start(io->ctx);
    uint32_t got = 0;
    for (;;) {
        int n = io->read(io->ctx, buf + got, (int)(r->size + 1 - got));   /* one byte past: a longer file shows */
        if (n < 0) { e = "download"; break; }
        if (n == 0) break;
        io->sha_update(io->ctx, buf + got, (size_t)n);
        if ((got += (uint32_t)n) > r->size) { e = "too_big"; break; }
    }
    uint8_t d[32];
    io->sha_finish(io->ctx, d);
    io->close(io->ctx);
    if (e) return e;
    if (got != r->size || memcmp(d, r->sha256, 32)) return "hash_mismatch";
    memcpy(digest, d, 32);
    return NULL;
}

const char *pkg_install(const char *root, const pkg_req_t *r, const pkg_io_t *io, uint8_t *buf, pkg_entry_t *out) {
    uint8_t d[32];
    const char *e = fetch(r, io, buf, d);
    if (e) return e;
    pkg_t *p = pkg_parse((const char *)buf, r->size, &e);
    if (!p) return "bad_package";
    pkg_entry_t ent = { .kind = p->kind, .size = r->size };
    bool same = !strcmp(p->id, r->id);
    strcpy(ent.id, p->id); strcpy(ent.name, p->name); strcpy(ent.version, p->version);
    pkg_unref(p);
    if (!same) return "bad_package";
    for (int i = 0; i < 32; i++) snprintf(ent.sha256 + 2 * i, 3, "%02x", d[i]);
    snprintf(ent.file, sizeof(ent.file), "%.8s.json", ent.sha256);
    if ((e = pkg_store_room(root, r->id, r->size))) return e;

    char fp[96];
    path(fp, sizeof(fp), root, ent.file);
    FILE *f = fopen(fp, "wb");
    bool ok = f && fwrite(buf, 1, r->size, f) == r->size;
    if (f && fclose(f) != 0) ok = false;
    pkg_entry_t *all = ok ? entries() : NULL;
    int n = all ? load(root, all, PKG_INSTALLED_MAX) : 0, i = all ? find(all, n, r->id) : -1;
    pkg_entry_t old = i >= 0 ? all[i] : (pkg_entry_t){ 0 };
    if (all) {
        if (i < 0) i = n++;
        all[i] = ent;
        ok = save(root, all, n);
        if (ok && old.file[0] && strcmp(old.file, ent.file)) { all[i] = old; drop_file(root, all, n, i); }
    } else ok = false;
    STORE_FREE(all);
    if (!ok) { remove(fp); return "no_space"; }
    *out = ent;
    return NULL;
}

int pkg_store_list(const char *root, pkg_entry_t *out, int max) {
    pkg_entry_t *e = entries();
    int n = e ? load(root, e, PKG_INSTALLED_MAX) : 0, k = 0;
    for (int i = 0; i < n && k < max; i++) if (exists(root, e[i].file)) out[k++] = e[i];
    STORE_FREE(e);
    return k;
}

bool pkg_store_find(const char *root, const char *id, pkg_entry_t *out) {
    pkg_entry_t *e = entries();
    int n = e ? pkg_store_list(root, e, PKG_INSTALLED_MAX) : 0, i = find(e, n, id);
    if (i >= 0 && out) *out = e[i];
    STORE_FREE(e);
    return i >= 0;
}

const char *pkg_store_remove(const char *root, const char *id) {
    pkg_entry_t *e = entries();
    int n = e ? load(root, e, PKG_INSTALLED_MAX) : 0, i = find(e, n, id);
    const bool present = i >= 0 && exists(root, e[i].file);
    if (i >= 0) {
        drop_file(root, e, n, i);
        e[i] = e[--n];
        save(root, e, n);
    }
    STORE_FREE(e);
    return present ? NULL : "not_installed";   /* a vanished file: its entry goes too */
}

char *pkg_store_read(const char *root, const char *id, size_t *len) {
    pkg_entry_t e;
    if (!pkg_store_find(root, id, &e)) return NULL;
    char p[96];
    path(p, sizeof(p), root, e.file);
    return slurp(p, PKG_FILE_MAX, len);
}

void pkg_store_free(char *buf) { STORE_FREE(buf); }
