/*
 * SPDX-License-Identifier: MIT
 *
 * web_device.c: the simulator's calls (the website's /sim/ page), included at the end of web_molty.c: it reads the
 * fluid's own state. Nothing here draws. Three parts:
 *
 *   device mode  the panel driven as the device drives it (the 1.75C driver: whole row pairs, odd edges from its
 *                framebuffer, web_molty.c molty_display_blit): the drawer task keeps what it last drew from frame to frame
 *                (here the task starts from the top each frame, so its locals are new: web_molty.c renames its three
 *                calls to the ones below, which give them back what the locals keep on the device), so only the
 *                windows the device sends are sent; the panel keeps the last pixels it was sent; and each frame is
 *                costed with the panel's measured constants (the table below): the caller shows the frame that long
 *                and moves the firmware's clock by as much.
 *   the senses   sense.c's handling of the touch, the two buttons and the IMU, on the same rules (gesture.c: tap or
 *                double tap, a tap during a recording, the bindings, the rate limits, the shake detector fed an IMU
 *                trace), and the link's part as small stand-ins (a listen, a question's answer, an install's
 *                download), each named below. sense.c's task cannot run here (an endless loop on the link's calls):
 *                sense_tick follows its loop body step by step. Every event an agent would hear is built by
 *                link_protocol.c's own frames and handed to the page (wm_events).
 *   lab          the firmware's experiment switches, a line each (LAB below).
 */
#include "gesture.h"
#include "haptic_plan.h"

/* ── device mode: the panel's measured costs ─────────────────────────────────────────────────────────────────────
 * One table. A frame on the device is the drawer's cells pass and its windows to the panel (core 0) against the sim's
 * own work (core 1), the two handing over once a frame (the slower of the two is the frame); the frame clock does
 * not go faster than FRAME_MS.                                                                                     */
#define DEV_WIN_MS       0.23    /* a window's commands, calm (test/host/scene_test.c panel_ms: the calm chart on the
                                  * 1.75C, 22 windows and 6.3 kpx in 9 ms)                                              */
#define DEV_PX_US        0.55    /* a pixel drawn and sent, calm (the same)                                            */
#define DEV_WAVE_WIN_MS  0.15    /* a window of a change of picture handed over (scene drawer handover, device split
                                  * at 2113895: push ~4 ms for 26 windows; scene_test.measured.c)                      */
#define DEV_WAVE_COMP_US 0.9     /* a pixel of a wave's box drawn and composed (the same: cmp 20 to 24 ms for 24.6 kpx) */
#define DEV_WAVE_COPY_US 0.05    /* a pixel of a wave's box copied from the kept glass (the same)                      */
#define DEV_CELLS_MS     11.0    /* the drawer's cells pass (scene_test.c CELLS_MS; device logs: cells 9.8 to 12)       */
#define DEV_SIM_MS       6.2     /* the sim task's own work (device log at e9f3195: anim wave.parts simw mean 6.2)     */
#define DEV_CPU_X        100.0   /* the device's cores do the firmware's own work this many times slower than the module
                                  * in a desktop browser: a fresh dust set builds in 0.45 to 0.8 s on the device (scene
                                  * drawer handover) and in 5.6 ms here (an M-series Mac, Node 22). Paces the work the
                                  * firmware budgets by the clock (web_sdk_cpu); a slower computer builds slower.     */
#define DEV_FRAME_MS     (FLUID_FRAME_US / 1000.0)   /* the frame clock's floor (mole_fluid.c, 30 ms)               */

static bool s_dev;                     /* device mode */
static bool s_force_pre;               /* the drawer was asked to draw everything before its task ran (not by it) */
static bool s_drew;                    /* the drawer drew this frame */
static struct { int pal, status; fluid_overlay_t ov; } s_kept = { .pal = -1, .status = -1 };   /* the task's locals */

/* the drawer task's own test: what it last drew has changed (draw_task: full), with what its locals keep. (The mark is
 * compared by the status it comes from: the same but for two statuses with one mark.) */
