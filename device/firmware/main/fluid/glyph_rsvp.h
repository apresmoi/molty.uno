/* Spritz-style ORP heuristic for the fluid's speed reader (from glyphcss-c).
 * English/ASCII byte contract: non-alphanumeric edges are trimmed.
 * glyphcss © 2025 Layoutit, MIT License. SPDX-License-Identifier: MIT */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int focal;
    int core_len;
    int length;
} gc_rsvp_orp_t;

int gc_rsvp_orp_bucket(int core_len);
gc_rsvp_orp_t gc_rsvp_compute_orp(const char *token, int token_len);
int gc_rsvp_pivot_width(const char *token, int token_len, int padding, int min_width, int explicit_focal);

#ifdef __cplusplus
}
#endif
