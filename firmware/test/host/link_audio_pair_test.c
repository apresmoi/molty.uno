#include "link_sdk_stub.h"
#include "link_protocol.h"
#include "../../main/link/link_audio.c"
#include "../../main/link/link_pair.c"
#include <assert.h>
#include <stdio.h>

static unsigned rotations, sent;
static esp_err_t send_error;
static const char *field(const char *key) { cJSON *j=cJSON_GetObjectItem(sdk_sent,key); assert(cJSON_IsString(j)); return j->valuestring; }
bool link_client_connected(void) { return true; }
esp_err_t link_send_json(const cJSON *j) {
    cJSON_Delete(sdk_sent); sdk_sent=cJSON_Duplicate(j,true); sent++; return send_error;
}
static unsigned bin_frames, bin_bytes, bin_ends, bin_seq;
esp_err_t link_send_binary(const void *data,size_t size) {
    link_frame_t h; const uint8_t *pcm; size_t bytes;
    assert(link_frame_parse(data,size,&h,&pcm,&bytes) && size<=16384);
    assert(h.seq==bin_seq++ && !bin_ends);       /* ordered, nothing after end */
    bin_frames++; bin_bytes+=bytes; bin_ends+=h.end; return ESP_OK;
}
static void listen_job(unsigned ms, audio_job_t *job) {
    char a[96]; snprintf(a,sizeof(a),"{\"job\":\"listen1\",\"ms\":%u,\"rate\":16000}",ms);
    cJSON *args=cJSON_Parse(a),*res=NULL;
    assert(link_audio_listen(args,&res)==ESP_OK); cJSON_Delete(args); cJSON_Delete(res);
    assert(xQueueReceive(s_jobs,job,0)==pdTRUE && job->listen);
    bin_frames=bin_bytes=bin_ends=bin_seq=0;
}
void link_identity_rotate(void) { rotations++; }
const char *link_identity_code(void) { return "ABCDEFGH"; }
esp_err_t link_cmd_mood(const char *mood) { strcpy(sdk_mood,mood); return ESP_OK; }
esp_err_t link_ui_set_text(const char *text,int len) { assert(len>=0 && len<=4096); memcpy(sdk_text,text,len+1); return ESP_OK; }

static cJSON *say_args(void) { return cJSON_Parse("{\"job\":\"molty-abcdefghij.j_1\",\"text\":\"hello world\",\"ms\":640,\"rate\":16000}"); }
static void begin_say(void) {
    cJSON *args=say_args(), *res=NULL;
    assert(link_audio_say(args,&res)==ESP_OK && res);
    assert(!strcmp(sdk_mood,"speaking") && !strcmp(sdk_state,"{\"state\":\"text\"}"));
    assert(!strcmp(sdk_text,"hello world"));
    assert(!strcmp(cJSON_GetObjectItem(res,"job")->valuestring,"molty-abcdefghij.j_1"));
    cJSON_Delete(res); cJSON_Delete(args);
    audio_job_t job; assert(xQueueReceive(s_jobs,&job,0)==pdTRUE && !job.listen);
}
static void receive_pcm(unsigned seq,bool end) {
    uint8_t frame[LINK_MESSAGE_MAX];
    link_frame_t h={.seq=seq,.end=end}; strcpy(h.stream,s_job);
    int n=link_frame_header((char *)frame,sizeof(frame),&h); assert(n>0);
    memset(frame+n,0x12,10240); /* relay's real 320 ms chunk */
    assert(link_audio_receive(frame,n+10240)==ESP_OK);
}
static void complete(esp_err_t e) { assert(xQueueSend(s_done,&e,0)==pdTRUE); link_audio_tick(); }

