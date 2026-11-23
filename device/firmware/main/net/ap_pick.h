/* ap_pick: which access point of the saved network to join (plain C, host-tested:
 * test/host/ap_pick_test.c).
 *
 * The driver alone joins the strongest AP of the network on every join. On a mesh that hops:
 * the signal swings by 20 dB on one spot, so each restart or rejoin can land on another node
 * than the one that just worked, and a repeater that still answers for Molty from the old node
 * can hold the new lease back for minutes (2026-11-22). So network.c remembers two BSSIDs:
 *   - the last one that gave an address: preferred while its signal is within AP_PICK_MARGIN_DB
 *     of the strongest (a stale favourite far weaker than another node is dropped for it);
 *   - the last one that gave no address (the lease ladder, wifi_retry.h) or could not be joined
 *     when aimed at: avoided for AP_AVOID_MS, unless it is the only one in range.
 * Nothing is stored in flash: the memory lives in no-init RAM, so a soft restart (OTA, panic,
 * esp_restart) keeps the last good AP; a power cycle forgets it and joins the strongest, as before.
 * The avoided one is dropped at a restart (its time is uptime). With nothing remembered network.c
 * joins exactly as before, with no scan of its own. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define AP_PICK_MARGIN_DB 8        /* about the scan-to-scan swing of one AP on one spot */
#define AP_AVOID_MS      180000u   /* a repeater's stale entry for a client ages out in a few minutes */
#define AP_PICK_MAX      8         /* APs of the network kept from one scan, the strongest */

typedef struct { uint8_t bssid[6]; int8_t rssi; uint8_t channel; } ap_seen_t;

typedef struct {
    uint32_t magic;              /* AP_MEM_MAGIC when the rest is valid (no-init RAM) */
    uint8_t good[6];             /* gave an address last; all zero: none */
    uint8_t bad[6];              /* gave none, avoided until bad_until_ms; all zero: none */
    uint32_t bad_until_ms;
    uint32_t check;              /* of the fields above: garbage after a power cycle is refused */
} ap_mem_t;

typedef enum { AP_PICK_NONE, AP_PICK_BEST, AP_PICK_LAST } ap_why_t;

/* At boot: keep the good AP if the memory survived intact, forget the rest. */
void ap_mem_boot(ap_mem_t *m);
void ap_mem_good(ap_mem_t *m, const uint8_t bssid[6]);                  /* it gave an address */
void ap_mem_bad(ap_mem_t *m, const uint8_t bssid[6], uint32_t now_ms);  /* it gave none */
void ap_mem_forget(ap_mem_t *m);                                        /* credentials erased */
/* Something to choose by: worth a scan before the join. */
bool ap_mem_active(const ap_mem_t *m, uint32_t now_ms);

/* Keep the AP_PICK_MAX strongest of a scan: add one record to seen[*n]. */
void ap_seen_add(ap_seen_t *seen, int *n, const uint8_t bssid[6], int8_t rssi, uint8_t channel);
/* The AP to join among seen[0..n), or -1 (none seen: let the driver scan and pick). */
int ap_pick(const ap_mem_t *m, const ap_seen_t *seen, int n, uint32_t now_ms, ap_why_t *why);
