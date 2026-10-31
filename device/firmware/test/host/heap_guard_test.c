/* heap_guard (main/diag/heap_guard.h): internal RAM under 12 KB for 3 s in a row → one
 * warning per episode, the shed once per boot; 20 KB for 10 s ends the episode. */
#include "heap_guard.h"
#include <assert.h>
#include <stdio.h>

static int run(heap_guard_t *g, uint32_t free_now, int seconds)
{
    int all = 0;
    for (int i = 0; i < seconds; i++) all |= heap_guard_step(g, free_now);
    return all;
}

int main(void)
{
    heap_guard_t g; heap_guard_init(&g);
    /* steady 33 KB, a listen's dips (26.9 KB, a 2 s TLS handshake at 9 KB): nothing */
    assert(run(&g, 33280, 60) == HG_NONE && run(&g, 26876, 5) == HG_NONE);
    assert(run(&g, 9000, 2) == HG_NONE && run(&g, 33280, 1) == HG_NONE);
    assert(run(&g, 9000, 2) == HG_NONE && run(&g, 33280, 1) == HG_NONE);   /* the count starts over */
    /* the 2026-10-31 hour: free sinking to 2 KB — the 3rd second under 12 KB warns AND sheds */
    assert(heap_guard_step(&g, 11000) == HG_NONE);
    assert(heap_guard_step(&g, 9000) == HG_NONE);
    assert(heap_guard_step(&g, 7000) == (HG_LOW | HG_SHED));
    /* still low: no repeat (one warning per episode, not one a second) */
    assert(run(&g, 2140, 600) == HG_NONE && g.episodes == 1);
    /* the shed gave ~40 KB back: above 20 KB for 10 s ends it, once */
    assert(run(&g, 45000, 9) == HG_NONE);
    assert(heap_guard_step(&g, 45000) == HG_OK && run(&g, 45000, 100) == HG_NONE);
    /* hysteresis: 15 KB (between the marks) is neither */
    assert(run(&g, 15000, 100) == HG_NONE);
    /* a second episode warns again — the shed happened already this boot */
    assert(run(&g, 8000, 3) == HG_LOW && g.episodes == 2);
    /* a recovery interrupted by a dip starts its 10 s over */
    assert(run(&g, 30000, 9) == HG_NONE && run(&g, 15000, 1) == HG_NONE && run(&g, 30000, 9) == HG_NONE);
    assert(heap_guard_step(&g, 30000) == HG_OK);
    puts("HEAP GUARD PASS: 3 s under 12 KB warns + sheds (once per boot), one warning per episode, 10 s over 20 KB ends it, dips and the band in between ignored");
    return 0;
}
