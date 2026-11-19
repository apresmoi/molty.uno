/*
 * SPDX-License-Identifier: MIT
 *
 * web_cat.c: where a rigged creature stands and where the camera looks, for the website's entrance (the cat walks in
 * from outside its screen). This file IS main/creature/creature_clip.c (included below, whole): the player keeps the
 * creature's place and its camera to itself, so the two things the page needs are written next to it. They move
 * state the player already has; nothing here plays, routes or draws.
 *
 *   web_cat_place   the creature, as it stands, put `across` to the side of the frame's middle and `back` away from
 *                   the viewer (world units). A call (cc_come: what a tap does) then walks it in with its own clips.
 *   web_cat_across  where it is now, across the frame and back from the viewer (web_cat_back)
 *   web_cat_coming  it is still on its way (or attending); web_cat_attend: once it has arrived, it attends for this
 *                   long only (the player's own 3 s is for a tap; an entrance ends when it sits).
 *   web_cat_pan     the picture's camera looks `across` to the side (world units) of where the player's camera does:
 *                   the same world through a window next to the screen's. The player's own camera, which the creature
 *                   steers by, does not move. 0 on the device, always.
 */
#define cc_cam cc_cam_own
#include "../main/creature/creature_clip.c"
#undef cc_cam

static float s_pan;

void web_cat_pan(float across) { s_pan = across; }

void web_cat_place(cc_t *c, float across, float back) {
    const float D2R = 3.14159265358979f / 180.f;
    const float rx = cosf(c->camyaw * D2R), rz = -sinf(c->camyaw * D2R);   /* across the frame */
    const float fx = sinf(c->camyaw * D2R), fz = cosf(c->camyaw * D2R);    /* toward the viewer */
    c->px = c->tx = c->camx + across * rx - back * fx;
    c->pz = c->tz = c->camz + across * rz - back * fz;
}

/* how far to the side of the frame's middle it stands, and how far back from the viewer (world units) */
float web_cat_across(const cc_t *c) {
    const float D2R = 3.14159265358979f / 180.f;
    return (c->px - c->camx) * cosf(c->camyaw * D2R) - (c->pz - c->camz) * sinf(c->camyaw * D2R);
}
float web_cat_back(const cc_t *c) {
    const float D2R = 3.14159265358979f / 180.f;
    return -((c->px - c->camx) * sinf(c->camyaw * D2R) + (c->pz - c->camz) * cosf(c->camyaw * D2R));
}
/* still on its way (or attending). A call gives up after CC_COME_S of walking: from far outside the frame it has
 * further to go than a tap ever asks, so while it is still outside its walk does not count */
bool web_cat_coming(cc_t *c) {
    if (c->come == 1 && fabsf(web_cat_across(c)) > 0.9f) c->comeT = 0.f;
    return c->come != 0;
}
/* arrived (attending): it attends for `seconds` only, and what is left between where its walk stopped and its own
 * place (the camera's middle: where it sits when nobody called it) is taken up a little each frame, unseen, so the
 * entrance ends exactly where a cat that never left sits. home: there now, whatever is left. */
void web_cat_attend(cc_t *c, float seconds) {
    if (c->come < 2 || c->come > 3) return;
    if (c->gesture >= 0 && c->gest[c->gesture].hold > seconds) c->gest[c->gesture].hold = seconds;
    c->px += (c->camx - c->px) * 0.08f; c->pz += (c->camz - c->pz) * 0.08f;
    c->tx = c->px; c->tz = c->pz;
}
void web_cat_home(cc_t *c) { c->px = c->tx = c->camx; c->pz = c->tz = c->camz; }

void cc_cam(const cc_t *c, float *x, float *z, float *yaw) {
    cc_cam_own(c, x, z, yaw);
    const float D2R = 3.14159265358979f / 180.f;
    *x += s_pan * cosf(*yaw * D2R); *z -= s_pan * sinf(*yaw * D2R);
}
