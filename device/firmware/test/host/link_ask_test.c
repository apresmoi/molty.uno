/* link_ask.c + sense.c's touch path on the host: the `ask` command, its answer / timeout /
 * cancel events (exact frames), the screen calls, and that a tap answering a pending
 * question is not also a `tap` event. */
#include "link_sdk_stub.h"
typedef struct { float ax, ay, az, gx, gy, gz; uint32_t ms; } molty_imu_sample_t;
typedef uint32_t TickType_t;
bool molty_imu_get(molty_imu_sample_t *out);
int molty_display_width(void);
int molty_display_height(void);
TickType_t xTaskGetTickCount(void);
int xTaskDelayUntil(TickType_t *wake, TickType_t period);
#include "../../main/link/link_ask.c"
#include "../../main/sense/sense.c"
#include <assert.h>
#include <stdio.h>

/* boundaries */
bool molty_imu_get(molty_imu_sample_t *out) { (void)out; return false; }
int molty_display_width(void) { return 412; }
int molty_display_height(void) { return 412; }
TickType_t xTaskGetTickCount(void) { return 0; }
int xTaskDelayUntil(TickType_t *wake, TickType_t period) { (void)wake; (void)period; return 1; }
static bool pairing; static int pair_taps;
bool link_pair_active(void) { return pairing; }
void link_pair_tap(float dx, float dy) { (void)dx; (void)dy; pair_taps++; }
void mole_fluid_event(mole_fluid_event_t ev) { (void)ev; }
static uint32_t binds; static int talks, stops;
uint32_t mole_fluid_gestures(void) { return binds; }
static bool screen_on = true, dnd;
void mole_fluid_screen(bool on) { screen_on = on; }
bool mole_fluid_screen_on(void) { return screen_on; }
bool power_dnd(void) { return dnd; }
void power_dnd_set(bool on) { dnd = on; }
void power_off(void) {}
bool haptic_quiet(void) { return false; }      /* (no motor: the shake detector as ever) */
static char cues[64];                            /* the device's own buzzes (haptic_cue), in order */
esp_err_t haptic_cue(const char *name) {
    if (strlen(cues) + strlen(name) + 2 > sizeof cues) cues[0] = 0;   /* (only the recent ones are looked at) */
    strcat(strcat(cues, cues[0] ? " " : ""), name); return ESP_OK;
}
esp_err_t haptic_touch(void) { return haptic_cue("touch"); }   /* the touch feedback, among them */
esp_err_t haptic_wheel(int ms, int strength) { (void)ms; (void)strength; return haptic_cue("wheel"); }
bool haptic_wheel_due(uint32_t *last_ms, uint32_t now_ms) { *last_ms = now_ms; return true; }
bool link_audio_gesture_talk(const char *input) { (void)input; talks++; return true; }
static int holds, releases;
void link_audio_hold(bool down, const char *input) { (void)input; if (down) holds++; else releases++; }
void link_audio_stop_any(void) { stops++; }
static bool listening;
bool link_audio_listening(uint32_t *age) { (void)age; return listening; }
static unsigned spot_hit;       /* the spot a tap lands on (mole_fluid.c), 0 none */
unsigned mole_fluid_spot_tap(int px, int py) { (void)px; (void)py; return spot_hit; }
static const char *spot_opens = "";   /* the layout that spot opens */
void mole_fluid_spot_id(unsigned spot, char *id, size_t size, char *open, size_t open_size) {
    const bool on = spot && spot != MOLE_SPOT_BACK;
    snprintf(id, size, "%s", on ? "mars" : "");
    snprintf(open, open_size, "%s", on ? spot_opens : "");
}
bool mole_fluid_scene_event(mole_scene_evt_t *out) { (void)out; return false; }
static int zone_hit;            /* the press lands on a scene's wheel (1), its centre (2), a beat's dot (3); 0 none */
int mole_fluid_zone(int px, int py) { (void)px; (void)py; return zone_hit; }
int mole_fluid_take_ticks(int *ms, int *strength) { *ms = *strength = 0; return 0; }
bool mole_fluid_ring_event(mole_ring_evt_t *out) { (void)out; return false; }
bool rhythm_active(void) { return false; }
void rhythm_local_play(void) { }
void rhythm_local_stop(void) { }
bool rhythm_take_event(rhythm_ev_t *e) { (void)e; return false; }
cJSON *rhythm_event_frame(const rhythm_ev_t *e) { (void)e; return NULL; }
bool link_audio_say_waiting(void) { return false; }
uint32_t link_audio_job_gen(void) { return 0; }
void link_audio_request(uint32_t gen, link_audio_req_t what) { (void)gen; (void)what; }
void link_audio_cancel(void) { }
bool link_audio_held(void) { return false; }
void mole_fluid_set_status(int status) { (void)status; }
void mole_fluid_listen(bool on) { (void)on; }
void mole_fluid_listen_color(int32_t rgb) { (void)rgb; }
int fd_status_from(bool wifi, bool relay, bool auth) { (void)wifi; (void)relay; (void)auth; return 0; }
void link_client_health(bool *w, bool *r, bool *a) { *w = *r = true; *a = false; }
static bool connected = true;
bool link_client_connected(void) { return connected; }
static char frames[16][256]; static unsigned nframes;
esp_err_t link_send_json(const cJSON *j) {
    char *t = cJSON_PrintUnformatted(j); assert(t && strlen(t) < sizeof(frames[0]) && nframes < 16);
    strcpy(frames[nframes++], t); cJSON_free(t); return ESP_OK;
}
/* the screen (mole_fluid.c): what the link wants (want_*) reaches the panel only when a
 * frame is drawn — glass = the question a tap on the panel may answer (mole_fluid_ask_shown) */