static bool dev_full(void) {
    return s_full_flush || s_mirror_back || s_dopts.palette != s_kept.pal || s_dstatus != s_kept.status;
}
static int web_draw_cells(fluid_draw_t *d, const fluid_frame_t *f, const fluid_draw_opts_t *o) {
    if (s_dev) {
        const bool full = dev_full();
        d->force = s_force_pre || full;
        if (!full) { fluid_draw_overlay_dirty(d, &s_kept.ov); fluid_draw_overlay_dirty(d, &s_dov); }
    }
    return fluid_draw_cells(d, f, o);
}
static int web_draw_send2(const fluid_draw_t *d, const fluid_draw_opts_t *o, const fluid_overlay_t *ov, int mark, bool full,
                          uint16_t *buf, uint16_t *buf2, int max_px, fluid_send_fn send, void *ctx) {
    if (s_dev) full = dev_full();
    const int n = fluid_draw_send2(d, o, ov, mark, full, buf, buf2, max_px, send, ctx);
    s_kept.pal = o->palette; s_kept.status = s_dstatus; s_kept.ov = *ov;
    s_mirror_back = false; s_drew = true;
    return n;
}
static void web_overlay_dirty(fluid_draw_t *d, const fluid_overlay_t *ov) { fluid_draw_overlay_dirty(d, ov); }

static void sense_tick(uint32_t now);
static bool s_sense;

static void web_drawer_before(void) { s_force_pre = s_draw && s_draw->force; s_drew = false; }

/* the last frame: what was sent, and what the device would have taken for it */
enum { FI_WINS, FI_PX, FI_COMP, FI_WAVE, FI_CELLS_MS, FI_SEND_MS, FI_DRAW_MS, FI_SIM_MS, FI_FRAME_MS, FI_ANIM, FI_N };
static float s_fi[FI_N];

static void web_frame_after(void) {
    const bool wave = s_drew && s_draw && (s_draw->anim_on >> FLUID_ANIM_WAVE & 1);
    const double comp = wave ? s_draw->m.comp : 0;
    const double send = !s_drew ? 0. : wave ? s_sent.wins * DEV_WAVE_WIN_MS + comp * DEV_WAVE_COMP_US / 1000. + (s_sent.px - comp) * DEV_WAVE_COPY_US / 1000.
                                            : s_sent.wins * DEV_WIN_MS + s_sent.px * DEV_PX_US / 1000.;
    const double cells = s_drew ? DEV_CELLS_MS : 0., draw = cells + send;
    const double floor_ms = (fd_asleep(&s_dir) ? FLUID_SLEEP_US : atomic_load(&s_screen) ? FLUID_FRAME_US : FLUID_OFF_POLL_MS * 1000) / 1000.;
    const double work = draw > DEV_SIM_MS ? draw : DEV_SIM_MS;
    s_fi[FI_WINS] = (float)s_sent.wins; s_fi[FI_PX] = (float)s_sent.px; s_fi[FI_COMP] = (float)comp; s_fi[FI_WAVE] = wave;
    s_fi[FI_CELLS_MS] = (float)cells; s_fi[FI_SEND_MS] = (float)send; s_fi[FI_DRAW_MS] = (float)draw; s_fi[FI_SIM_MS] = (float)DEV_SIM_MS;
    s_fi[FI_FRAME_MS] = (float)(work > floor_ms ? work : floor_ms);
    s_fi[FI_ANIM] = s_draw ? (float)s_draw->anim_on : 0.f;
    s_sent.wins = 0; s_sent.px = 0;
    if (s_sense) sense_tick((uint32_t)(s_clock / 1000));
}

/** Device mode on or off. On: only the windows the device sends go to the panel, which keeps the rest; off: the whole
 *  panel every frame (the page's look). The first frame either way is drawn whole. */
WM_API void wm_device(int on) { s_dev = s_dev_panel = on != 0; s_kept.pal = -1; web_sdk_cpu(s_dev ? DEV_CPU_X : 0.); }
/** The last frame: its windows, pixels, a wave's composed pixels, a wave on (0/1), and the device's time for it on the
 *  measured constants (ms): cells, send, the drawer's sum, the sim's, the frame's (what the device takes from this frame
 *  to the next: the caller shows it that long and passes it to the next wm_frame), the animations on (bits). */
WM_API const float *wm_frame_info(void) { return s_fi; }

/* ── the senses: sense.c's loop on gesture.c, with the link's part stood in ──────────────────────────────────────── */

