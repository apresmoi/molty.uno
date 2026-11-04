/*
 * SPDX-License-Identifier: MIT
 *
 * pkg_creature.h: the creatures this Molty can be (docs/PACKAGES.md section 5): the creature
 * packages baked into the image (every one in packages/ at build time, by id: the firmware
 * names none of them) and the ones installed since (pkg.install). An installed package
 * replaces a built-in one with its id. The choice is kept in NVS ("fluid" / "creature").
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "pkg_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

/* one built-in creature package (generated: build/creature_builtin.c from packages/index.json) */
typedef struct { const char *id, *name, *version, *start, *end; } pkgc_builtin_t;
extern const pkgc_builtin_t PKGC_BUILTINS[];
extern const int PKGC_N_BUILTINS;

typedef struct { char id[PKG_ID_MAX + 1], name[PKG_TEXT_MAX + 1], version[PKG_VERSION_MAX + 1]; bool builtin; } pkgc_entry_t;

/** The built-in creatures, then the installed ones (one that shares a built-in's id takes its place). */
int   pkgc_list(pkgc_entry_t *out, int max);
/** The package file of creature `id` (installed first), PSRAM, free with heap_caps_free; NULL: none. */
char *pkgc_bytes(const char *id, size_t *len);
/** The creature chosen last (POST /api/creature/select, creature.select): true with its id, or
 *  with "" when the uploaded .molty was chosen since; false when nothing was ever chosen. */
bool  pkgc_chosen(char id[PKG_ID_MAX + 1]);
/** Keep the choice ("" = the uploaded .molty). */
esp_err_t pkgc_choose(const char *id);

#ifdef __cplusplus
}
#endif
