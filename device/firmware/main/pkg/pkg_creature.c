/*
 * SPDX-License-Identifier: MIT
 *
 * pkg_creature.c: the built-in and installed creature packages, and the choice (pkg_creature.h).
 */
#include "pkg_creature.h"
#include "pkg_store.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "nvs.h"

int pkgc_list(pkgc_entry_t *out, int max) {
    int n = 0;
    for (int i = 0; i < PKGC_N_BUILTINS && n < max; i++, n++) {
        const pkgc_builtin_t *b = &PKGC_BUILTINS[i];
        strlcpy(out[n].id, b->id, sizeof(out[n].id)); strlcpy(out[n].name, b->name, sizeof(out[n].name));
        strlcpy(out[n].version, b->version, sizeof(out[n].version)); out[n].builtin = true;
    }
    pkg_entry_t *e = heap_caps_malloc(sizeof(pkg_entry_t) * PKG_INSTALLED_MAX, MALLOC_CAP_SPIRAM);
    const int ni = e ? pkg_store_list(PKG_ROOT, e, PKG_INSTALLED_MAX) : 0;
    for (int i = 0; i < ni; i++) {
        if (e[i].kind != PKG_CREATURE) continue;
        int k = 0;
        while (k < n && strcmp(out[k].id, e[i].id)) k++;
        if (k == n) { if (n >= max) continue; n++; }
        strlcpy(out[k].id, e[i].id, sizeof(out[k].id)); strlcpy(out[k].name, e[i].name, sizeof(out[k].name));
        strlcpy(out[k].version, e[i].version, sizeof(out[k].version)); out[k].builtin = false;
    }
    heap_caps_free(e);
    return n;
}

char *pkgc_bytes(const char *id, size_t *len) {
    pkg_entry_t e;
    if (pkg_store_find(PKG_ROOT, id, &e) && e.kind == PKG_CREATURE) {
        char *b = pkg_store_read(PKG_ROOT, id, len);
        if (b) return b;
    }
    for (int i = 0; i < PKGC_N_BUILTINS; i++) {
        const pkgc_builtin_t *b = &PKGC_BUILTINS[i];
        if (strcmp(b->id, id)) continue;
        const size_t n = (size_t)(b->end - b->start);
        char *c = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!c) return NULL;
        memcpy(c, b->start, n); c[n] = 0;
        *len = n;
        return c;
    }
    return NULL;
}

bool pkgc_chosen(char id[PKG_ID_MAX + 1]) {
    nvs_handle_t h;
    size_t n = PKG_ID_MAX + 1;
    id[0] = 0;
    if (nvs_open("fluid", NVS_READONLY, &h) != ESP_OK) return false;
    const bool ok = nvs_get_str(h, "creature", id, &n) == ESP_OK;
    nvs_close(h);
    if (!ok || (id[0] && !pkg_id_ok(id))) { id[0] = 0; return false; }
    return true;
}

esp_err_t pkgc_choose(const char *id) {
    nvs_handle_t h;
    esp_err_t e = nvs_open("fluid", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_str(h, "creature", id);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}