#define TAP_MIN_MS    250u             /* sense.c: taps <= 4/s */
#define BUTTON_MIN_MS 250u             /* sense.c */
#define SHAKE_MIN_MS  2000u            /* sense.c */
#define DL_RING_PCT   90.f             /* pkg_cmd.c: the download's share of the ring */

static cJSON *s_evts;                  /* what an agent would hear since the page last asked */
static void ws_emit(cJSON *j) {
    if (!j) return;
    if (!s_evts) s_evts = cJSON_CreateArray();
    if (cJSON_GetArraySize(s_evts) >= 64) cJSON_DeleteItemFromArray(s_evts, 0);
    cJSON_AddItemToArray(s_evts, j);
}

static gesture_t s_g;
static gesture_tapper_t s_tapper, s_presser[2];
static gesture_limit_t s_tap_l = { .min_ms = TAP_MIN_MS }, s_btn_l = { .min_ms = BUTTON_MIN_MS }, s_shake_l = { .min_ms = SHAKE_MIN_MS };
static gesture_switch_t s_sw[2];
static bool s_btn[2], s_btn_prev[2];
static bool s_tap_flag;
static uint32_t s_tap_xy;
static unsigned s_spot;
static uint32_t s_imu_at, s_shake_at;  /* the last IMU sample's time; a shake's start (0: none) */
static bool s_dnd;

/* link_audio.c's listen, stood in: the ring on the glass (mole_fluid_listen, as it does), the shake event that names a
 * listen a gesture or a shake started, the acknowledgement when it ends (link_ack_decide, then the `thinking` mood as
 * link_audio.c sets it). The voice is the page's (wm_listen_level). */
static struct { bool on; uint32_t t0, speech_ms; unsigned n; } s_lis;
static void lis_start(const char *input, bool hold, bool shake, const gesture_shake_t *sh) {
    if (s_lis.on) return;
    s_lis.on = true; s_lis.t0 = (uint32_t)(s_clock / 1000); s_lis.speech_ms = 0;
    mole_fluid_listen_color(-1);
    mole_fluid_listen(true);
    if (!shake && !input) return;      /* the agent's own listen: no event */
    char id[16];
    snprintf(id, sizeof id, "shake-%u", ++s_lis.n);
    ws_emit(link_evt_shake_frame(sh ? sh->peak_g : 0., sh ? sh->reversals : 0, sh ? sh->ms : 0u, id, input, hold));
}
static void lis_stop(const char *reason) {
    if (!s_lis.on) return;
    s_lis.on = false;
    mole_fluid_listen(false);
    if (link_ack_decide((link_ack_t)mole_fluid_ack_mode(), reason, true, s_lis.speech_ms, false, true) != LINK_ACK_OFF) {
        char b[64];
        snprintf(b, sizeof b, "{\"mood\":\"thinking\",\"hold_s\":%d}", LINK_ACK_THINK_S);
        mole_fluid_creature_state_set_json(b);
    }
}

/* the question's other half (link_ask.c, link_pair.c, pkg_cmd.c): who asked, and what its answer does */
enum { Q_ASK, Q_PAIR, Q_INSTALL };
static struct { int kind; char job[40], name[40], version[24]; char label[FLUID_ASK_MAX][FLUID_ASK_LABEL_MAX + 1]; } s_qa;
static struct { bool on; uint32_t t0, ms; } s_dl;   /* an install's download, stood in: its bytes arrive over ms */
/* pkg_cmd.c send_evt: {"t":"evt","name":"pkg","data":{job?, id, state, <key>: <val>}} */
static void pkg_evt(const char *state, const char *key, const char *val) {
    cJSON *j = cJSON_CreateObject(), *d = cJSON_CreateObject();
    if (!j || !d) { cJSON_Delete(j); cJSON_Delete(d); return; }
    cJSON_AddStringToObject(j, "t", "evt"); cJSON_AddStringToObject(j, "name", "pkg");
    if (s_qa.job[0]) cJSON_AddStringToObject(d, "job", s_qa.job);
    cJSON_AddStringToObject(d, "id", s_qa.name); cJSON_AddStringToObject(d, "state", state);
    if (key) cJSON_AddStringToObject(d, key, val);
    cJSON_AddItemToObject(j, "data", d);
    ws_emit(j);
}
static void answered(int i, const char *reason) {
    if (s_qa.kind == Q_PAIR) { ws_emit(link_pair_frame(s_qa.job, i == 0, i == 0 ? NULL : reason ? reason : "rejected")); return; }
    if (s_qa.kind == Q_INSTALL) {      /* pkg_cmd.c on_answer: yes downloads (the ring forms as the question blows away) */
        if (i == 0) { s_dl.on = true; s_dl.t0 = (uint32_t)(s_clock / 1000); mole_fluid_progress(0.f, false); }
        else pkg_evt("declined", "reason", i == 1 ? "declined" : reason);
        return;
    }
    ws_emit(link_evt_answer_frame(s_qa.job, i, i >= 0 ? s_qa.label[i] : NULL, reason));
}
static bool pairing(void) { return s_q.open && s_qa.kind == Q_PAIR; }

