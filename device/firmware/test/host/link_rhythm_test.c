/* rhythm_dev.c + rhythm_core.c on the host: the beat player shares the speaker (a job takes it, the rhythm fades and
 * gives it up, then comes back with a count-in), the buzz on the beats, the link's commands and the device's own events.
 * The SDK is link_sdk_stub.c: its speaker asserts one owner at a time. */
#include "../../main/rhythm/rhythm_dev.c"
#include "link_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static const char *refused;
esp_err_t link_cmd_refuse(const char *code) { refused = code; return LINK_CMD_REFUSED; }
static int buzzes;
esp_err_t haptic_buzz(const haptic_plan_t *p) { assert(p && p->n == 1 && p->seg[0] == 30); buzzes++; return ESP_OK; }

static const char *LIB = "{\"rhythms\":[{\"id\":\"chacarera\",\"label\":\"CHACARERA\",\"steps\":6,\"per\":3,\"beats\":[1,4],"
    "\"layers\":[{\"id\":\"aro\",\"sound\":\"rim\",\"color\":\"#9fc0ff\",\"hits\":[1,4]},{\"id\":\"parche\",\"sound\":\"head\",\"color\":\"#ff9a7a\",\"hits\":[3,5],\"accent\":[5]}],"
    "\"count_in\":1},{\"id\":\"son-clave\",\"label\":\"SON CLAVE\",\"steps\":16,\"per\":4,\"beats\":[1,5,9,13],"
    "\"layers\":[{\"id\":\"clave\",\"sound\":\"rim\",\"color\":\"#9fc0ff\",\"hits\":[1,4,7,11,13]}]}],\"pick\":0,\"bpm\":120";

/* the speaker's writes: the last block, how many, and a stop the test plants */
static int16_t last[256]; static size_t last_n; static int writes, job_at = -1, pause_at = -1;
static bool counted_in;
static void on_write(const int16_t *pcm, size_t n) {
    memcpy(last, pcm, n * sizeof(int16_t)); last_n = n; writes++;
    if (writes == 3) { rhy_view_t v; rhythm_now(&v, NULL); counted_in |= v.count_in; }
    if (writes == pause_at) rhythm_local_play();
}
static bool waiting(void) { return job_at >= 0 && writes >= job_at; }
static bool never(void) { return false; }

static cJSON *args(const char *tail) {
    static char b[2048];
    snprintf(b, sizeof b, "%s%s}", LIB, tail);
    cJSON *j = cJSON_Parse(b);
    assert(j);
    return j;
}

