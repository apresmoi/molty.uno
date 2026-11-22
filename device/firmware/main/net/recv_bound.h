/* recv_bound: one chunk of a request body, given up when the sender goes silent (plain C,
 * host-tested: test/host/recv_bound_test.c).
 *
 * esp_http_server runs every handler on its one task, and httpd_req_recv returns
 * HTTPD_SOCK_ERR_TIMEOUT after each recv_wait_timeout (5 s, the IDF default) without data.
 * Retrying that forever let one dead upload hold the whole web server (2026-11-22: an OTA
 * on a weak signal stopped at 253 KB, "error in recv : 11" every 5 s, every new connection
 * reset and the update claim held until a reboot by hand). A read now waits out those
 * short timeouts only until RECV_SILENCE_US has passed without a byte. */
#pragma once
#include <stdint.h>

/* 30 s of silence: six of the server's 5 s timeouts, far longer than a Wi-Fi hiccup or a
 * sender's pause, short enough that a dead upload frees the server in half a minute */
#define RECV_SILENCE_US (30LL * 1000000)
#define RECV_TIMEOUT    (-3)   /* = HTTPD_SOCK_ERR_TIMEOUT (http.c checks it) */

/* One receive: > 0 bytes, RECV_TIMEOUT when none came in its wait, anything else an error. */
typedef int (*recv_fn_t)(void *ctx, char *buf, int len);

/* > 0: bytes read. RECV_TIMEOUT: none for RECV_SILENCE_US (now_us: a microsecond clock).
 * Anything else: recv's own error, at once. */
int recv_bound(recv_fn_t recv, void *ctx, char *buf, int len, int64_t (*now_us)(void));