static void test_say(void) {
    begin_say(); unsigned before=sent;
    receive_pcm(0,false); link_audio_tick(); assert(sent==before && !sdk_drained);
    receive_pcm(1,true); link_audio_tick(); assert(sent==before && !sdk_drained);
    esp_err_t e=play(); assert(e==ESP_OK && sdk_drained==1 && sdk_written==20480);
    assert(sent==before); complete(e);
    assert(!strcmp(field("name"),"say.done") && !strcmp(sdk_mood,"idle") && !link_audio_busy());
    assert(!strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(sdk_sent,"data"),"job")->valuestring,"molty-abcdefghij.j_1"));
    begin_say(); before=sent; receive_pcm(0,true); link_audio_cancel();
    complete(play()); assert(sent==before && !link_audio_busy());
    begin_say(); before=sent; receive_pcm(0,true); sdk_play_error=ESP_FAIL;
    complete(play()); assert(sent==before && !link_audio_busy()); sdk_play_error=ESP_OK;
    /* Bare end frame is accepted, including an empty TTS result. */
    begin_say();
    const char *end="{\"s\":\"molty-abcdefghij.j_1\",\"seq\":0,\"kind\":\"pcm16\",\"rate\":16000,\"end\":true}";
    assert(link_audio_receive(end,strlen(end))==ESP_OK); complete(play());
    assert(!strcmp(field("name"),"say.done"));
    cJSON *args=say_args(),*res=NULL;
    cJSON_ReplaceItemInObject(args,"rate",cJSON_CreateNumber(8000));
    assert(link_audio_say(args,&res)==ESP_ERR_INVALID_ARG && !link_audio_busy());
    cJSON_ReplaceItemInObject(args,"rate",cJSON_CreateNumber(16000));
    cJSON_ReplaceItemInObject(args,"ms",cJSON_CreateNumber(-1));
    assert(link_audio_say(args,&res)==ESP_ERR_INVALID_ARG); cJSON_Delete(args);
    /* Listen finishes on the binary end flag and must not emit say.done. */
    audio_job_t job; listen_job(10,&job);
    before=sent; complete(record(&job)); assert(sent==before && !strcmp(sdk_mood,"idle"));
    assert(bin_frames==1 && bin_bytes==320 && bin_ends==1);
    /* Streamed uplink: 5 s = 80000 samples in 300 ms frames, sent while recording. */
    listen_job(5000,&job); sdk_rx_chunks=0;
    complete(record(&job));
    assert(bin_bytes==160000 && bin_ends==1 && bin_frames==17 && sdk_rx_chunks==50);
    /* Mic failure mid-stream fails fast with an end frame (relay: no 5 s wait). */
    listen_job(5000,&job); sdk_rx_error=ESP_ERR_TIMEOUT;
    esp_err_t le=record(&job); complete(le); sdk_rx_error=ESP_OK;
    assert(le==ESP_ERR_TIMEOUT && bin_frames==1 && bin_bytes==0 && bin_ends==1);
}
static void request(void) {
    cJSON *args=cJSON_Parse("{\"t\":\"pair.request\",\"req\":\"request1\",\"agent_name\":\"test agent\",\"code\":\"1234\"}");
    link_pair_request(args); cJSON_Delete(args); assert(link_pair_active());
}
static cJSON *status(void) { cJSON *j=cJSON_CreateObject(); link_pair_status(j); return j; }
static void test_pair(void) {
    cJSON *st=status();   /* boot: nothing pending, no decision yet */
    assert(cJSON_IsNull(cJSON_GetObjectItem(st,"pair")) && cJSON_IsNull(cJSON_GetObjectItem(st,"last_pair"))); cJSON_Delete(st);
    request();
    /* Static prompt: the code alone, as one held token. */
    assert(!strcmp(sdk_state,"{\"state\":\"text\",\"text\":\"1234\"}"));
    st=status(); cJSON *p=cJSON_GetObjectItem(st,"pair");
    assert(!strcmp(cJSON_GetObjectItem(p,"agent_name")->valuestring,"test agent") && !strcmp(cJSON_GetObjectItem(p,"code")->valuestring,"1234"));
    assert(cJSON_GetObjectItem(p,"expires_in_s")->valueint==110 && cJSON_IsNull(cJSON_GetObjectItem(st,"last_pair"))); cJSON_Delete(st);
    sdk_now+=30000000; link_pair_tick(); assert(link_pair_active());     /* 30 s is no longer the limit */
    st=status(); assert(cJSON_GetObjectItem(cJSON_GetObjectItem(st,"pair"),"expires_in_s")->valueint==80); cJSON_Delete(st);
    sdk_now+=79999999; link_pair_tick(); assert(link_pair_active());
    sdk_now+=1; link_pair_tick();
    assert(!link_pair_active() && !strcmp(field("reason"),"timeout") && !rotations);
    st=status(); p=cJSON_GetObjectItem(st,"last_pair");
    assert(cJSON_IsNull(cJSON_GetObjectItem(st,"pair")) && cJSON_GetObjectItem(p,"seq")->valueint==1);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(p,"ok")) && !strcmp(cJSON_GetObjectItem(p,"reason")->valuestring,"timeout"));
    assert(!strcmp(cJSON_GetObjectItem(p,"agent_name")->valuestring,"test agent")); cJSON_Delete(st);
    request(); sdk_gpio[6]=0; link_pair_tick(); sdk_gpio[6]=1;
    assert(!link_pair_active() && !strcmp(field("reason"),"rejected") && !rotations);
    request(); link_pair_touch(1); link_pair_tick(); link_pair_touch(0);
    assert(!link_pair_active() && cJSON_IsTrue(cJSON_GetObjectItem(sdk_sent,"ok")) && rotations==1);
    assert(!strcmp(field("link_code"),"ABCDEFGH"));
    st=status(); p=cJSON_GetObjectItem(st,"last_pair");
    assert(cJSON_GetObjectItem(p,"seq")->valueint==3 && cJSON_IsTrue(cJSON_GetObjectItem(p,"ok")) && cJSON_IsNull(cJSON_GetObjectItem(p,"reason")));
    cJSON_Delete(st);
    request(); sdk_gpio[0]=0; link_pair_tick(); sdk_gpio[0]=1;
    assert(!link_pair_active() && rotations==2);
    request(); send_error=ESP_FAIL; link_pair_touch(1); link_pair_tick(); link_pair_touch(0);
    assert(!link_pair_active() && rotations==3); send_error=ESP_OK;
    begin_say(); unsigned before=sent;
    cJSON *args=cJSON_Parse("{\"req\":\"busy\",\"agent_name\":\"test\",\"code\":\"1234\"}");
    link_pair_request(args); cJSON_Delete(args);
    assert(sent==before+1 && !strcmp(field("reason"),"rejected"));
    link_audio_cancel(); complete(play());
}
int main(void) {
    assert(link_audio_init()==ESP_OK && link_pair_init()==ESP_OK);
    test_say(); test_pair();
    cJSON_Delete(sdk_sent); free(s_jobs); free(s_done); free(s_ring);
    puts("LINK AUDIO/PAIR PASS: paced PCM, final playback drain, errors, cmd-time text/mood, pairing reasons");
}
