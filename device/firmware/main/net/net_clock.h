/*
 * SPDX-License-Identifier: MIT
 *
 * net_clock.h: the date, UTC, from network time (SNTP through esp_netif, CONFIG_MOLTY_SNTP_SERVER) once Wi-Fi has an
 * address, synced again every CONFIG_LWIP_SNTP_UPDATE_DELAY (an hour). The chip's own clock keeps it across a soft
 * restart (ESP-IDF keeps the system time in the RTC through any reset but a power-on): a time past 2025 at boot is
 * taken as kept. Nothing is written to flash. Until the date is known there is none (net_clock_utc 0): no sky.
 * GET /api/diag reports it (time: {utc, source, synced_s_ago}).
 */
#pragma once

#include "cJSON.h"

/** Once, with the first address (network.c): network time starts. Any later call does nothing. */
void   net_clock_start(void);
/** The time now, unix s UTC, or 0 while it is not known. Any task. */
double net_clock_utc(void);
/** {utc (s, or null), source: "sntp" | "kept" | "none", synced_s_ago (or null)} */
cJSON *net_clock_json(void);
