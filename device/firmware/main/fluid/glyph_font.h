/* glyph_font — 5x7 bitmap font + ASCII shading ramp (from glyphcss-c,
 * glyphcss © 2025 Layoutit, MIT). SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 7 rows, bit4 = leftmost of 5 columns. */
const uint8_t *gc_font5x7(char c);
/* a lowercase b for a flat (Db, Eb): a code of its own, so every other lowercase b is drawn as ever (as uppercase) */
#define GC_FLAT '\x01'

/* " .,:;!+=*xX#@" — dark → bright, every glyph renderable by gc_font5x7. */
extern const char *GC_RAMP_ASCII;

#ifdef __cplusplus
}
#endif
