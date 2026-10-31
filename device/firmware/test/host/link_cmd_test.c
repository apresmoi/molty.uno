/* link_cmd.c on the host: which moods count as explicit (link_cmd_mood_gen). link_audio
 * reverts to idle at a listen/say's end only when this count did not move
 * (link_audio_pair_test.c test_mood_survives). */
#include "link_sdk_stub.h"
typedef struct { float ax, ay, az, gx, gy, gz; uint32_t ms; } molty_imu_sample_t;
bool molty_imu_get(molty_imu_sample_t *out);
esp_err_t molty_speaker_set_volume(uint8_t vol);
cJSON *mole_diag_json(int timeline_secs);
char *mole_diag_log_text(uint32_t since, size_t max_bytes, uint32_t *next, bool json);
#include "../../main/link/link_cmd.c"
#include <assert.h>
#include <stdio.h>

bool molty_imu_get(molty_imu_sample_t *out) { (void)out; return false; }
esp_err_t molty_speaker_set_volume(uint8_t vol) { (void)vol; return ESP_OK; }
cJSON *mole_diag_json(int t) { (void)t; return cJSON_CreateObject(); }
char *mole_diag_log_text(uint32_t s, size_t m, uint32_t *n, bool j) { (void)s;(void)m;(void)n;(void)j; return NULL; }
esp_err_t mole_fluid_set_text(const char *t, int len) { (void)t;(void)len; return ESP_OK; }
static int acted = -1, acts;
void mole_fluid_act(int a) { acted = a; acts++; }
/* the bindings (mole_fluid.c, NVS): what cmd gestures passes through */
static const cJSON *gestures_given; static esp_err_t gestures_answer = ESP_OK;
esp_err_t mole_fluid_gestures_apply(const cJSON *b) { gestures_given = b; return gestures_answer; }
cJSON *mole_fluid_gestures_json(void) { cJSON *o = cJSON_CreateObject(); cJSON_AddStringToObject(o, "tap", "event"); return o; }
bool link_pair_active(void) { return false; }
esp_err_t link_audio_say(const cJSON *a, cJSON **o) { (void)a;(void)o; return ESP_OK; }
esp_err_t link_audio_listen(const cJSON *a, cJSON **o) { (void)a;(void)o; return ESP_OK; }
esp_err_t link_audio_listen_stop(const cJSON *a, cJSON **o) { (void)a;(void)o; return ESP_OK; }
esp_err_t link_audio_say_cancel(const cJSON *a, cJSON **o) { (void)a;(void)o; return ESP_OK; }
static uint32_t s_ext_hold_ms, s_ext_n;   /* link_audio_external_react calls (the relay's reaction over a listen) */
void link_audio_external_react(uint32_t hold_ms) { s_ext_hold_ms = hold_ms; s_ext_n++; }
static unsigned ask_cancels, ask_cmds;
void link_ask_cancel(void) { ask_cancels++; }
esp_err_t link_ask_cmd(const cJSON *a, cJSON **o) { (void)a;(void)o; ask_cmds++; return ESP_OK; }
static unsigned update_cmds;
esp_err_t self_update_cmd(const cJSON *a, cJSON **o) { (void)a;(void)o; update_cmds++; return link_cmd_refuse("wrong_board"); }

/* self_update.c's screen writer, extracted verbatim by link_build.sh */
#include "update_screen.inc"

static unsigned gen_at_unlock;
static void snap_gen(void) { gen_at_unlock=link_cmd_mood_gen(); }
/* another task's explicit mood, completed just before this task got the lock */
static void racing_mood(void) { atomic_fetch_add(&s_mood_gen,1); }