/* sense_touch: a pairing or a question takes the press, else a scene's spot (unless it stops a recording), else it is a
 * tap for the task. True: handled (the senses are on); *spot_out: the spot it took (mole_fluid_spot_tap), 0 none. */
static bool web_sense_touch(int x, int y, unsigned *spot_out) {
    if (!s_sense) return false;
    if (s_q.open) {                    /* link_pair_tap / link_ask_tap: never a `tap` event, whatever it lands on */
        const float r = s_px * 0.5f;
        const int i = mole_fluid_ask_shown() == s_q.seq ? fluid_ask_pick(&s_q.hit, ((float)x + 0.5f - r) / r, ((float)y + 0.5f - r) / r) : -1;
        if (i >= 0) { mole_fluid_ask_pick(i); s_q.open = false; s_q.answer = i; answered(i, NULL); }
        return true;
    }
    if (!s_lis.on && mole_fluid_zone(x, y)) { *spot_out = 0; return true; }   /* a wheel's, its centre's, a beat's dot: the fluid's */
    const uint32_t w = (uint32_t)s_px - 1u, X = (uint32_t)x * 65535u / w, Y = (uint32_t)y * 65535u / w;
    s_tap_xy = (X > 65535u ? 65535u : X) << 16 | (Y > 65535u ? 65535u : Y);
    const unsigned spot = s_lis.on ? 0 : mole_fluid_spot_tap(x, y);
    if (spot) s_spot = spot; else s_tap_flag = true;
    *spot_out = spot;
    return true;
}

/* sense.c gesture_do: what the human bound this input to */
static void gesture_do(gesture_input_t in, cJSON *event) {
    const gesture_action_t a = gesture_binds_get(mole_fluid_gestures(), in);
    if (a == GESTURE_DO_EVENT) { ws_emit(event); return; }
    cJSON_Delete(event);
    const bool button = in == GESTURE_IN_BUTTON_ACCEPT || in == GESTURE_IN_BUTTON_REJECT;
    if (a == GESTURE_DO_HOLD && button) lis_start(GESTURE_INPUT_NAMES[in], true, false, NULL);   /* link_audio_hold(true) */
    else if (a == GESTURE_DO_TALK || a == GESTURE_DO_HOLD) lis_start(GESTURE_INPUT_NAMES[in], false, false, NULL);
    else if (a == GESTURE_DO_STOP) lis_stop("tap");
}
static void screen_do(gesture_taps_t presses) {
    if (presses == GESTURE_TAPS_SINGLE) mole_fluid_screen(!mole_fluid_screen_on());
    else if (presses == GESTURE_TAPS_DOUBLE) { s_dnd = !s_dnd; ws_emit(link_evt_dnd_frame(s_dnd)); }
}
static void spot_do(unsigned spot, uint32_t now) {
    if (!gesture_limit_allow(&s_tap_l, now)) return;
    char id[PKG_STR_MAX + 1], open[PKG_KEY_MAX + 1];
    mole_fluid_spot_id(spot, id, sizeof id, open, sizeof open);
    cJSON *ev = link_evt_tap_frame((s_tap_xy >> 16) / 65535.0, (s_tap_xy & 0xFFFFu) / 65535.0), *data = cJSON_GetObjectItemCaseSensitive(ev, "data");
    if (data && id[0]) cJSON_AddStringToObject(data, "spot", id);
    if (data && open[0]) cJSON_AddStringToObject(data, "opened", open);
    if (data && spot == MOLE_SPOT_BACK) cJSON_AddBoolToObject(data, "closed", true);
    ws_emit(ev);
}
static void taps_do(gesture_taps_t taps, uint32_t now) {
    if (taps == GESTURE_TAPS_NONE || !gesture_limit_allow(&s_tap_l, now)) return;
    const double x = (s_tap_xy >> 16) / 65535.0, y = (s_tap_xy & 0xFFFFu) / 65535.0;
    if (taps == GESTURE_TAPS_DOUBLE) gesture_do(GESTURE_IN_DOUBLE_TAP, link_evt_double_tap_frame(x, y));
    else gesture_do(GESTURE_IN_TAP, link_evt_tap_frame(x, y));
}

