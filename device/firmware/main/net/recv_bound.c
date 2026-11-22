#include "recv_bound.h"

int recv_bound(recv_fn_t recv, void *ctx, char *buf, int len, int64_t (*now_us)(void))
{
    const int64_t start = now_us();
    for (;;) {
        const int n = recv(ctx, buf, len);
        if (n != RECV_TIMEOUT || now_us() - start >= RECV_SILENCE_US) return n;
    }
}
