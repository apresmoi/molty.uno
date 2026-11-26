/*
 * SPDX-License-Identifier: MIT
 *
 * rhythm_wire.h: rhythm.play's `rhythms` (docs/AGENT-LINK.md "Rhythm"; the molty-rhythm skill's library.mjs wire())
 * into the beat player's patterns, strictly: an unknown key, a step outside the bar, an accent that is not a hit, a
 * bar without a hit list a layer, more than the player holds: refused with the reason. Plain C + cJSON.
 */
#pragma once
#include <stdbool.h>
#include "cJSON.h"
#include "rhythm_player.h"

/** The patterns of arr (1..RHY_LIB) into out, each `region` below nregions; how many, or -1 with *err. */
int rhy_parse_lib(const cJSON *arr, int nregions, rhy_pattern_t *out, const char **err);
/** A region of the library, as the wheel shows it: its label (capitals, up to 12) and its tint. */
typedef struct { char label[RHY_LABEL_MAX + 1]; uint32_t rgb; } rhy_region_t;
/** rhythm.play's `regions` (1..RHY_REGIONS {label, color}) into out; how many, 0 when absent, -1 with *err. */
int rhy_parse_regions(const cJSON *arr, rhy_region_t *out, const char **err);
/** A list of pt's layer ids into layer bits (NULL: none). False: not a list of its ids. */
bool rhy_parse_mute(const rhy_pattern_t *pt, const cJSON *ids, uint32_t *mute);
/** pt's layer named id, -1 none. */
int rhy_layer_of(const rhy_pattern_t *pt, const char *id);
