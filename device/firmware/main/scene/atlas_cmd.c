/*
 * SPDX-License-Identifier: MIT
 *
 * atlas_cmd.c: the link's atlas.load and atlas.show (docs/AGENT-LINK.md "Atlas", cap "atlas"): what atlas.c makes of
 * them, as the link answers. Quick: a kind's objects parsed into PSRAM, nothing drawn here (the fluid task draws).
 */
#include <string.h>
#include "atlas.h"
#include "link_cmd.h"
#include "pkg_cmd.h"

static esp_err_t answer(const char *err) {
    if (!err) return ESP_OK;
    if (!strcmp(err, "invalid_args")) return ESP_ERR_INVALID_ARG;
    if (!strcmp(err, "oom")) return ESP_ERR_NO_MEM;
    return link_cmd_refuse(err);
}
esp_err_t atlas_cmd_load(const cJSON *args, cJSON **out) { (void)out; return answer(atlas_load(args)); }
esp_err_t atlas_cmd_show(const cJSON *args, cJSON **out) { (void)out; return answer(atlas_show(args)); }
