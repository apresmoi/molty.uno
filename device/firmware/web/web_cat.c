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

bool web_cat_coming(const cc_t *c) { return c->come != 0; }
void web_cat_attend(cc_t *c, float seconds) {
    if (c->come >= 2 && c->come <= 3 && c->gesture >= 0 && c->gest[c->gesture].hold > seconds) c->gest[c->gesture].hold = seconds;
}

void cc_cam(const cc_t *c, float *x, float *z, float *yaw) {
    cc_cam_own(c, x, z, yaw);
    const float D2R = 3.14159265358979f / 180.f;
    *x += s_pan * cosf(*yaw * D2R); *z -= s_pan * sinf(*yaw * D2R);
}
