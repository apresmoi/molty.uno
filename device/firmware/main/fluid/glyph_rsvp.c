/* glyph_rsvp.c — Spritz-style optimal-recognition-point (ORP) for the fluid's
 * speed reader. From glyphcss-c (the glyphcss UI reader).
 * glyphcss © 2025 Layoutit, MIT License. SPDX-License-Identifier: MIT */
#include "glyph_rsvp.h"

#include <string.h>

static int gc_rsvp_is_ascii_alnum(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

int gc_rsvp_orp_bucket(int core_len) {
    if (core_len <= 1) return 0;
    if (core_len <= 5) return 1;
    if (core_len <= 9) return 2;
    if (core_len <= 13) return 3;
    return 4;
}

gc_rsvp_orp_t gc_rsvp_compute_orp(const char *token, int token_len) {
    gc_rsvp_orp_t result = { 0, 0, 0 };
    if (!token) return result;
    if (token_len < 0) token_len = (int)strlen(token);
    result.length = token_len;
    if (token_len <= 0) return result;

    int first = 0;
    int last = token_len - 1;
    while (first < token_len && !gc_rsvp_is_ascii_alnum((unsigned char)token[first])) first++;
    while (last >= 0 && !gc_rsvp_is_ascii_alnum((unsigned char)token[last])) last--;

    int core_start = first > last ? 0 : first;
    result.core_len = first > last ? token_len : last - first + 1;
    result.focal = core_start + gc_rsvp_orp_bucket(result.core_len);
    if (result.focal >= token_len) result.focal = token_len - 1;
    return result;
}

int gc_rsvp_pivot_width(const char *token, int token_len, int padding, int min_width, int explicit_focal) {
    gc_rsvp_orp_t orp = gc_rsvp_compute_orp(token, token_len);
    int focal = orp.focal;
    if (explicit_focal >= 0 && orp.length > 0) {
        focal = explicit_focal;
        if (focal >= orp.length) focal = orp.length - 1;
    }
    int left = focal;
    int right = orp.length > 0 ? orp.length - focal - 1 : 0;
    int wider = left > right ? left : right;
    int span = orp.length > 0 ? wider * 2 + 1 : 0;
    int width = span + (padding > 0 ? padding : 0);
    return width > min_width ? width : min_width;
}