static int shown_n = -1, picked = -1; static unsigned shows, ends; static char shown[5][15], shown_title[41], shown_detail[37];
static int32_t shown_rgb[5];          /* the colours handed to the screen */
static unsigned want_seq, glass; static bool want_up;
esp_err_t mole_fluid_ask_show(const fluid_ask_q_t *q, unsigned *seq) {
    shows++; shown_n = q->n; picked = -1;
    snprintf(shown_title, sizeof(shown_title), "%s", q->title); snprintf(shown_detail, sizeof(shown_detail), "%s", q->detail);
    for (int i = 0; i < q->n; i++) { snprintf(shown[i], sizeof(shown[i]), "%s", q->label[i]); shown_rgb[i] = (int32_t)q->rgb[i]; }
    *seq = ++want_seq; want_up = true;
    return ESP_OK;
}
void mole_fluid_ask_pick(int index) { picked = index; want_up = false; }
void mole_fluid_ask_end(void) { ends++; want_up = false; }
unsigned mole_fluid_ask_shown(void) { return glass; }
/* the panel catches up: the wanted question, condensed (fluid_ask_tappable) */
static void draw(void) { glass = want_up ? want_seq : 0; }

/* the firmware's own question (link_ask_open): its end goes to the owner, not the wire */
static int done_index = -9, dones; static char done_reason[16];
static void on_done(int index, const char *reason) {
    dones++; done_index = index; snprintf(done_reason, sizeof(done_reason), "%s", reason ? reason : "");
}
static const char *const UL[2] = { "update", "later" };
/* the command only: the panel still shows what it showed */
static esp_err_t ask_cmd(const char *args) {
    cJSON *a = cJSON_Parse(args), *out = NULL; assert(a);
    esp_err_t e = link_ask_cmd(a, &out);
    if (e == ESP_OK) assert(out && !strcmp(cJSON_GetObjectItem(out, "job")->valuestring, cJSON_GetObjectItem(a, "job")->valuestring));
    else assert(!out);
    cJSON_Delete(a); cJSON_Delete(out);
    return e;
}
/* the command, then the panel shows its question */
static esp_err_t ask(const char *args) { esp_err_t e = ask_cmd(args); draw(); return e; }
static const int32_t UC[2] = { 0x3ccf96, 0xf1f3f5 };
static esp_err_t open_q(const char *job, unsigned timeout_ms) { esp_err_t e = link_ask_open(job, "update molty?", NULL, UL, UC, 2, timeout_ms, on_done); draw(); return e; }
/* a finger down then up at panel px (x, y) */
static void touch(int x, int y) {
    uint16_t xs[1] = { (uint16_t)x }, ys[1] = { (uint16_t)y };
    sense_touch(1, xs, ys); sense_touch(0, xs, ys);
}
static const char *last(void) { return nframes ? frames[nframes - 1] : ""; }

#define YNI "{\"job\":\"j1\",\"options\":[\"Yes\",\" no \",\"idk\"],\"timeout_ms\":5000}"

/* A tap answers only the question the panel showed when it landed: screen updates are
 * async, so right after ask B replaced ask A the finger may still be on A's labels. */