/* A shake as the IMU reports it: one axis swinging +-2.4 g at 4 Hz for 0.9 s, a sample every 20 ms (the board's
 * cache), into gesture.c's own detector. */
static void imu_feed(uint32_t now) {
    if (!s_imu_at || now - s_imu_at > 1000u) s_imu_at = now;   /* (the first frame, or the screen asleep a while) */
    for (; s_imu_at + 20u <= now; s_imu_at += 20u) {
        const uint32_t t = s_imu_at + 20u;
        const float u = s_shake_at && t - s_shake_at < 900u ? (float)(t - s_shake_at) / 1000.f : -1.f;
        const float ax = u >= 0.f ? 2.4f * sinf(u * 6.2831853f * 4.f) : 0.f;
        gesture_shake_t sh;
        if (!pairing() && gesture_feed(&s_g, ax, 0.f, 1.f, t, &sh)) {
            mole_fluid_event(MOLE_FLUID_SHAKE);   /* reacts with or without an agent */
            /* shake to talk (on out of the box): link_audio_shake starts the listen and sends the event naming it */
            if (gesture_limit_allow(&s_shake_l, t)) {
                if (mole_fluid_shake_talk() && !s_lis.on) lis_start(NULL, false, true, &sh);
                else ws_emit(link_evt_shake_frame(sh.peak_g, sh.reversals, sh.ms, NULL, NULL, false));
            }
        }
    }
}

static int s_wheel_ticks;              /* the motor's ticks a wheel asked for, not yet taken (wm_wheel_ticks) */
static int s_tick_ms, s_tick_strength; /* ... the last one's plan (haptic_plan_wheel) */
static uint32_t s_tick_at;

