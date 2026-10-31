#include "heap_guard.h"
#include <string.h>

void heap_guard_init(heap_guard_t *g) { memset(g, 0, sizeof(*g)); }

int heap_guard_step(heap_guard_t *g, uint32_t free_now)
{
    int out = HG_NONE;
    g->below = free_now < HEAP_GUARD_LOW ? g->below + 1 : 0;
    g->above = free_now > HEAP_GUARD_OK ? g->above + 1 : 0;
    if (!g->low && g->below >= HEAP_GUARD_LOW_S) {
        g->low = true; g->episodes++;
        out |= HG_LOW;
        if (!g->shed) { g->shed = true; out |= HG_SHED; }
    } else if (g->low && g->above >= HEAP_GUARD_OK_S) {
        g->low = false;
        out |= HG_OK;
    }
    return out;
}
