/* recv_bound (main/net/recv_bound.h): a body read waits out the server's 5 s receive timeouts,
 * but a sender silent for RECV_SILENCE_US is given up, so its handler returns. */
#include "recv_bound.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int64_t s_now;                 /* the fake clock, us */
static int s_calls, s_quiet;          /* s_quiet: timeouts before the data (-1: forever) */
static int s_err;                     /* 0: data after the timeouts, else this error */

static int64_t now_us(void) { return s_now; }
static int fake_recv(void *ctx, char *buf, int len)
{
    (void)ctx;
    assert(++s_calls < 1000);         /* retried forever: the bug this guards */
    if (s_quiet != 0) {
        if (s_quiet > 0) s_quiet--;
        s_now += 5LL * 1000000;       /* each timeout is the server's recv_wait_timeout */
        return RECV_TIMEOUT;
    }
    if (s_err) return s_err;
    memset(buf, 'x', (size_t)len);
    return len;
}
static void reset(int quiet, int err) { s_now = 1000; s_calls = 0; s_quiet = quiet; s_err = err; }

int main(void)
{
    char buf[16];
    /* a dead sender: timeouts forever. Given up after 30 s (6 timeouts), not retried forever */
    reset(-1, 0);
    assert(recv_bound(fake_recv, NULL, buf, sizeof(buf), now_us) == RECV_TIMEOUT);
    assert(s_calls == 6 && s_now - 1000 == RECV_SILENCE_US);
    /* data at once */
    reset(0, 0);
    assert(recv_bound(fake_recv, NULL, buf, 8, now_us) == 8 && s_calls == 1);
    /* a short pause (25 s of timeouts), then data: waited out */
    reset(5, 0);
    assert(recv_bound(fake_recv, NULL, buf, 8, now_us) == 8 && s_calls == 6);
    /* the silence counts from each read: every chunk may pause 25 s and the body still arrives */
    reset(5, 0);
    for (int i = 0; i < 10; i++) {
        assert(recv_bound(fake_recv, NULL, buf, 4, now_us) == 4);
        s_quiet = 5;
    }
    /* a socket error comes back at once, as before */
    reset(2, -1);
    assert(recv_bound(fake_recv, NULL, buf, 8, now_us) == -1 && s_calls == 3);
    puts("RECV BOUND PASS: a silent sender is given up after 30 s of timeouts, pauses under that are waited out, errors return at once");
    return 0;
}