static void test_displayed(void) {
    sdk_now = 50000000;
    assert(ask("{\"job\":\"A\",\"options\":[\"yes\",\"no\"],\"timeout_ms\":5000}") == ESP_OK);
    unsigned n0 = nframes;
    /* B replaces A; the panel has not drawn B yet: a tap on A's "no" (right half) answers
     * nothing — not B, not a `tap` event — and B stays pending */
    assert(ask_cmd("{\"job\":\"B\",\"options\":[\"red\",\"green\"],\"timeout_ms\":5000}") == ESP_OK);
    assert(nframes == n0 + 1 && strstr(last(), "\"job\":\"A\"") && strstr(last(), "cancelled"));
    touch(206 + 100, 206);
    assert(!atomic_load(&s_tap));
    link_ask_tick();
    assert(link_ask_pending() && nframes == n0 + 1 && picked == -1);
    /* B's letters still morphing out of A's (not condensed: nothing tappable) */
    glass = 0;
    touch(206 + 100, 206); link_ask_tick();
    assert(link_ask_pending() && nframes == n0 + 1 && !atomic_load(&s_tap));
    /* B on the panel: the next tap is B's answer */
    draw();
    touch(206 + 100, 206); link_ask_tick();
    assert(!link_ask_pending() && picked == 1);
    assert(!strcmp(last(), "{\"t\":\"evt\",\"name\":\"answer\",\"data\":{\"job\":\"B\",\"index\":1,\"label\":\"green\"}}"));
    /* the firmware's own question replacing an agent's: same rule, its owner hears nothing
     * from a tap aimed at the old labels */
    assert(ask("{\"job\":\"C\",\"options\":[\"a\",\"b\"],\"timeout_ms\":5000}") == ESP_OK);
    int d0 = dones;
    assert(link_ask_open("u8", NULL, NULL, UL, UC, 2, 5000, on_done) == ESP_OK);
    touch(206 - 100, 206); link_ask_tick();
    assert(dones == d0 && link_ask_pending());
    draw(); touch(206 - 100, 206); link_ask_tick();
    assert(dones == d0 + 1 && done_index == 0);
    /* and an agent's ask replacing the firmware's: a tap on "update" is not its answer */
    assert(open_q("u9", 5000) == ESP_OK);
    n0 = nframes; d0 = dones;
    assert(ask_cmd("{\"job\":\"D\",\"options\":[\"x\",\"y\"],\"timeout_ms\":5000}") == ESP_OK);
    assert(dones == d0 + 1 && !strcmp(done_reason, "cancelled"));
    touch(206 - 100, 206); link_ask_tick();
    assert(link_ask_pending() && nframes == n0);
    link_ask_cancel();
}

/* The deadline judges the tap's capture time, not when the link task gets to it. */
static void test_deadline(void) {
    sdk_now = 60000000;
    unsigned n0 = nframes;
    /* captured exactly at the deadline, drained later: the tap wins */
    assert(ask("{\"job\":\"E\",\"options\":[\"a\",\"b\"],\"timeout_ms\":1000}") == ESP_OK);
    sdk_now += 1000000; touch(206 + 100, 206);
    sdk_now += 30000; link_ask_tick();
    assert(!link_ask_pending() && nframes == n0 + 1);
    assert(!strcmp(last(), "{\"t\":\"evt\",\"name\":\"answer\",\"data\":{\"job\":\"E\",\"index\":1,\"label\":\"b\"}}"));
    /* captured 1 µs after it (the tick ran late): the timeout wins, the tap answers nothing */
    assert(ask("{\"job\":\"F\",\"options\":[\"a\",\"b\"],\"timeout_ms\":1000}") == ESP_OK);
    sdk_now += 1000001; touch(206 + 100, 206);
    link_ask_tick();
    assert(!link_ask_pending() && nframes == n0 + 2);
    assert(!strcmp(last(), "{\"t\":\"evt\",\"name\":\"answer\",\"data\":{\"job\":\"F\",\"index\":null,\"reason\":\"timeout\"}}"));
    /* the firmware's own question: the same */
    int d0 = dones;
    assert(open_q("u10", 1000) == ESP_OK);
    sdk_now += 1000000; touch(206 - 100, 206); sdk_now += 30000; link_ask_tick();
    assert(dones == d0 + 1 && done_index == 0);
    assert(open_q("u11", 1000) == ESP_OK);
    sdk_now += 1000001; touch(206 - 100, 206); link_ask_tick();
    assert(dones == d0 + 2 && done_index == -1 && !strcmp(done_reason, "timeout"));
}

/* The device's own buzzes: a question appears (double), an answer is taken (touch); nothing for a tap
 * beside the answers, a timeout, a cancel. */
