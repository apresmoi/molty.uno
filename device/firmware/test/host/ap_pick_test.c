/* ap_pick (main/net/ap_pick.h): the last AP that gave an address is preferred within the margin,
 * one that gave none is avoided only for a while, and with nothing remembered it is the strongest. */
#include "ap_pick.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t A[6] = {0xe2, 0x96, 0x5f, 0xbb, 0xa9, 0xd9};
static const uint8_t B[6] = {0x92, 0x83, 0xc4, 0x9b, 0x78, 0xef};
static const uint8_t C[6] = {0x3c, 0xa6, 0x2f, 0x00, 0x00, 0x01};

static int idx(const ap_seen_t *s, int n, const uint8_t b[6])
{
    for (int i = 0; i < n; i++) if (!memcmp(s[i].bssid, b, 6)) return i;
    return -1;
}

int main(void)
{
    ap_mem_t m; memset(&m, 0xa5, sizeof m);        /* no-init RAM after a power cycle: garbage */
    ap_mem_boot(&m);
    assert(!ap_mem_active(&m, 0));                  /* refused: nothing remembered, no scan of our own */
    ap_seen_t s[AP_PICK_MAX]; int n = 0; ap_why_t why;
    assert(ap_pick(&m, s, 0, 0, &why) == -1 && why == AP_PICK_NONE);   /* none seen: the driver picks */
    ap_seen_add(s, &n, A, -60, 1);
    ap_seen_add(s, &n, B, -55, 6);
    ap_seen_add(s, &n, C, -75, 11);
    /* nothing remembered: the strongest, as the driver would */
    assert(ap_pick(&m, s, n, 0, &why) == idx(s, n, B) && why == AP_PICK_BEST);
    /* A gave an address: preferred while within 8 dB of the strongest */
    ap_mem_good(&m, A);
    assert(ap_mem_active(&m, 0));
    assert(ap_pick(&m, s, n, 0, &why) == idx(s, n, A) && why == AP_PICK_LAST);
    s[idx(s, n, A)].rssi = -63;                     /* exactly 8 dB weaker: still A */
    assert(ap_pick(&m, s, n, 0, &why) == idx(s, n, A) && why == AP_PICK_LAST);
    s[idx(s, n, A)].rssi = -64;                     /* 9 dB weaker: the strongest */
    assert(ap_pick(&m, s, n, 0, &why) == idx(s, n, B) && why == AP_PICK_BEST);
    s[idx(s, n, A)].rssi = -60;
    /* the remembered one not in range (another network's, or switched off): the strongest */
    ap_seen_t t[AP_PICK_MAX]; int tn = 0;
    ap_seen_add(t, &tn, B, -55, 6);
    ap_seen_add(t, &tn, C, -50, 11);
    assert(ap_pick(&m, t, tn, 0, &why) == idx(t, tn, C) && why == AP_PICK_BEST);
    /* A gave no address: avoided for 3 minutes, even though it is the last good one */
    const uint32_t t0 = 1000;
    ap_mem_bad(&m, A, t0);
    assert(ap_pick(&m, s, n, t0, &why) == idx(s, n, B) && why == AP_PICK_BEST);
    assert(ap_pick(&m, s, n, t0 + AP_AVOID_MS - 1, &why) == idx(s, n, B));
    /* only for a while: then A is the last good one again */
    assert(ap_pick(&m, s, n, t0 + AP_AVOID_MS, &why) == idx(s, n, A) && why == AP_PICK_LAST);
    /* the avoided one is the only AP in range: join it anyway */
    ap_seen_t one[AP_PICK_MAX]; int on = 0;
    ap_seen_add(one, &on, A, -70, 1);
    assert(ap_pick(&m, one, on, t0, &why) == 0 && why == AP_PICK_BEST);
    /* the avoided one is never "last good" while avoided: B gave an address meanwhile */
    ap_mem_good(&m, B);
    assert(ap_pick(&m, s, n, t0, &why) == idx(s, n, B) && why == AP_PICK_LAST);
    /* an address from the avoided one lifts it */
    ap_mem_good(&m, A);
    assert(ap_pick(&m, s, n, t0, &why) == idx(s, n, A) && why == AP_PICK_LAST);
    /* the avoid time across the uptime wrap */
    ap_mem_bad(&m, A, 0xffffff00u);
    assert(ap_pick(&m, s, n, 0x100u, &why) == idx(s, n, B));
    assert(ap_mem_active(&m, 0x100u));
    /* a soft restart keeps the last good AP and drops the avoided one (uptime starts over) */
    ap_mem_good(&m, C);
    ap_mem_bad(&m, A, 5000);
    ap_mem_boot(&m);
    assert(!memcmp(m.good, C, 6));
    assert(ap_pick(&m, s, n, 5000, &why) == idx(s, n, B) && why == AP_PICK_BEST);   /* C 20 dB weaker */
    s[idx(s, n, C)].rssi = -58;
    assert(ap_pick(&m, s, n, 5000, &why) == idx(s, n, C) && why == AP_PICK_LAST);
    /* a corrupted memory is refused at boot */
    m.good[2] ^= 0x10;
    ap_mem_boot(&m);
    assert(!ap_mem_active(&m, 0));
    /* forget: credentials erased, nothing kept */
    ap_mem_good(&m, A);
    ap_mem_bad(&m, B, 0);
    ap_mem_forget(&m);
    assert(!ap_mem_active(&m, 0));
    ap_mem_boot(&m);
    assert(!ap_mem_active(&m, 0));
    /* a scan with more APs than kept: the strongest AP_PICK_MAX stay */
    ap_seen_t many[AP_PICK_MAX]; int mn = 0;
    for (int i = 0; i < 20; i++) {
        uint8_t b[6] = {2, 0, 0, 0, 0, (uint8_t)i};
        ap_seen_add(many, &mn, b, (int8_t)(-90 + i), 1);
    }
    assert(mn == AP_PICK_MAX);
    for (int i = 0; i < mn; i++) assert(many[i].rssi >= -90 + 20 - AP_PICK_MAX);
    puts("AP PICK PASS: the strongest with nothing remembered, the last good AP within 8 dB, a failed AP avoided for 3 min unless alone, kept across a soft restart, garbage refused");
    return 0;
}
