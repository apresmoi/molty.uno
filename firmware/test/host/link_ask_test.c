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
void mole_fluid_set_status(int status) { (void)status; }
void mole_fluid_listen(bool on) { (void)on; }
void mole_fluid_listen_level(float level) { (void)level; }
int fd_status_from(bool wifi, bool relay, bool auth) { (void)wifi; (void)relay; (void)auth; return 0; }
void link_client_health(bool *w, bool *r, bool *a) { *w = *r = true; *a = false; }
static bool connected = true;
bool link_client_connected(void) { return connected; }
static char frames[16][256]; static unsigned nframes;
esp_err_t link_send_json(const cJSON *j) {
    char *t = cJSON_PrintUnformatted(j); assert(t && strlen(t) < sizeof(frames[0]) && nframes < 16);
    strcpy(frames[nframes++], t); cJSON_free(t); return ESP_OK;
}
/* the screen (mole_fluid.c) */
static int shown_n = -1, picked = -1; static unsigned shows, ends; static char shown[5][9];
esp_err_t mole_fluid_ask_show(int n, const char *const *labels) {
    shows++; shown_n = n; picked = -1;
    for (int i = 0; i < n; i++) snprintf(shown[i], sizeof(shown[i]), "%s", labels[i]);
    return ESP_OK;
}
void mole_fluid_ask_pick(int index) { picked = index; }
void mole_fluid_ask_end(void) { ends++; }

static esp_err_t ask(const char *args) {
    cJSON *a = cJSON_Parse(args), *out = NULL; assert(a);
    esp_err_t e = link_ask_cmd(a, &out);
    if (e == ESP_OK) assert(out && !strcmp(cJSON_GetObjectItem(out, "job")->valuestring, cJSON_GetObjectItem(a, "job")->valuestring));
    else assert(!out);
    cJSON_Delete(a); cJSON_Delete(out);
    return e;
}
/* a finger down then up at panel px (x, y) */
static void touch(int x, int y) {
    uint16_t xs[1] = { (uint16_t)x }, ys[1] = { (uint16_t)y };
    sense_touch(1, xs, ys); sense_touch(0, xs, ys);
}
static const char *last(void) { return nframes ? frames[nframes - 1] : ""; }

#define YNI "{\"job\":\"j1\",\"options\":[\"Yes\",\" no \",\"idk\"],\"timeout_ms\":5000}"

int main(void) {
    /* no touch: nothing could answer → unsupported, nothing on screen */
    sdk_caps &= ~MOLTY_CAP_TOUCH;
    assert(ask(YNI) == ESP_ERR_NOT_SUPPORTED && !shows && !link_ask_pending());
    sdk_caps = 0xFFFFFFFFu;
    /* bad args: refused, nothing shown */
    assert(ask("{\"job\":\"j1\",\"options\":[\"123456789\"],\"timeout_ms\":5000}") == ESP_ERR_INVALID_ARG && !shows);

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
    atomic_store(&s_mailbox, (s_ask.gen == 1 ? 255u : s_ask.gen - 1) << 24 | 2048u << 12 | 2048u);
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
    puts("LINK ASK PASS: unsupported without touch, args, answer by sector (label verbatim), first tap wins, no tap event while pending, not rate-limited, timeout, cancel, replace, stale tap, pairing, link down");
}