static void test_cues(void) {
    sdk_now = 70000000; cues[0] = 0;
    assert(ask("{\"job\":\"G\",\"options\":[\"a\",\"b\"],\"timeout_ms\":1000}") == ESP_OK);
    assert(!strcmp(cues, "double"));
    touch(206, 206); link_ask_tick();                  /* the centre: not an answer */
    assert(link_ask_pending() && !strcmp(cues, "double"));
    touch(206 + 100, 206); link_ask_tick();
    assert(!link_ask_pending() && !strcmp(cues, "double touch"));
    cues[0] = 0;
    assert(ask("{\"job\":\"H\",\"options\":[\"a\",\"b\"],\"timeout_ms\":1000}") == ESP_OK);
    sdk_now += 1000001; link_ask_tick();               /* timed out: no tick */
    assert(!link_ask_pending() && !strcmp(cues, "double"));
    assert(open_q("u12", 1000) == ESP_OK && !strcmp(cues, "double double"));   /* the firmware's own question too */
    link_ask_cancel();
    assert(!strcmp(cues, "double double"));
}

/* A tap on a scene's spot is the spot's (its reading shows), not a plain tap; a question's
 * labels and a recording come first */
static void test_spot(void) {
    nframes = 0;
    atomic_store(&s_tap, false); atomic_store(&s_spot, 0);
    spot_hit = 0x105;
    touch(100, 100);
    assert(atomic_load(&s_spot) == 0x105 && !atomic_load(&s_tap));
    atomic_store(&s_spot, 0);
    listening = true;                       /* a tap that stops a recording does that */
    touch(100, 100);
    assert(!atomic_load(&s_spot) && atomic_load(&s_tap));
    listening = false; atomic_store(&s_tap, false);
    spot_hit = 0;
    touch(100, 100);
    assert(!atomic_load(&s_spot) && atomic_load(&s_tap));
    atomic_store(&s_tap, false);
    /* a press on a scene's wheel, its centre or a beat's dot is the fluid's: never a spot or a tap; the centre and a dot
     * buzz as a tap that does something, the wheel does not (its clicks tick) */
    spot_hit = 0x105;
    for (zone_hit = 1; zone_hit <= 3; zone_hit++) {
        cues[0] = 0; sdk_now += 1000000;
        touch(100, 100);
        assert(!atomic_load(&s_spot) && !atomic_load(&s_tap) && (zone_hit == 1 ? !cues[0] : !strcmp(cues, "touch")));
    }
    zone_hit = 0; spot_hit = 0;
    /* the agent hears which spot, whatever tap is bound to */
    const unsigned n0 = nframes;
    spot_do(0x105, sdk_now / 1000, &(gesture_limit_t){ .min_ms = 250 });
    assert(nframes == n0 + 1 && strstr(last(), "\"name\":\"tap\"") && strstr(last(), "\"spot\":\"mars\"") && !strstr(last(), "opened") && !strstr(last(), "closed"));
    /* a spot that opens a layout on Molty itself: the agent hears that it did */
    spot_opens = "sign-closeup";
    spot_do(0x105, sdk_now / 1000, &(gesture_limit_t){ .min_ms = 250 });
    assert(nframes == n0 + 2 && strstr(last(), "\"spot\":\"mars\"") && strstr(last(), "\"opened\":\"sign-closeup\""));
    spot_opens = "";
    /* a tap beside the spots of a layout so opened is Molty's too (it goes back): no plain tap, the agent hears `closed` */
    spot_hit = MOLE_SPOT_BACK;
    touch(100, 100);
    assert(atomic_load(&s_spot) == MOLE_SPOT_BACK && !atomic_load(&s_tap));
    atomic_store(&s_spot, 0); spot_hit = 0;
    spot_do(MOLE_SPOT_BACK, sdk_now / 1000, &(gesture_limit_t){ .min_ms = 250 });
    assert(nframes == n0 + 3 && strstr(last(), "\"name\":\"tap\"") && strstr(last(), "\"closed\":true") && !strstr(last(), "spot"));
}

