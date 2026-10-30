/*
 * self_update.h — Molty installs a firmware release by itself (docs/AGENT-LINK.md,
 * "Firmware updates"; the rules are update_core.h).
 *
 *   link     `cmd update {job, version, url, sha256, size, timeout_ms}`: the "update" /
 *            "later" question goes on screen (the ask screen), then `res ok {job}`.
 *            Refusals: busy, bad_url, wrong_board, too_big, invalid_args, unsupported (no
 *            touch). Events `update {job, state}`: declined (+reason) / accepted /
 *            downloading (+progress) / flashing / rebooting / failed (+error).
 *   console  GET /api/update → {ok, state, version?, progress?, error?};
 *            POST /api/update {version, url, sha256, size} starts at once (the click is
 *            the consent, the same trust as POST /api/ota) → the GET body, or
 *            {ok:false, error} with the refusal codes above.
 *   download its own task (prio 1, below the fluid), HTTPS with the certificate bundle,
 *            GitHub's redirect to its asset CDN followed (https only), streamed into the
 *            spare OTA slot; the reader shows "update", 10 %…100 %, "restart".
 *   boot     self_update_boot() replaces "valid once the HTTP server is up": see
 *            update_core.h "the mark-valid rule".
 */
#pragma once
#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

/* The link's `update` command (link task). */
esp_err_t self_update_cmd(const cJSON *args, cJSON **result);
esp_err_t self_update_http_register(httpd_handle_t server);
/* app_main, where the image used to be marked valid (HTTP server up). */
void self_update_boot(void);
/* link_client, on every `welcome`. */
void self_update_welcomed(void);
/* POST /api/ota: one OTA at a time across both paths. claim → true when it may write
 * the spare slot (release when done or failed); it also drops a stale arm. */
bool self_update_ota_claim(void);
void self_update_ota_release(void);
/* An update or OTA is writing the spare slot (its progress owns the screen): no shake to talk. */
bool self_update_busy(void);