/* sense.c sense_task, one pass (the device: every 10 ms; here once a frame) */
static void sense_tick(uint32_t now) {
    const bool pair = pairing();
    imu_feed(now);
    if (pair) gesture_reset(&s_g);
    if (s_lis.on && s_voice_in >= 0.05f) s_lis.speech_ms += (uint32_t)(s_fi[FI_FRAME_MS] + 0.5f);   /* the VAD hears it */
    const bool beat = !pair && !s_q.open && rhythm_active();
    for (int b = 0; b < 2; b++) {
        const bool on = s_btn[b];
        if (beat) {                    /* sense.c: a rhythm shows: accept plays and stops it, reject stops it and leaves */
            if (on && !s_btn_prev[b] && gesture_limit_allow(&s_btn_l, now)) { if (b == MOLTY_BTN_ACCEPT) rhythm_local_play(); else rhythm_local_stop(); }
            s_presser[b].pending = false; gesture_switch_reset(&s_sw[b]); s_sw[b].down = on;
            s_btn_prev[b] = on;
            continue;
        }
        const gesture_input_t in = b == MOLTY_BTN_ACCEPT ? GESTURE_IN_BUTTON_ACCEPT : GESTURE_IN_BUTTON_REJECT;
        const bool screen = gesture_binds_get(mole_fluid_gestures(), in) == GESTURE_DO_SCREEN && !s_q.open;
        const gesture_sw_t click = gesture_switch_feed(&s_sw[b], on, now);
        if (pair || !screen) { s_presser[b].pending = false; gesture_switch_reset(&s_sw[b]); }
        else if (click == GESTURE_SW_CLICK) screen_do(gesture_tapper_feed(&s_presser[b], now, true));
        else if (click == GESTURE_SW_HOLD) { s_presser[b].pending = false; mole_fluid_goodbye(); }   /* power_off */
        if (on && !s_btn_prev[b] && pair) {   /* link_pair.c: the accept / reject buttons decide a pairing */
            mole_fluid_ask_pick(b == MOLTY_BTN_ACCEPT ? 0 : 1); s_q.open = false; answered(b == MOLTY_BTN_ACCEPT ? 0 : 1, NULL);
        } else if (on && !s_btn_prev[b] && !screen) {
            mole_fluid_event(MOLE_FLUID_BUTTON);
            if (gesture_limit_allow(&s_btn_l, now)) {   /* button_do: a pending question keeps its meaning */
                cJSON *ev = link_evt_button_frame(b == MOLTY_BTN_ACCEPT);
                if (s_q.open) ws_emit(ev); else gesture_do(in, ev);
            }
        }
        screen_do(gesture_tapper_poll(&s_presser[b], now));
        if (!on && s_btn_prev[b] && s_lis.on) lis_stop("tap");   /* link_audio_hold(false): let go, a held talk sends */
        s_btn_prev[b] = on;
    }
    const bool guarded = gesture_tap_guarded(&s_g, now);
    if (guarded || pair) s_tapper.pending = false;
    if (s_tap_flag && !pair) {
        s_tap_flag = false;
        switch (gesture_tap_route(s_lis.on, now - s_lis.t0, true, false, guarded)) {
        case GESTURE_TAP_STOP_LISTEN: lis_stop("tap"); break;
        case GESTURE_TAP_DROP_LISTEN: case GESTURE_TAP_CANCEL_SAY: lis_stop("cancelled"); break;
        case GESTURE_TAP_GESTURE:
            taps_do(gesture_tapper_feed(&s_tapper, now, gesture_binds_get(mole_fluid_gestures(), GESTURE_IN_DOUBLE_TAP) != GESTURE_DO_NONE), now);
            break;
        case GESTURE_TAP_IGNORE: break;
        }
    }
    s_tap_flag = false;
    taps_do(gesture_tapper_poll(&s_tapper, now), now);
    const unsigned spot = s_spot;
    s_spot = 0;
    if (spot && !pair) spot_do(spot, now);
    mole_scene_evt_t sv;               /* the screen changed on the device itself: the relay hears what is up */
    if (mole_fluid_scene_event(&sv)) ws_emit(link_evt_scene_frame(sv.pkg, sv.layout, sv.why, sv.spot));
    {   /* sense.c: a wheel's clicks, its own tick, one at most every HAPTIC_WHEEL_GAP_MS */
        int ms, strength;
        haptic_plan_t pl;
        if (mole_fluid_take_ticks(&ms, &strength) && haptic_wheel_due(&s_tick_at, now) && haptic_plan_wheel(&pl, ms, strength)) {
            s_wheel_ticks++; s_tick_ms = pl.seg[0]; s_tick_strength = pl.strength;
        }
    }
    mole_ring_evt_t re;                /* a wheel settled */
    if (mole_fluid_ring_event(&re)) ws_emit(link_evt_ring_frame(re.spot, re.value, re.index, re.label, re.id));
    rhythm_ev_t rv;                    /* the beat player's own: a mute, a pause, a stop */
    while (rhythm_take_event(&rv)) ws_emit(rhythm_event_frame(&rv));
    if (s_dl.on) {                     /* pkg_cmd.c download_task: the ring follows the bytes, then full and into the water */
        const float got = (float)(now - s_dl.t0) / (float)(s_dl.ms ? s_dl.ms : 1u);
        if (got < 1.f) mole_fluid_progress(DL_RING_PCT * got, false);
        else { s_dl.on = false; mole_fluid_progress(100.f, true); pkg_evt("installed", "version", s_qa.version); }
    }
}

/** The motor's ticks a wheel asked for since the last call (sense.c: one a click, one at most every 60 ms); ms and
 *  strength (non-NULL): the last one's. */