int main(void) {
    sdk_on_speaker_write = on_write;
    assert(rhythm_dev_init() == ESP_OK);
    assert(!rhythm_wants_speaker() && !rhythm_active());

    /* no speaker: unsupported */
    sdk_caps &= ~MOLTY_CAP_SPEAKER;
    cJSON *a = args(""), *out = NULL;
    assert(rhythm_cmd_play(a, &out) == ESP_ERR_NOT_SUPPORTED);
    sdk_caps |= MOLTY_CAP_SPEAKER;

    /* set before play: not_playing; bad args */
    cJSON *s = cJSON_Parse("{\"bpm\":90}");
    assert(rhythm_cmd_set(s, &out) == LINK_CMD_REFUSED && !strcmp(refused, "not_playing"));
    cJSON *bad = args(",\"mute\":[\"bell\"]");
    assert(rhythm_cmd_play(bad, &out) == ESP_ERR_INVALID_ARG);
    cJSON *bad2 = args(",\"tempo\":3");
    assert(rhythm_cmd_play(bad2, &out) == ESP_ERR_INVALID_ARG);

    /* play: the state comes back */
    assert(rhythm_cmd_play(a, &out) == ESP_OK && out);
    assert(!strcmp(cJSON_GetObjectItem(out, "rhythm")->valuestring, "chacarera") && cJSON_GetObjectItem(out, "bpm")->valueint == 120);
    cJSON_Delete(out); out = NULL;
    assert(rhythm_wants_speaker() && rhythm_active());

    /* it plays until a job waits: then it fades, drains and lets the speaker go */
    job_at = 40;
    const unsigned b0 = sdk_speaker_begins, d0 = sdk_drained;
    rhythm_dev_play(waiting);
    assert(sdk_speaker_begins == b0 + 1 && sdk_speaker_ends == sdk_speaker_begins && sdk_drained == d0 + 1);
    assert(counted_in);                                    /* it began with its count-in */
    assert(writes >= 40 && writes <= 41 && last_n < 256);    /* gave way within one block: the fade, then nothing */
    int tail = 0;
    for (size_t q = 0; q < last_n; q++) if (abs(last[q]) > 4000) tail = (int)q;
    assert(tail < RHY_FADE);                               /* loud only inside the fade */
    rhy_view_t v;
    assert(rhythm_now(&v, NULL) && v.paused && !v.rest && rhythm_wants_speaker() && !rhythm_sounding());   /* (paused for the job, not at rest) */
    printf("  share   a job came after %d blocks: the rhythm faded and gave way within one block, the stream ended (one owner)\n", job_at);

    /* the job owns the speaker meanwhile (the stub asserts a single owner) */
    assert(molty_speaker_stream_begin(16000) == ESP_OK);
    molty_speaker_stream_end();

    /* after it: a count-in, then on; the accept button stops it at rest (the event says paused) */
    job_at = -1; writes = 0; counted_in = false; pause_at = 30;
    rhythm_dev_play(never);
    assert(counted_in && writes >= 30 && writes <= 32 && sdk_speaker_ends == sdk_speaker_begins);
    assert(!rhythm_wants_speaker() && rhythm_active());
    rhythm_ev_t ev;
    assert(rhythm_take_event(&ev) && ev.kind == RHY_EV_STATE && !strcmp(ev.state, "paused") && !strcmp(ev.id, "chacarera"));
    assert(rhythm_now(&v, NULL) && v.rest && v.count_in);      /* at rest: back at the phrase's start */
    cJSON *f = rhythm_event_frame(&ev);
    assert(!strcmp(cJSON_GetObjectItem(f, "name")->valuestring, "rhythm") && !strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(f, "data"), "state")->valuestring, "paused"));
    cJSON_Delete(f);
    printf("  share   back with a count-in after the job; the accept button stopped it at rest after %d blocks\n", writes);

    /* the buzz: off unless asked; on, one per main beat heard */
    cJSON *on = cJSON_Parse("{\"buzz\":true,\"paused\":false}");
    assert(rhythm_cmd_set(on, &out) == ESP_OK && cJSON_IsTrue(cJSON_GetObjectItem(out, "buzz")));
    cJSON_Delete(out); out = NULL;
    writes = 0; pause_at = 250; buzzes = 0;               /* 4 s at 120: a 1 s count-in (no buzz), then 3 s */
    while (rhythm_take_event(&ev)) {}
    rhythm_dev_play(never);
    assert(buzzes >= 5 && buzzes <= 7);   /* a beat every 0.5 s (a dotted quarter at 120), after a one-bar count-in */
    printf("  buzz    %d buzzes in %.1f s at 120 (two main beats a bar)\n", buzzes, writes * 256.0 / RHY_RATE);

    /* the screen's acts: a mute (a tap event naming the drum), a pick, a tempo */
    cJSON *go = cJSON_Parse("{\"paused\":false}");
    assert(rhythm_cmd_set(go, NULL) == ESP_OK);
    while (rhythm_take_event(&ev)) {}                     /* (the pause above) */
    rhythm_local_mute(1, 0.4f, 0.6f);
    assert(rhythm_take_event(&ev) && ev.kind == RHY_EV_MUTE && ev.on && !strcmp(ev.id, "parche"));
    f = rhythm_event_frame(&ev);
    cJSON *d = cJSON_GetObjectItem(f, "data");
    assert(!strcmp(cJSON_GetObjectItem(f, "name")->valuestring, "tap") && !strcmp(cJSON_GetObjectItem(d, "spot")->valuestring, "parche") && cJSON_IsTrue(cJSON_GetObjectItem(d, "muted")));
    cJSON_Delete(f);
    rhythm_local_bpm(150.f);
    rhythm_local_pick(1);
    char id[RHY_ID_MAX + 1], labels[4][RHYTHM_LABEL_ROOM];
    assert(rhythm_now(&v, NULL) && v.cur == 1 && v.bpm == 150.f && v.count_in && !v.mute);
    assert(rhythm_labels(-1, labels, NULL, 4) == 2 && !strcmp(labels[1], "SON CLAVE") && !strcmp(rhythm_id(1, id), "son-clave"));
    assert(!rhythm_take_event(&ev));                       /* the wheel's own event is the wheel's (mole_fluid.c) */

    /* the sync trim: kept on the device (NVS) when it changes, only then; out of range refused; the kept one at boot */
    const int w0 = sdk_rhythm_sync_writes;
    cJSON *sy = cJSON_Parse("{\"sync\":-35}"), *sy2 = cJSON_Parse("{\"sync\":300}");
    assert(rhythm_cmd_set(sy, &out) == ESP_OK && cJSON_GetObjectItem(out, "sync")->valueint == -35 && sdk_rhythm_sync == -35 && sdk_rhythm_sync_writes == w0 + 1);
    cJSON_Delete(out); out = NULL;
    assert(rhythm_cmd_set(sy, NULL) == ESP_OK && sdk_rhythm_sync_writes == w0 + 1);   /* (the same: no write) */
    assert(rhythm_cmd_set(sy2, NULL) == ESP_ERR_INVALID_ARG && rhythm_sync() == -35);
    sdk_rhythm_sync = 40; rhythm_dev_init();
    assert(rhythm_sync() == 40);
    cJSON_Delete(sy); cJSON_Delete(sy2);
    printf("  sync    the trim kept in NVS when it changes, refused past 250 ms, read back at boot\n");

    /* the clock left the screen: it stops after RHYTHM_ORPHAN_S */
    rhythm_orphan(false, 1.f);
    assert(rhythm_active());
    rhythm_orphan(true, 1.f);
    rhythm_orphan(false, 1.5f);
    assert(rhythm_active());
    rhythm_orphan(false, 0.6f);
    rhythm_dev_play(never);                                /* the fade out */
    assert(!rhythm_active() && !rhythm_wants_speaker());
    assert(rhythm_take_event(&ev) && !strcmp(ev.state, "stopped"));
    assert(rhythm_cmd_set(s, &out) == LINK_CMD_REFUSED && !strcmp(refused, "not_playing"));
    assert(rhythm_cmd_stop(NULL, NULL) == ESP_OK);

    cJSON_Delete(a); cJSON_Delete(s); cJSON_Delete(bad); cJSON_Delete(bad2); cJSON_Delete(on); cJSON_Delete(go);
    printf("link_rhythm_test: ok\n");
    return 0;
}
