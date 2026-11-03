/*
 * SPDX-License-Identifier: MIT
 *
 * pkg_parse.h — a package file (docs/PACKAGES.md section 1) into bounded structs, strictly.
 *
 * Plain C + cJSON, no FreeRTOS / ESP-IDF: pkg_cmd.c parses on the link task (one
 * allocation in PSRAM on the device), test/host/pkg_test.c runs the same code.
 *
 *   strict    any key the format does not name, a wrong type, an out-of-range number, an
 *             unknown item kind, an asset name that resolves to nothing, a binding to an
 *             undeclared param (or one of the wrong type), an "@field" outside an "each":
 *             the whole file is refused, with the reason in *err.
 *   bounded   every count and size below has a cap; nothing in the file can make the
 *             device allocate more than the file's own size times a small factor.
 *   values    a field holds a literal, a "$param" (read from scene.play's params) or an
 *             "@field" (read from the element of the item's "each" list). Bindings are
 *             resolved at play time (scene_build, scene_player.h), where their values are
 *             type-checked again.
 * The firmware knows shapes, not subjects: nothing here names what a package is about.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PKG_FILE_MAX      (256 * 1024)  /* bytes of JSON                                   */
#define PKG_ID_MAX        32            /* [a-z0-9-]{1,32}                                  */
#define PKG_TEXT_MAX      32            /* name: printable ASCII                            */
#define PKG_VERSION_MAX   15            /* [0-9A-Za-z.-]                                    */
#define PKG_KEY_MAX       24            /* asset / layout / param / field names: [A-Za-z0-9_-] */
#define PKG_ASSETS_MAX    64
#define PKG_ASSET_SIDE    128           /* px, either side                                  */
#define PKG_PALETTE_MAX   16
#define PKG_LAYOUTS_MAX   32
#define PKG_ITEMS_MAX     64            /* per layout                                       */
#define PKG_PARAMS_MAX    8             /* per layout                                       */
#define PKG_PTS_MAX       16            /* per line / curve                                 */
#define PKG_STR_MAX       40            /* a text item's characters (= the letter overlay)  */
#define PKG_DUR_MAX       600.f         /* s                                                */

enum { PK_RING, PK_ARC, PK_TICKS, PK_LINE, PK_CURVE, PK_DISC, PK_BALL, PK_GLYPH, PK_TEXT, PK_KINDS };
extern const char *const PKG_KIND_NAMES[PK_KINDS];

/* an item's fields; the numeric ones first (PF_NUMS) */
enum { PF_X, PF_Y, PF_R, PF_W, PF_A0, PF_A1, PF_LEN, PF_N, PF_SIZE, PF_LIT, PF_SIDE, PF_SPIN,
       PF_ROT, PF_ON, PF_OFF, PF_PA, PF_PR, PF_NUMS = PF_PR + 1,
       PF_COLOR = PF_NUMS, PF_ASSET, PF_STR, PF_COUNT };

enum { PV_NONE, PV_NUM, PV_COLOR, PV_ASSET, PV_STR, PV_PARAM, PV_ELEM };
typedef struct {
    uint8_t src;                 /* PV_*                                                  */
    int16_t idx;                 /* PV_ASSET: asset index; PV_PARAM: param index           */
    float num;                   /* PV_NUM                                                 */
    uint32_t rgb;                /* PV_COLOR: 0xRRGGBB                                     */
    const char *s;               /* PV_STR: the text; PV_ELEM: the element's field name    */
} pkg_val_t;

typedef struct {
    uint8_t k;                   /* PK_*                                                   */
    int8_t each;                 /* the list param it repeats over, -1 = once              */
    bool polar;                  /* PF_PA / PF_PR place it instead of PF_X / PF_Y           */
    uint8_t npts;
    pkg_val_t f[PF_COUNT];
    pkg_val_t (*pts)[2];         /* line / curve                                           */
} pkg_item_t;

enum { PP_NUMBER, PP_TEXT, PP_LIST };
typedef struct { const char *name; uint8_t type; } pkg_param_t;

typedef struct {
    const char *name;
    float dur;                   /* s; 0 = until replaced or its ttl ends                  */
    int nparams, nitems;
    pkg_param_t params[PKG_PARAMS_MAX];
    pkg_item_t *items;
} pkg_layout_t;

enum { PA_MASK, PA_IMAGE };
typedef struct {
    const char *name;
    uint8_t kind, npal;
    uint16_t w, h;
    const uint8_t *data;         /* mask: 1 bit/px MSB first; image: 4 bits/px high nibble first;
                                  * rows top to bottom, no row padding                      */
    uint32_t pal[PKG_PALETTE_MAX];   /* image: 0xRRGGBB; index 0 is transparent             */
} pkg_asset_t;

enum { PKG_SCENES, PKG_CREATURE };
typedef struct {
    char id[PKG_ID_MAX + 1], name[PKG_TEXT_MAX + 1], version[PKG_VERSION_MAX + 1];
    uint8_t kind;                /* PKG_SCENES / PKG_CREATURE                              */
    int nassets, nlayouts;
    pkg_asset_t *assets;
    pkg_layout_t *layouts;
    int refs;                    /* pkg_ref / pkg_unref (atomic)                           */
    size_t used, cap;            /* the arena: this struct, then everything it points to   */
} pkg_t;

/** Parse len bytes of a package file. NULL with *err (static) on any violation; else a
 *  package with one reference, freed by pkg_unref. */
pkg_t *pkg_parse(const char *json, size_t len, const char **err);
void   pkg_ref(pkg_t *p);
void   pkg_unref(pkg_t *p);
const pkg_layout_t *pkg_layout(const pkg_t *p, const char *name);
/** "#rrggbb" → 0xRRGGBB. */
bool   pkg_color(const char *s, uint32_t *rgb);
/** The range a numeric field's value must lie in (literals at parse, bound values at play). */
bool   pkg_num_ok(int pf, float v);
/** [a-z0-9-]{1,32} */
bool   pkg_id_ok(const char *id);
/** 1..max printable ASCII characters. */
bool   pkg_text_ok(const char *s, size_t max);

#ifdef __cplusplus
}
#endif
