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
static bool pairing;
bool link_pair_active(void) { return pairing; }
void mole_fluid_event(mole_fluid_event_t ev) { (void)ev; }
static uint32_t binds; static int talks, stops;
uint32_t mole_fluid_gestures(void) { return binds; }
bool link_audio_gesture_talk(void) { talks++; return true; }
void link_audio_stop_any(void) { stops++; }
bool link_audio_listening(uint32_t *age) { (void)age; return false; }
bool link_audio_say_waiting(void) { return false; }
uint32_t link_audio_job_gen(void) { return 0; }
void link_audio_request(uint32_t gen, link_audio_req_t what) { (void)gen; (void)what; }
void link_audio_cancel(void) { }
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
static int shown_n = -1, picked = -1; static unsigned shows, ends; static char shown[5][9];
static int32_t shown_rgb[5];          /* the colours handed to the screen; -2 = none (NULL) */
static unsigned want_seq, glass; static bool want_up;
esp_err_t mole_fluid_ask_show(int n, const char *const *labels, const int32_t *colors, unsigned *seq) {
    shows++; shown_n = n; picked = -1;
    for (int i = 0; i < n; i++) { snprintf(shown[i], sizeof(shown[i]), "%s", labels[i]); shown_rgb[i] = colors ? colors[i] : -2; }
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
static esp_err_t open_q(const char *job, unsigned timeout_ms) { esp_err_t e = link_ask_open(job, UL, UC, 2, timeout_ms, on_done); draw(); return e; }
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
    assert(link_ask_open("u8", UL, UC, 2, 5000, on_done) == ESP_OK);
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

int main(void) {
    /* the buttons do what they are bound to; a pending question comes first */
    sdk_now = 90000000;
    binds = gesture_binds_set(gesture_binds_default(), GESTURE_IN_BUTTON_REJECT, GESTURE_DO_TALK);
    button_do(false); assert(talks == 1 && nframes == 0);
    button_do(true); assert(talks == 1 && nframes == 1 && strstr(last(), "\"button\""));   /* event: today's behaviour */
    binds = gesture_binds_set(binds, GESTURE_IN_BUTTON_ACCEPT, GESTURE_DO_STOP);
    button_do(true); assert(stops == 1 && nframes == 1);
    binds = gesture_binds_set(binds, GESTURE_IN_BUTTON_ACCEPT, GESTURE_DO_NONE);
    button_do(true); assert(stops == 1 && nframes == 1);
    assert(ask(YNI) == ESP_OK);
    button_do(false); assert(talks == 1 && nframes == 2);                      /* an open question: an event, whatever is bound */
    link_ask_cancel(); nframes = 0; shows = 0; dones = 0; binds = gesture_binds_default();
    /* no touch: nothing could answer → unsupported, nothing on screen */
    sdk_caps &= ~MOLTY_CAP_TOUCH;
    assert(ask(YNI) == ESP_ERR_NOT_SUPPORTED && !shows && !link_ask_pending());
    sdk_caps = 0xFFFFFFFFu;
    /* bad args: refused, nothing shown */
    assert(ask("{\"job\":\"j1\",\"options\":[\"123456789\"],\"timeout_ms\":5000}") == ESP_ERR_INVALID_ARG && !shows);
    /* colours: optional, one "#rrggbb" per option; a bad one refuses the question */
    assert(ask("{\"job\":\"j1\",\"options\":[\"a\",\"b\"],\"colors\":[\"#3ccf96\"],\"timeout_ms\":5000}") == ESP_ERR_INVALID_ARG && !shows);
    assert(ask("{\"job\":\"j1\",\"options\":[\"a\"],\"colors\":[\"green\"],\"timeout_ms\":5000}") == ESP_ERR_INVALID_ARG && !shows);
    assert(ask("{\"job\":\"c1\",\"options\":[\"a\",\"b\"],\"colors\":[\"#3CCF96\",\"#000000\"],\"timeout_ms\":5000}") == ESP_OK);
    assert(shows == 1 && shown_rgb[0] == 0x3ccf96 && shown_rgb[1] == 0);
    assert(ask("{\"job\":\"c2\",\"options\":[\"a\",\"b\"],\"timeout_ms\":5000}") == ESP_OK);   /* older relay: defaults */
    assert(shows == 2 && shown_rgb[0] == -1 && shown_rgb[1] == -1);
    assert(open_q("c3", 5000) == ESP_OK && shown_rgb[0] == 0x3ccf96 && shown_rgb[1] == 0xf1f3f5);   /* the firmware's own */
    link_ask_cancel(); shows = 0; nframes = 0; dones = 0;

    /* an answer: the tap's sector picks the option (n = 3: upper left, upper right, bottom);
     * the label goes back exactly as received; no `tap` event for that tap */
    sdk_now = 1000000;
    assert(ask(YNI) == ESP_OK && link_ask_pending() && shows == 1 && shown_n == 3 && !strcmp(shown[1], " no "));
    assert(nframes == 0);
    touch(206 + 90, 206 - 60);
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
    touch(206, 206 + 150); link_ask_tick();       /* bottom of 3 */
    assert(!strcmp(last(), "{\"t\":\"evt\",\"name\":\"answer\",\"data\":{\"job\":\"j8\",\"index\":2,\"label\":\"z\"}}"));

    /* pairing owns the touch: no answer from a pairing tap */
    assert(ask("{\"job\":\"j9\",\"options\":[\"a\"],\"timeout_ms\":5000}") == ESP_OK);
    pairing = true; touch(206, 206); pairing = false;
    link_ask_tick(); assert(link_ask_pending());
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
    assert(link_ask_open("u1", UL, UC, 2, 5000, NULL) == ESP_ERR_INVALID_ARG);
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
    touch(206, 206); link_ask_tick();
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
    assert(link_ask_open("u7", UL, UC, 2, 5000, on_done) == ESP_ERR_NOT_SUPPORTED);
    sdk_caps = 0xFFFFFFFFu;
    test_displayed();
    test_deadline();
    link_ask_cancel();
    /* activity log: every question shown (answered, timed out, cancelled, replaced) ended its tag */
    assert(sdk_job_begins>0 && sdk_job_begins==sdk_job_ends);
    puts("LINK ASK PASS: job tags begin/end on every path, unsupported without touch, args, answer by sector (label verbatim), first tap wins, no tap event while pending, not rate-limited, timeout, cancel, replace, stale tap, pairing, link down, firmware-owned question (tap/timeout/cancel/replace → owner, never an answer event), a tap answers only the question on the panel, a tap at or before the deadline beats the timeout and one after it loses");
}