WM_API int wm_wheel_ticks(int *ms, int *strength) {
    const int n = s_wheel_ticks;
    s_wheel_ticks = 0;
    if (ms) *ms = s_tick_ms;
    if (strength) *strength = s_tick_strength;
    return n;
}

/** The device's own handling of its inputs on (the simulator): a tap, a double tap, the buttons and a shake do what
 *  sense.c does with them, and what an agent would hear piles up for wm_events. Off (the website's screens): a tap is
 *  only the finger (wm_touch). */
WM_API void wm_sense(int on) { s_sense = on != 0; if (s_sense) gesture_init(&s_g, NULL); }
/** What an agent would hear since the last call: a JSON array of the device's evt frames, oldest first; the caller frees
 *  it (wm_free). NULL: nothing. */
WM_API char *wm_events(void) {
    if (!s_evts || !cJSON_GetArraySize(s_evts)) return NULL;
    char *out = cJSON_PrintUnformatted(s_evts);
    cJSON_Delete(s_evts); s_evts = NULL;
    return out;
}
/** A button (0 accept, 1 reject) is down or up. */
WM_API void wm_button(int b, int down) { if (b == 0 || b == 1) s_btn[b] = down != 0; }
/** The device is shaken (its IMU feels it over the next 0.9 s). */
WM_API void wm_shake_imu(void) { s_shake_at = (uint32_t)(s_clock / 1000); }
/** The agent's listen (link `listen`): on, and off as its turn ends (the device's end of turn: `turn_end`). */
WM_API void wm_talk(int on) { if (on) lis_start(NULL, false, false, NULL); else lis_stop("turn_end"); }
/** Do not disturb (POST /api/power {dnd}). */
WM_API void wm_dnd(int on) { if (s_dnd != (on != 0)) { s_dnd = on != 0; ws_emit(link_evt_dnd_frame(s_dnd)); } if (s_up) mole_fluid_dnd(s_dnd); }
/** The screen off or on (POST /api/power {screen}). Power off: the goodbye. */
WM_API void wm_screen(int on) { if (s_up) mole_fluid_screen(on != 0); }
WM_API void wm_goodbye(void) { if (s_up) mole_fluid_goodbye(); }
/** Progress (link `progress`): pct 0..100 (< 0: no percent), done; stop: it lets go where it got to. */
WM_API void wm_progress(float pct, int done) { if (s_up) mole_fluid_progress(pct, done != 0); }
WM_API void wm_progress_stop(void) { if (s_up) mole_fluid_progress_stop(); }
/** A question with what its answer does: wm_ask's JSON plus "kind" ("ask": the agent's, an `answer` event; "pair": a
 *  pairing, its pair.result; "install": a package's install, its download and `pkg` events), "job" and, for an install,
 *  "id", "version" and "ms" (the download's length). 0, or -1. */
WM_API int wm_question(const char *json) {
    cJSON *a = cJSON_Parse(json);
    const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "kind")),
               *job = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "job")),
               *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "id")),
               *ver = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "version"));
    const cJSON *opts = cJSON_GetObjectItemCaseSensitive(a, "options"), *ms = cJSON_GetObjectItemCaseSensitive(a, "ms");
    if (s_q.open) answered(-1, "cancelled");   /* link_ask.c: a pending question is cancelled first */
    memset(&s_qa, 0, sizeof s_qa);
    s_qa.kind = kind && !strcmp(kind, "pair") ? Q_PAIR : kind && !strcmp(kind, "install") ? Q_INSTALL : Q_ASK;
    snprintf(s_qa.job, sizeof s_qa.job, "%s", job ? job : "");
    snprintf(s_qa.name, sizeof s_qa.name, "%s", id ? id : "");
    snprintf(s_qa.version, sizeof s_qa.version, "%s", ver ? ver : "");
    for (int i = 0; i < cJSON_GetArraySize(opts) && i < FLUID_ASK_MAX; i++)
        snprintf(s_qa.label[i], sizeof s_qa.label[i], "%s", cJSON_GetStringValue(cJSON_GetArrayItem(opts, i)) ? cJSON_GetStringValue(cJSON_GetArrayItem(opts, i)) : "");
    s_dl.ms = cJSON_IsNumber(ms) ? (uint32_t)ms->valuedouble : 3000u;
    cJSON_Delete(a);
    return wm_ask(json);
}
/** The question ends unanswered (cancelled, a timeout): its answer event says so. */
WM_API void wm_question_end(const char *reason) {
    if (!s_up || !s_q.open) return;
    wm_ask_end();
    answered(-1, reason && reason[0] ? reason : "cancelled");
}
/** scene.play (queue 0) or scene.queue (1) of the package in hand. 0, or -1. */
WM_API int wm_scene_op(const char *layout, const char *params, float ttl_s, int queue) {
    if (!queue) {                      /* it takes the screen: a question on it is cancelled (pkg_cmd.c) */
        const bool asked = s_q.open, ok = wm_scene(layout, params, ttl_s) == 0;
        if (ok && asked) answered(-1, "cancelled");
        return ok ? 0 : -1;
    }
    if (!s_up || !s_pkg) return -1;
    cJSON *pj = params ? cJSON_Parse(params) : NULL;
    const char *err = NULL;
    scene_t *sc = (!params || cJSON_IsObject(pj)) ? scene_build(s_pkg, layout, pj, ttl_s, &err) : NULL;
    cJSON_Delete(pj);
    if (!sc) return -1;
    if (mole_fluid_scene(MOLE_SCENE_QUEUE, sc) != ESP_OK) { scene_free(sc); return -1; }
    return 0;
}

