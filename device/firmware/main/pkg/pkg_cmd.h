/*
 * SPDX-License-Identifier: MIT
 *
 * pkg_cmd.h: packages and scenes on the link and the local API (docs/PACKAGES.md
 * sections 2 and 3; the rules are pkg_store.h, pkg_parse.h and scene_player.h).
 *
 *   pkg.install {id, url, sha256, size, name, job?, timeout_ms?}: the owner is asked with
 *            one tap (the ask screen: the name, green, or "no"), then `res ok {job?, id}`.
 *            Refusals at once: invalid_args, bad_url, too_big, no_space, busy (an install
 *            is under way), unsupported (no touch: nobody could say yes). Then one event
 *            {"t":"evt","name":"pkg","data":{job?, id, state, ...}}: state "installed"
 *            (+version), "declined" (+reason: "declined" | "timeout" | "cancelled") or
 *            "failed" (+error: hash_mismatch, too_big, bad_package, no_space, download,
 *            tls, http_<code>, ...). The file comes straight from the allowlisted URL over
 *            verified TLS (the certificate bundle), redirects not followed.
 *   pkg.remove {id} → {removed}; not_installed.   pkg.list {} → {packages: [...]}.
 *   scene.play / scene.queue {pkg, layout, params?, ttl_s?} → {pkg, layout, ttl_s};
 *            not_installed, no_layout, bad_params, bad_package, busy (queue full).
 *   scene.clear {} → {}: back to the creature.
 *   local    GET /api/pkg, DELETE /api/pkg?id=, POST /api/scene (scene.play's args).
 */
#pragma once
#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t pkg_cmd_install(const cJSON *args, cJSON **result);
esp_err_t pkg_cmd_remove(const cJSON *args, cJSON **result);
esp_err_t pkg_cmd_list(const cJSON *args, cJSON **result);
esp_err_t pkg_cmd_scene_play(const cJSON *args, cJSON **result);
esp_err_t pkg_cmd_scene_queue(const cJSON *args, cJSON **result);
esp_err_t pkg_cmd_scene_clear(const cJSON *args, cJSON **result);
esp_err_t pkg_http_register(httpd_handle_t server);