/* Touch feedback (sense.c's acted): a tap that does something buzzes once, at once; the water and the buttons never. */
static void test_haptics(void) {
    uint16_t xs[1] = { 100 }, ys[1] = { 100 };
    sdk_now = 200000000; cues[0] = 0; spot_hit = 0; listening = false;
    touch(100, 100); assert(!cues[0]);                                     /* the water (a ripple, a miss): none */
    atomic_store(&s_tap, false);
    sdk_now += 1000000; spot_hit = 0x105;
    touch(100, 100); assert(!strcmp(cues, "touch"));                        /* a planet, a sign: one */
    sdk_now += 1000000; spot_hit = MOLE_SPOT_BACK;
    touch(100, 100); assert(!strcmp(cues, "touch touch"));                   /* out of a close-up: one */
    sdk_now += 1000000; spot_hit = 0x105; cues[0] = 0;                     /* one press held and dragged, the panel repeating it: one */
    for (int i = 0; i < 20; i++) { xs[0] = (uint16_t)(100 + i * 5); sense_touch(1, xs, ys); sdk_now += 16000; }
    sense_touch(0, xs, ys); assert(!strcmp(cues, "touch"));
    sdk_now += 1000000; cues[0] = 0;                                       /* the panel's bounce (up and down in 30 ms): one */
    sense_touch(1, xs, ys); sense_touch(0, xs, ys); sdk_now += 30000; sense_touch(1, xs, ys); sense_touch(0, xs, ys);
    assert(!strcmp(cues, "touch"));
    sdk_now += 120000; touch(100, 100); assert(!strcmp(cues, "touch touch"));   /* two quick separate taps: two */
    sdk_now += 1000000; cues[0] = 0; listening = true;
    touch(100, 100); assert(!cues[0]);                                     /* a tap that stops a recording: the mic's */
    listening = false; atomic_store(&s_tap, false); atomic_store(&s_spot, 0);
    /* a question's answer: one (link_ask), even over a spot */
    sdk_now += 1000000;
    assert(ask("{\"job\":\"T\",\"options\":[\"a\",\"b\"],\"timeout_ms\":5000}") == ESP_OK);
    cues[0] = 0; touch(206 + 100, 206); link_ask_tick();
    assert(!link_ask_pending() && !strcmp(cues, "touch"));
    spot_hit = 0;
    /* the buttons click by themselves: never a buzz, whatever they are bound to or do */
    binds = gesture_binds_default();
    const gesture_action_t acts[] = { GESTURE_DO_EVENT, GESTURE_DO_STOP, GESTURE_DO_NONE, GESTURE_DO_TALK, GESTURE_DO_HOLD, GESTURE_DO_SCREEN };
    for (int i = 0; i < 6; i++) {
        sdk_now += 1000000; cues[0] = 0;
        binds = gesture_binds_set(binds, GESTURE_IN_BUTTON_ACCEPT, acts[i]);
        button_do(true); assert(!cues[0]);
    }
    link_audio_hold(false, NULL);
    sdk_now += 1000000;
    assert(ask("{\"job\":\"U\",\"options\":[\"a\"],\"timeout_ms\":5000}") == ESP_OK);
    cues[0] = 0; button_do(true); assert(!cues[0]);                         /* a question's acknowledge */
    link_ask_cancel(); binds = gesture_binds_default();
    /* the screen switch: its clicks and its hold act, without a buzz */
    gesture_tapper_t pr = { 0 };
    screen_on = true; dnd = false;
    sdk_now += 1000000; cues[0] = 0;
    switch_do(GESTURE_SW_CLICK, &pr, 5000); assert(!cues[0] && screen_on);
    screen_do(gesture_tapper_poll(&pr, 5400)); assert(!screen_on && !cues[0]);
    switch_do(GESTURE_SW_HOLD, &pr, 6000); assert(!cues[0]);
    screen_on = true;
}