/* ── lab: the firmware's experiment switches, a line each: its name, its setter and getter, its default, low, high ── */
static const struct { const char *name; void (*set)(int); int (*get)(void); int def, lo, hi; } LAB[] = {
    { "scene_dust", mole_fluid_scene_dust, fluid_fine_dust_kind_get, 0, 0, 3 },
    { "scene_hard_pct", mole_fluid_scene_hard, fluid_fine_hard_density_get, 1, 0, 25 },
};
#define LAB_N ((int)(sizeof LAB / sizeof LAB[0]))
/** How many switches; switch i's name; its value now, default, low and high (4 ints); set it (not kept across a
 *  restart, as on the device). */
WM_API int wm_lab_n(void) { return LAB_N; }
WM_API const char *wm_lab_name(int i) { return i >= 0 && i < LAB_N ? LAB[i].name : ""; }
WM_API const int *wm_lab_range(int i) {
    static int r[4];
    if (i >= 0 && i < LAB_N) { r[0] = LAB[i].get(); r[1] = LAB[i].def; r[2] = LAB[i].lo; r[3] = LAB[i].hi; }
    return r;
}
WM_API void wm_lab_set(int i, int v) { if (s_up && i >= 0 && i < LAB_N) LAB[i].set(v); }

/** How many pixels of the panel differ from the look drawn whole now (the drawer's own full draw, as /api/framebuffer
 *  takes it): 0 when every change was sent. -1 during a change of picture (its old picture is meant to show). For the
 *  checks of device mode. */
static int s_off_at[16];
WM_API int wm_glass_off(void) {
    static uint16_t *ref;
    if (!s_up || !s_draw || !s_frame) return 0;
    if (fluid_draw_morphing(s_draw)) return -1;
    if (!ref && !(ref = malloc((size_t)s_px * (size_t)s_px * sizeof(uint16_t)))) return 0;
    const fluid_rect_t t = { ref, s_px, 0, 0, s_px, s_px };
    fluid_draw_rect(s_draw, &s_dopts, &t, true);
    fluid_draw_overlay(s_draw, &t, s_dopts.palette, &s_kept.ov);
    int n = 0;
    for (int q = 0; q < s_px * s_px; q++)
        if (s_rgba_of[ref[q]] != s_rgba[q]) { if (n < 8) { s_off_at[2 * n] = q % s_px; s_off_at[2 * n + 1] = q / s_px; } n++; }
    return n;
}
/** Where the first 8 of those pixels are (x, y each). */
WM_API const int *wm_glass_off_at(void) { return s_off_at; }
/** Device mode's CPU factor, other than DEV_CPU_X (a slower or faster computer, a check of the firmware under time
 *  pressure). <= 0: back to DEV_CPU_X. */
WM_API void wm_device_cpu(double x) { if (s_dev) web_sdk_cpu(x > 0. ? x : DEV_CPU_X); }