static esp_err_t run(const char *name, const char *args) {
    cJSON *a=cJSON_Parse(args),*out=NULL; assert(a);
    esp_err_t e=link_cmd_run(name,a,&out); cJSON_Delete(a); cJSON_Delete(out); return e;
}
int main(void) {
    link_cmd_init();
    /* gestures: read, set; a refusal changes and answers nothing */
    { cJSON *r=NULL, *a=cJSON_Parse("{}");
      assert(link_cmd_run("gestures",a,&r)==ESP_OK && !gestures_given && cJSON_GetObjectItem(r,"bindings")); cJSON_Delete(r); cJSON_Delete(a);
      a=cJSON_Parse("{\"bindings\":{\"tap\":\"talk\"}}"); r=NULL;
      assert(link_cmd_run("gestures",a,&r)==ESP_OK && gestures_given); cJSON_Delete(r);
      gestures_answer=ESP_ERR_INVALID_ARG; r=NULL;
      assert(link_cmd_run("gestures",a,&r)==ESP_ERR_INVALID_ARG && !r); gestures_answer=ESP_OK; cJSON_Delete(a); }
    unsigned g=link_cmd_mood_gen();
    /* the device's own moods (listening / speaking / idle around a job) are not explicit */
    assert(link_cmd_mood("listening",-1)==ESP_OK && link_cmd_mood_gen()==g);
    /* the link's mood command is, once accepted, with or without hold_s */
    assert(run("mood","{\"mood\":\"happy\",\"hold_s\":0}")==ESP_OK && link_cmd_mood_gen()==g+1);
    assert(strstr(sdk_state,"\"mood\":\"happy\"") && strstr(sdk_state,"\"hold_s\":0"));
    assert(run("mood","{\"mood\":\"thinking\"}")==ESP_OK && link_cmd_mood_gen()==g+2);
    /* rejected: not counted */
    assert(run("mood","{\"mood\":\"furious\"}")==ESP_ERR_INVALID_ARG && link_cmd_mood_gen()==g+2);
    assert(run("mood","{\"mood\":\"happy\",\"hold_s\":-1}")==ESP_ERR_INVALID_ARG && link_cmd_mood_gen()==g+2);
    assert(run("show","{\"text\":\"hi\"}")==ESP_OK && link_cmd_mood_gen()==g+2);
    /* a react (mood {react:true}, cap "react") shows the mood but is not explicit: the listen's
     * ack and idle fallback stay, and it never cancels a question; a non-bool react is refused */
    s_ext_n=0;
    assert(run("mood","{\"mood\":\"surprised\",\"hold_s\":0,\"react\":true}")==ESP_OK && link_cmd_mood_gen()==g+2);
    assert(strstr(sdk_state,"\"mood\":\"surprised\""));
    /* ...and tells the listen it plays (Molty's own listener reactions wait for it); the relay's
     * return to `listening` and an explicit mood do not */
    assert(s_ext_n==1 && s_ext_hold_ms==LINK_REACT_SHOW_MS);
    assert(run("mood","{\"mood\":\"listening\",\"hold_s\":0,\"react\":true}")==ESP_OK && s_ext_n==1);
    assert(run("mood","{\"mood\":\"happy\",\"react\":false}")==ESP_OK && link_cmd_mood_gen()==g+3 && s_ext_n==1);
    assert(run("mood","{\"mood\":\"happy\",\"react\":1}")==ESP_ERR_INVALID_ARG && link_cmd_mood_gen()==g+3);
    /* local HTTP state bodies: only a top-level mood string counts */
    assert(link_cmd_state_set_explicit("{\"mood\":\"sleepy\",\"hold_s\":30}")==ESP_OK && link_cmd_mood_gen()==g+4);
    assert(link_cmd_state_set_explicit("{\"state\":\"text\",\"text\":\"my \\\"mood\\\" is fine\"}")==ESP_OK && link_cmd_mood_gen()==g+4);
    assert(link_cmd_state_set_explicit("{\"state\":\"blob\"}")==ESP_OK && link_cmd_mood_gen()==g+4);
    assert(link_cmd_state_set_explicit("not json \"mood\"")==ESP_OK && link_cmd_mood_gen()==g+4);
    /* the creature reads "mood" case-insensitively with escapes decoded: count those too */
    assert(link_cmd_state_set_explicit("{\"Mood\":\"happy\"}")==ESP_OK && link_cmd_mood_gen()==g+5);
    assert(link_cmd_state_set_explicit("{\"\\u006dood\":\"happy\"}")==ESP_OK && link_cmd_mood_gen()==g+6);
    /* the link's raw state command with a mood is explicit too */
    assert(run("state","{\"mood\":\"happy\"}")==ESP_OK && link_cmd_mood_gen()==g+7);
    assert(run("state","{\"state\":\"blob\"}")==ESP_OK && link_cmd_mood_gen()==g+7);
    /* the race: an explicit mood's apply + count, and a job end's check + idle, share one
     * lock (HTTP task vs link task). Every mood write below must run with it held. */
    sdk_state_locked=false; assert(link_cmd_state_set_explicit("{\"mood\":\"happy\"}")==ESP_OK && sdk_state_locked);
    sdk_state_locked=false; assert(run("mood","{\"mood\":\"sleepy\"}")==ESP_OK && sdk_state_locked);
    unsigned g0=link_cmd_mood_gen();
    strcpy(sdk_state,""); sdk_state_locked=false; link_cmd_mood_idle_unless(g0);
    assert(strstr(sdk_state,"\"mood\":\"idle\"") && sdk_state_locked);
    /* critical-section boundaries: the count moves before release, the check after acquire */
    unsigned g1=link_cmd_mood_gen(); sdk_on_unlock=snap_gen;
    assert(link_cmd_state_set_explicit("{\"mood\":\"happy\"}")==ESP_OK && gen_at_unlock==g1+1);
    assert(run("mood","{\"mood\":\"sleepy\"}")==ESP_OK && gen_at_unlock==g1+2);
    sdk_on_unlock=NULL;
    g1=link_cmd_mood_gen(); sdk_on_lock=racing_mood; strcpy(sdk_state,"");
    link_cmd_mood_idle_unless(g1); sdk_on_lock=NULL;
    assert(!strstr(sdk_state,"idle"));
    g0=link_cmd_mood_gen();
    /* an explicit mood since the snapshot: idle is skipped */
    assert(run("mood","{\"mood\":\"thinking\"}")==ESP_OK);
    link_cmd_mood_idle_unless(g0); assert(strstr(sdk_state,"\"mood\":\"thinking\""));
    /* the update's progress owns the screen: a say/listen that started before the update
     * (its snapshot g0) and ends during the download must not put idle over "40%" */
    g0=link_cmd_mood_gen(); sdk_state_locked=false;
    screen_word("40%");
    assert(strstr(sdk_state,"\"state\":\"text\"") && sdk_state_locked);   /* written under the mood lock */
    link_cmd_mood_idle_unless(g0);
    assert(strstr(sdk_state,"\"state\":\"text\"") && !strstr(sdk_state,"idle"));
    assert(sdk_locks==0);
    /* ask is registered; commands that change the screen cancel a pending question once
     * accepted, the rest (reads, refused args, hardware) leave it up */
    assert(run("ask","{}")==ESP_OK && ask_cmds==1);
    unsigned c=ask_cancels;
    assert(run("show","{\"text\":\"hi\"}")==ESP_OK && ask_cancels==c+1);
    assert(run("mood","{\"mood\":\"happy\"}")==ESP_OK && ask_cancels==c+2);
    assert(run("mood","{\"mood\":\"happy\",\"react\":true}")==ESP_OK && ask_cancels==c+2);   /* a react leaves it up */
    assert(run("state","{\"palette\":\"green\"}")==ESP_OK && ask_cancels==c+3);
    assert(run("state","{}")==ESP_OK && run("status","{}")==ESP_OK && run("diag","{}")==ESP_OK && ask_cancels==c+3);
    assert(run("volume","{\"volume\":3}")==ESP_OK && run("imu","{}")==ESP_ERR_NOT_SUPPORTED && ask_cancels==c+3);
    assert(run("show","{\"text\":1}")==ESP_ERR_INVALID_ARG && run("mood","{\"mood\":\"furious\"}")==ESP_ERR_INVALID_ARG && ask_cancels==c+3);
    /* a command the creature refuses after its args passed leaves the question up too:
     * only an accepted one takes the screen (state's body is validated by the creature) */
    sdk_state_error=ESP_ERR_INVALID_ARG;
    assert(run("state","{\"palette\":\"nope\"}")==ESP_ERR_INVALID_ARG && ask_cancels==c+3);
    assert(run("state","{\"mood\":\"happy\"}")==ESP_ERR_INVALID_ARG && ask_cancels==c+3);
    assert(run("mood","{\"mood\":\"happy\"}")==ESP_ERR_INVALID_ARG && ask_cancels==c+3);
    assert(run("show","{\"text\":\"hi\"}")==ESP_ERR_INVALID_ARG && ask_cancels==c+3);
    sdk_state_error=ESP_OK; sdk_select_error=ESP_ERR_NO_MEM;
    assert(run("mood","{\"mood\":\"happy\"}")==ESP_ERR_NO_MEM && run("show","{\"text\":\"hi\"}")==ESP_ERR_NO_MEM && ask_cancels==c+3);
    sdk_select_error=ESP_OK;
    assert(run("state","{\"palette\":\"green\"}")==ESP_OK && ask_cancels==c+4);
    /* act: a gesture on the creature — never takes the screen (no mood, no question
     * cancelled, not an explicit mood); unknown actions refused */
    unsigned ga=link_cmd_mood_gen(); c=ask_cancels; strcpy(sdk_state,"");
    cJSON *ao=NULL, *aa=cJSON_Parse("{\"action\":\"nod\"}");
    s_ext_n=0;
    assert(link_cmd_run("act",aa,&ao)==ESP_OK && acted==CA_NOD && acts==1 && !strcmp(cJSON_GetObjectItem(ao,"action")->valuestring,"nod"));
    assert(s_ext_n==1 && s_ext_hold_ms==(uint32_t)(cute_act_dur(CA_NOD)*1000.f));   /* the gesture's length: a listen's own reactions wait */
    cJSON_Delete(aa); cJSON_Delete(ao);
    assert(link_cmd_mood_gen()==ga && ask_cancels==c && !sdk_state[0]);
    assert(run("act","{\"action\":\"dance\"}")==ESP_ERR_INVALID_ARG && run("act","{}")==ESP_ERR_INVALID_ARG && acts==1);
    /* update is registered; a handler's own refusal code reads back */
    cJSON *out=NULL, *a=cJSON_Parse("{}");
    assert(link_cmd_run("update",a,&out)==LINK_CMD_REFUSED && update_cmds==1 && !strcmp(link_cmd_refusal(),"wrong_board"));
    cJSON_Delete(a);
    cJSON_Delete(sdk_sent);
    puts("LINK CMD PASS: explicit moods counted (link mood cmd, local state POST) under the mood lock, own/rejected moods not; screen commands cancel a question only once accepted; act never takes the screen; react moods / acts hold the listen's own reactions for their length");
}
