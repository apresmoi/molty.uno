/* sta_watch — see sta_watch.h. */
#include "sta_watch.h"

void sta_watch_init(sta_watch_t *w)
{
    w->up = false;
    w->evented = false;
    w->outage_logged = false;
}

void sta_watch_seed(sta_watch_t *w, bool associated)
{
    if (!w->evented) w->up = associated;
}

bool sta_watch_event(sta_watch_t *w, sta_watch_ev_t ev)
{
    w->evented = true;
    if (ev == STA_WATCH_EV_CONNECTED) {
        w->up = true;
        w->outage_logged = false;
        return false;
    }
    w->up = false;
    if (w->outage_logged) return false;
    w->outage_logged = true;
    return true;
}

bool sta_watch_up(const sta_watch_t *w) { return w->up; }