int main(void) {
    /* the buttons do what they are bound to; a pending question comes first */
    sdk_now = 90000000;
    binds = gesture_binds_default();
    button_do(true); assert(holds == 1 && talks == 0 && nframes == 0);       /* BOOT: held to talk out of the box */
    binds = gesture_binds_set(binds, GESTURE_IN_TAP, GESTURE_DO_HOLD);
    taps_do(GESTURE_TAPS_SINGLE, sdk_now, &(gesture_limit_t){ .min_ms = 250 }); assert(holds == 1 && talks == 1);   /* a tap cannot be held: talk */
    talks = 0;
    binds = gesture_binds_set(gesture_binds_default(), GESTURE_IN_BUTTON_REJECT, GESTURE_DO_TALK);
    binds = gesture_binds_set(binds, GESTURE_IN_BUTTON_ACCEPT, GESTURE_DO_EVENT);
    button_do(false); assert(talks == 1 && nframes == 0);
    button_do(true); assert(talks == 1 && nframes == 1 && strstr(last(), "\"button\""));   /* event: as bound */
    binds = gesture_binds_set(binds, GESTURE_IN_BUTTON_ACCEPT, GESTURE_DO_STOP);
    button_do(true); assert(stops == 1 && nframes == 1);
    binds = gesture_binds_set(binds, GESTURE_IN_BUTTON_ACCEPT, GESTURE_DO_NONE);
    button_do(true); assert(stops == 1 && nframes == 1);
    assert(ask(YNI) == ESP_OK);
    button_do(false); assert(talks == 1 && nframes == 2);                      /* an open question: an event, whatever is bound */
    link_ask_cancel(); nframes = 0; shows = 0; dones = 0; binds = gesture_binds_default();
    /* the screen switch (PWR out of the box): a press, once the double window passed, flips the
     * screen; a double press flips do not disturb and leaves the screen alone; no frame either way */
    assert(gesture_binds_get(binds, GESTURE_IN_BUTTON_REJECT) == GESTURE_DO_SCREEN);
    gesture_tapper_t pr = { 0 };
    screen_do(gesture_tapper_feed(&pr, 1000, true)); assert(screen_on && !dnd);
    screen_do(gesture_tapper_poll(&pr, 1200)); assert(screen_on);
    screen_do(gesture_tapper_poll(&pr, 1400)); assert(!screen_on && !dnd);
    screen_do(gesture_tapper_feed(&pr, 2000, true)); screen_do(gesture_tapper_feed(&pr, 2200, true));
    screen_do(gesture_tapper_poll(&pr, 3000)); assert(!screen_on && dnd);
    screen_do(gesture_tapper_feed(&pr, 4000, true)); screen_do(gesture_tapper_poll(&pr, 4400)); assert(screen_on && dnd);
    assert(nframes == 0);
    /* no touch: nothing could answer → unsupported, nothing on screen */
    sdk_caps &= ~MOLTY_CAP_TOUCH;
    assert(ask(YNI) == ESP_ERR_NOT_SUPPORTED && !shows && !link_ask_pending());
    sdk_caps = 0xFFFFFFFFu;
    /* bad args: refused, nothing shown */
    assert(ask("{\"job\":\"j1\",\"options\":[\"123456789012345\"],\"timeout_ms\":5000}") == ESP_ERR_INVALID_ARG && !shows);
    /* colours: optional, one "#rrggbb" per option; a bad one refuses the question */
    assert(ask("{\"job\":\"j1\",\"options\":[\"a\",\"b\"],\"colors\":[\"#3ccf96\"],\"timeout_ms\":5000}") == ESP_ERR_INVALID_ARG && !shows);
    assert(ask("{\"job\":\"j1\",\"options\":[\"a\"],\"colors\":[\"green\"],\"timeout_ms\":5000}") == ESP_ERR_INVALID_ARG && !shows);
    assert(ask("{\"job\":\"c1\",\"options\":[\"a\",\"b\"],\"colors\":[\"#3CCF96\",\"#000000\"],\"timeout_ms\":5000}") == ESP_OK);
    assert(shows == 1 && shown_rgb[0] == 0x3ccf96 && shown_rgb[1] == 0);
    assert(ask("{\"job\":\"c2\",\"options\":[\"a\",\"b\"],\"timeout_ms\":5000}") == ESP_OK);   /* older relay: defaults */
    assert(shows == 2 && shown_rgb[0] == 0x3ccf96 && shown_rgb[1] == 0xff6b6b);
    assert(open_q("c3", 5000) == ESP_OK && shown_rgb[0] == 0x3ccf96 && shown_rgb[1] == 0xf1f3f5);   /* the firmware's own */
    assert(!strcmp(shown_title, "update molty?") && !shown_detail[0]);
    /* the agent's title over the answers; the whole label at its count's limit */
    assert(ask("{\"job\":\"c4\",\"options\":[\"send it now\",\"keep a draft\"],\"title\":\"Send the summary to Ana?\",\"timeout_ms\":5000}") == ESP_OK);
    assert(!strcmp(shown_title, "Send the summary to Ana?") && !strcmp(shown[1], "keep a draft"));
    link_ask_cancel(); shows = 0; nframes = 0; dones = 0;

    /* an answer: the tap's rim arc picks the option (n = 3: lower left, bottom, lower right); the label goes back
     * exactly as received; no `tap` event for that tap */
    sdk_now = 1000000;
    assert(ask(YNI) == ESP_OK && link_ask_pending() && shows == 1 && shown_n == 3 && !strcmp(shown[1], " no "));
    assert(nframes == 0);
    touch(206 + 90, 206 - 60);                         /* on the question (the upper half): answers nothing, no tap either */
    assert(!atomic_load(&s_tap));
    link_ask_tick(); assert(link_ask_pending() && picked == -1);
    touch(206, 206); link_ask_tick(); assert(link_ask_pending());   /* the centre: nothing */
    touch(206, 206 + 150);                             /* the bottom arc */
    assert(!atomic_load(&s_tap));                      /* sense.c: no ordinary tap pending */
    link_ask_tick();
    assert(picked == 1 && !link_ask_pending() && nframes == 1);
    assert(!strcmp(last(), "{\"t\":\"evt\",\"name\":\"answer\",\"data\":{\"job\":\"j1\",\"index\":1,\"label\":\" no \"}}"));
    link_ask_tick(); assert(nframes == 1);              /* answered once */
    /* no question: a tap is a tap again */
    touch(100, 100); assert(atomic_exchange(&s_tap, false));

    /* a bad ask leaves the pending one up (no cancel), a second tap before the tick is
     * also swallowed and the first one answers */
    assert(ask("{\"job\":\"j2\",\"options\":[\"a\",\"b\"],\"timeout_ms\":5000}") == ESP_OK);
    assert(ask("{\"job\":\"j3\",\"options\":[],\"timeout_ms\":5000}") == ESP_ERR_INVALID_ARG && link_ask_pending() && nframes == 1);
    touch(206 - 100, 206); touch(206 + 100, 206);
    assert(!atomic_load(&s_tap));
    link_ask_tick();
    assert(!strcmp(last(), "{\"t\":\"evt\",\"name\":\"answer\",\"data\":{\"job\":\"j2\",\"index\":0,\"label\":\"a\"}}"));
    /* answers are not rate-limited like taps (≤ 4/s): another question answered 10 ms
     * later still reports */
    sdk_now += 10000;
    assert(ask("{\"job\":\"j4\",\"options\":[\"ok\"],\"timeout_ms\":5000}") == ESP_OK);
    touch(30, 380); link_ask_tick();
    assert(!strcmp(last(), "{\"t\":\"evt\",\"name\":\"answer\",\"data\":{\"job\":\"j4\",\"index\":0,\"label\":\"ok\"}}"));

    /* timeout: exactly timeout_ms after the command */
    unsigned n0 = nframes, e0 = ends;
    assert(ask("{\"job\":\"j5\",\"options\":[\"a\",\"b\",\"c\",\"d\"],\"timeout_ms\":1000}") == ESP_OK);
    sdk_now += 999999; link_ask_tick(); assert(link_ask_pending() && nframes == n0);
    sdk_now += 1; link_ask_tick();
    assert(!link_ask_pending() && ends == e0 + 1 && nframes == n0 + 1);
    assert(!strcmp(last(), "{\"t\":\"evt\",\"name\":\"answer\",\"data\":{\"job\":\"j5\",\"index\":null,\"reason\":\"timeout\"}}"));

    /* cancelled by another screen command; nothing to cancel the second time */
    assert(ask("{\"job\":\"j6\",\"options\":[\"a\"],\"timeout_ms\":5000}") == ESP_OK);
    link_ask_cancel();
    assert(!link_ask_pending() && ends == e0 + 2);
    assert(!strcmp(last(), "{\"t\":\"evt\",\"name\":\"answer\",\"data\":{\"job\":\"j6\",\"index\":null,\"reason\":\"cancelled\"}}"));
    n0 = nframes; link_ask_cancel(); assert(nframes == n0 && ends == e0 + 2);

    /* a new ask cancels the old one first (its event before anything else), and the new
     * labels replace the old on screen without melting back in between */
    assert(ask("{\"job\":\"j7\",\"options\":[\"a\",\"b\"],\"timeout_ms\":5000}") == ESP_OK);
    n0 = nframes; unsigned s0 = shows;
    assert(ask("{\"job\":\"j8\",\"options\":[\"x\",\"y\",\"z\"],\"timeout_ms\":5000}") == ESP_OK);
    assert(nframes == n0 + 1 && shows == s0 + 1 && ends == e0 + 2 && shown_n == 3);
    assert(!strcmp(last(), "{\"t\":\"evt\",\"name\":\"answer\",\"data\":{\"job\":\"j7\",\"index\":null,\"reason\":\"cancelled\"}}"));
    /* a tap that raced the old question (its generation) answers nothing */
    atomic_store(&s_mailbox, (uint64_t)(s_ask.gen == 1 ? 255u : s_ask.gen - 1) << 56 | 2048ull << 44 | 2048ull << 32 | (uint32_t)sdk_now);
    link_ask_tick(); assert(link_ask_pending() && nframes == n0 + 1);
    touch(206 + 130, 206 + 100); link_ask_tick();       /* lower right of 3 */
    assert(!strcmp(last(), "{\"t\":\"evt\",\"name\":\"answer\",\"data\":{\"job\":\"j8\",\"index\":2,\"label\":\"z\"}}"));

    /* pairing owns the touch: no answer from a pairing tap */
    assert(ask("{\"job\":\"j9\",\"options\":[\"a\"],\"timeout_ms\":5000}") == ESP_OK);
    pairing = true; touch(206, 206 + 150); pairing = false;
    link_ask_tick(); assert(link_ask_pending() && pair_taps == 1);   /* the pairing's tap (link_pair_tap) */
    link_ask_cancel();

    /* link down: the question still ends on screen, but no event goes out */
    connected = false; n0 = nframes;
    assert(ask("{\"job\":\"j10\",\"options\":[\"a\",\"b\"],\"timeout_ms\":5000}") == ESP_OK);
    touch(300, 206); link_ask_tick();
    assert(!link_ask_pending() && picked == 1 && nframes == n0);
    assert(ask("{\"job\":\"j11\",\"options\":[\"a\",\"b\"],\"timeout_ms\":1000}") == ESP_OK);
    sdk_now += 1000000; link_ask_tick(); assert(!link_ask_pending() && nframes == n0);
    connected = true;

    /* link_ask_open (self_update's "update" / "later"): same screen + tap, the owner hears
     * the end, no `answer` event ever */
    n0 = nframes;
    assert(link_ask_open("u1", NULL, NULL, UL, UC, 2, 5000, NULL) == ESP_ERR_INVALID_ARG);
    assert(open_q("u1", 5000) == ESP_OK && link_ask_pending() && shown_n == 2 && !strcmp(shown[0], "update"));
    touch(206 - 100, 206); link_ask_tick();                 /* left half: "update" */
    assert(dones == 1 && done_index == 0 && !done_reason[0] && picked == 0 && nframes == n0 && !link_ask_pending());
    assert(open_q("u2", 1000) == ESP_OK);
    touch(206 + 100, 206); link_ask_tick();                 /* right half: "later" */
    assert(dones == 2 && done_index == 1 && nframes == n0);
    assert(open_q("u3", 1000) == ESP_OK);
    sdk_now += 1000000; link_ask_tick();
    assert(dones == 3 && done_index == -1 && !strcmp(done_reason, "timeout") && nframes == n0);
    assert(open_q("u4", 5000) == ESP_OK);
    link_ask_cancel();                                      /* a screen command / link down */
    assert(dones == 4 && done_index == -1 && !strcmp(done_reason, "cancelled") && nframes == n0);
    /* an agent's ask replaces it: the owner hears cancelled, the ask runs as usual */
    assert(open_q("u5", 5000) == ESP_OK);
    assert(ask("{\"job\":\"j12\",\"options\":[\"a\"],\"timeout_ms\":5000}") == ESP_OK);
    assert(dones == 5 && !strcmp(done_reason, "cancelled") && nframes == n0);
    touch(206, 206 + 120); link_ask_tick();
    assert(dones == 5 && !strcmp(last(), "{\"t\":\"evt\",\"name\":\"answer\",\"data\":{\"job\":\"j12\",\"index\":0,\"label\":\"a\"}}"));
    /* the update question replaces a pending agent ask: that ask ends cancelled on the wire */
    assert(ask("{\"job\":\"j13\",\"options\":[\"a\"],\"timeout_ms\":5000}") == ESP_OK);
    n0 = nframes;
    assert(open_q("u6", 5000) == ESP_OK && nframes == n0 + 1);
    assert(!strcmp(last(), "{\"t\":\"evt\",\"name\":\"answer\",\"data\":{\"job\":\"j13\",\"index\":null,\"reason\":\"cancelled\"}}"));
    /* link down: the owner still hears the end (it decides what goes out) */
    connected = false;
    touch(206 - 100, 206); link_ask_tick();
    assert(dones == 6 && done_index == 0);
    connected = true;
    /* no touch: unsupported */
    sdk_caps &= ~MOLTY_CAP_TOUCH;
    assert(link_ask_open("u7", NULL, NULL, UL, UC, 2, 5000, on_done) == ESP_ERR_NOT_SUPPORTED);
    sdk_caps = 0xFFFFFFFFu;
    test_displayed();
    test_spot();
    test_deadline();
    test_cues();
    test_haptics();
    link_ask_cancel();
    /* activity log: every question shown (answered, timed out, cancelled, replaced) ended its tag */
    assert(sdk_job_begins>0 && sdk_job_begins==sdk_job_ends);
    puts("LINK ASK PASS: job tags begin/end on every path, unsupported without touch, args, answer by sector (label verbatim), first tap wins, no tap event while pending, not rate-limited, timeout, cancel, replace, stale tap, pairing, link down, firmware-owned question (tap/timeout/cancel/replace → owner, never an answer event), a tap answers only the question on the panel, a tap at or before the deadline beats the timeout and one after it loses, a tap that acts buzzes once (a spot, back, an answer), the water, the mic and the buttons never");
}
