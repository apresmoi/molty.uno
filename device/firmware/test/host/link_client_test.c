#include "link_sdk_stub.h"
#include "../../main/link/link_client.c"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>

struct sdk_ws { bool connected; };
static void (*callback)(void *,esp_event_base_t,int32_t,void *);
static jmp_buf stopped;
static unsigned starts, stops, hellos, codes, forgets, wipes, steps, stage;
static int64_t close_at, forget_at;
static char test_code[9]="ABCDEFGH";
static unsigned scenario, injected_close;
/* activity log uploads: the {"t":"log"} frames sent, in order, and when */
static unsigned logs, log_calls;
static bool log_prev[8];
static int64_t log_at[8];
static unsigned prev_left, cur_left;      /* frames the fake ring still has */
static cJSON *fake_log_frame(bool prev, uint32_t *cursor, int max_lines, int backlog) {
    log_calls++;
    assert(max_lines==48);
    unsigned *left = prev ? &prev_left : &cur_left;
    /* the backlog bound (2 frames) applies to the first frame after a welcome only */
    static bool prev_seen, cur_seen;
    if (prev) { assert(backlog==(prev_seen ? 0 : 96)); prev_seen=true; }
    else { assert(backlog==(cur_seen ? 0 : 96)); cur_seen=true; }
    if (!*left) return NULL;
    (*left)--; (*cursor)+=10;
    cJSON *f=cJSON_CreateObject(); cJSON_AddStringToObject(f,"t","log");
    if (prev) cJSON_AddBoolToObject(f,"prev",true);
    return f;
}

static void text_event(const char *text) {
    esp_websocket_event_data_t d={.op_code=1,.data_ptr=(char *)text,.data_len=(int)strlen(text),.payload_len=(int)strlen(text),.fin=true};
    callback(NULL,NULL,WEBSOCKET_EVENT_DATA,&d);
}
static void close_event(unsigned code) {
    char bytes[2]={(char)(code>>8),(char)code};
    esp_websocket_event_data_t d={.op_code=8,.data_ptr=bytes,.data_len=2};
    callback(NULL,NULL,WEBSOCKET_EVENT_DATA,&d);
    close_at=sdk_now;
}
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *cfg) {
    assert(!strcmp(cfg->uri,"ws://192.168.1.2:8787/v1/device?id=molty-abcdefghij"));
    assert(cfg->disable_auto_reconnect && cfg->crt_bundle_attach);
    /* a radio stall rides on TCP retransmit; the component aborts when this runs out */
    assert(cfg->network_timeout_ms>=15000 && cfg->pingpong_timeout_sec>cfg->network_timeout_ms/1000);
    return calloc(1,sizeof(struct sdk_ws));
}
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t ws,int id,void (*fn)(void *,esp_event_base_t,int32_t,void *),void *arg) {
    (void)ws;(void)id;(void)arg; callback=fn; return ESP_OK;
}
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t ws) {
    starts++; ws->connected=true; callback(NULL,NULL,WEBSOCKET_EVENT_CONNECTED,NULL);
    /* scenario 5: a relay that takes log uploads (and gives its clock); else an older one */
    text_event(scenario==5 ? "{\"t\":\"welcome\",\"now\":1790000000000,\"log\":{\"max_lines\":48}}" : "{\"t\":\"welcome\"}");
    /* the relay's listener hints (cap hints): handed to link_audio; a malformed one is not */
    if (scenario==5) { text_event("{\"t\":\"hint\",\"job\":5,\"k\":\"pause\"}"); text_event("{\"t\":\"hint\",\"job\":\"j1\",\"k\":\"question\"}"); }
    return ESP_OK;
}
esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t ws) { ws->connected=false; stops++; return ESP_OK; }
esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t ws) { free(ws); return ESP_OK; }
bool esp_websocket_client_is_connected(esp_websocket_client_handle_t ws) { return ws->connected; }
int esp_websocket_client_send_text(esp_websocket_client_handle_t ws,const char *text,int n,unsigned timeout) {
    (void)ws;(void)timeout;
    cJSON *j=cJSON_Parse(text); assert(j);
    const char *type=cJSON_GetObjectItem(j,"t")->valuestring;
    if(!strcmp(type,"hello")) hellos++;
    if(!strcmp(type,"log")) {
        assert(logs<8); log_prev[logs]=cJSON_IsTrue(cJSON_GetObjectItem(j,"prev")); log_at[logs]=sdk_now; logs++;
    }
    if(!strcmp(type,"link_code")) {
        codes++;
        assert(!strcmp(cJSON_GetObjectItem(j,"code")->valuestring,test_code));
        assert(!strcmp(cJSON_GetObjectItem(j,"link_code")->valuestring,test_code));
    }
    if(!strcmp(type,"forget")) {
        forgets++; forget_at=sdk_now;
        if(scenario==2) sdk_now+=17000; /* sending consumes part of the 3 s budget */
    }
    cJSON_Delete(j); return n;
}
int esp_websocket_client_send_bin(esp_websocket_client_handle_t ws,const void *p,int n,unsigned ticks) { (void)ws;(void)p;assert(ticks>=10000);return n; }
esp_err_t link_identity_init(void) { return ESP_OK; }
const char *link_identity_id(void) { return "molty-abcdefghij"; }
const char *link_identity_code(void) { return test_code; }
bool link_identity_tick(void) { return false; }
cJSON *link_identity_hello(void) { return cJSON_Parse("{\"t\":\"hello\"}"); }
esp_err_t link_identity_forget(void) {
    wipes++;
    if(scenario==1) { assert(forgets==1 && atomic_load(&s_close_code)==4410); assert(sdk_now-forget_at<3000000); }
    if(scenario==2) { assert(forgets==1 && sdk_now-forget_at==3000000); }
    if(scenario==3) assert(!forgets);
    assert(!s_ws); /* socket stopped before changing its identity */
    longjmp(stopped,1);
}
esp_err_t link_audio_init(void) { return ESP_OK; }
esp_err_t link_pair_init(void) { return ESP_OK; }
void link_cmd_init(void) {}
void link_audio_cancel(void) {}
void link_audio_link_down(void) {}
void link_audio_resume_hello(cJSON *hello) { (void)hello; }
void link_audio_resume_welcome(const cJSON *welcome) { (void)welcome; }
static bool relay_adopts;
void link_audio_relay_adopts(bool on) { relay_adopts=on; }
void link_pair_cancel(void) {}
void link_audio_tick(void) {}
static char hint_job[64], hint_kind[16]; static unsigned hints;
void link_audio_hint(const char *job, const char *kind) { snprintf(hint_job,sizeof(hint_job),"%s",job); snprintf(hint_kind,sizeof(hint_kind),"%s",kind); hints++; }
/* link_ask.c: ticked every loop, its question cancelled whenever the session goes */
static unsigned ask_cancels, ask_ticks, ask_cancels_connected;
void link_ask_cancel(void) { ask_cancels++; if (link_client_connected()) ask_cancels_connected++; }
void link_ask_tick(void) { ask_ticks++; }
/* self_update.c: told of every welcome (a fresh image is kept on it), and the refusal
 * code a handler chose (link_cmd_refuse) is the res's error */
static unsigned welcomes;
void self_update_welcomed(void) { welcomes++; assert(link_client_connected()); }
const char *link_cmd_refusal(void) { return "bad_url"; }
void link_pair_tick(void) {}
void link_pair_status(cJSON *j) { cJSON_AddNullToObject(j,"pair"); cJSON_AddNullToObject(j,"last_pair"); }
void link_pair_request(const cJSON *args) { (void)args; assert(!"unexpected pairing"); }
esp_err_t link_audio_receive(const void *data,size_t n) { (void)data;(void)n; return ESP_OK; }
esp_err_t link_cmd_run(const char *name,const cJSON *args,cJSON **out) { (void)name;(void)args;(void)out; assert(!"unexpected command"); return ESP_FAIL; }

static void tick(void) {
    assert(++steps<100); /* deterministic watch/termination condition */
    if(scenario==0) {
        if(link_client_connected() && !stage) { strcpy(test_code,"JKLMNPQR"); stage++; }
        else if(codes) { assert(starts==1 && stops==0 && hellos==1); longjmp(stopped,1); }
    } else if(scenario==5) {
        /* the previous boot's lines first, then this boot's backlog, 2 s apart */
        if(logs==2 && !stage) {
            assert(log_prev[0] && !log_prev[1] && log_at[1]-log_at[0]>=2000000);
            assert(sdk_sync_wall==1790000000000LL && sdk_sync_up<=sdk_now/1000);
            stage=1; steps=0; cur_left=1; sdk_log_request=true;   /* a job ended: its lines go up */
        } else if(stage==1 && logs==3) {
            assert(!log_prev[2] && log_at[2]-log_at[1]>=2000000);
            /* nothing new: no more frames */
            stage=2; steps=0;
        } else if(stage==2 && steps>60) { assert(logs==3 && s_prev_done && !s_log_due); longjmp(stopped,1); }
    } else if(scenario<=3) {
        if(scenario==3 && !stage) { atomic_store(&s_connected,false); stage++; atomic_store(&s_forget,true); }
        else if(link_client_connected() && !stage) {
            httpd_req_t r={0}; assert(http_forget(&r)==ESP_OK); stage++;
        }
        if(forgets && scenario==1 && !injected_close) {
            assert(wipes==0); text_event("{\"t\":\"forgotten\"}");
            /* Forgotten text alone must not trigger the identity wipe. */
            if(sdk_now-forget_at>=200000) { injected_close=4410; close_event(4410); }
        }
    } else {
        if(link_client_connected() && stage==0) { close_event(injected_close); stage=1; }
        else if(!s_ws && stage==1) {
            assert(starts==1);
            httpd_req_t r={0}; assert(http_get(&r)==ESP_OK);
            assert(cJSON_IsTrue(cJSON_GetObjectItem(sdk_sent,"auth_failed"))==(injected_close==4401));
            assert(cJSON_GetObjectItem(sdk_sent,"close_code")->valueint==(int)injected_close);
            sdk_now=close_at+(injected_close==4401 ? 300000000LL : 1000000LL)-1; stage=2;
        } else if(stage==2) {
            assert(starts==1); sdk_now=close_at+(injected_close==4401 ? 300000000LL : 1000000LL); stage=3;
        } else if(stage==3 && starts==2 && link_client_connected()) {
            assert(!atomic_load(&s_auth)); longjmp(stopped,1);
        }
    }
}
static void run(unsigned which,unsigned code) {
    disconnect("test");
    strcpy(s_url,"ws://192.168.1.2:8787"); strcpy(test_code,"ABCDEFGH");
    atomic_store(&s_auth,false); atomic_store(&s_connected,false); atomic_store(&s_lost,false);
    atomic_store(&s_forget,false); atomic_store(&s_reconnect,false); atomic_store(&s_close_code,0);
    starts=stops=hellos=codes=forgets=wipes=steps=stage=0;
    sdk_now=close_at=forget_at=0; scenario=which; injected_close=code; sdk_wait=tick;
    logs=0; prev_left=which==5 ? 1 : 0; cur_left=which==5 ? 1 : 0;
    s_prev_done=false; s_prev_first=true; s_log_last=0; s_log_cur=s_prev_cur=0; s_log_max=0;
    if(!setjmp(stopped)) link_task(NULL);
    sdk_wait=NULL;
}
static void test_receive_bounds(void) {
    disconnect("test");
    atomic_store(&s_lost,false);
    char *data=malloc(LINK_TEXT_MAX); assert(data); memset(data,' ',LINK_TEXT_MAX);
    data[0]='{'; data[1]='}';
    esp_websocket_event_data_t d={.op_code=1,.data_ptr=data,.data_len=4096,.payload_len=8192,.fin=false};
    websocket_event(NULL,NULL,WEBSOCKET_EVENT_DATA,&d);
    d.data_ptr=data+4096; d.payload_offset=4096;
    websocket_event(NULL,NULL,WEBSOCKET_EVENT_DATA,&d);
    /* Continuation at the WebSocket frame boundary, to exactly 64 KiB. */
    d.op_code=0; d.data_ptr=data+8192; d.payload_offset=0;
    d.payload_len=LINK_TEXT_MAX-8192; d.data_len=d.payload_len; d.fin=true;
    websocket_event(NULL,NULL,WEBSOCKET_EVENT_DATA,&d);
    rx_msg_t m; assert(xQueueReceive(s_rxq,&m,0)==pdTRUE);
    assert(m.opcode==1 && m.len==LINK_TEXT_MAX && !atomic_load(&s_lost)); free(m.data);
    d.op_code=1; d.data_ptr=data; d.payload_offset=0; d.data_len=0; d.payload_len=LINK_TEXT_MAX+1;
    websocket_event(NULL,NULL,WEBSOCKET_EVENT_DATA,&d);
    assert(atomic_load(&s_lost)); disconnect("test"); atomic_store(&s_lost,false);
    d.op_code=2; d.payload_len=LINK_MESSAGE_MAX+1;
    websocket_event(NULL,NULL,WEBSOCKET_EVENT_DATA,&d);
    assert(atomic_load(&s_lost)); disconnect("test");
    d.close_status_code=4401;
    websocket_event(NULL,NULL,WEBSOCKET_EVENT_CLOSED,&d);
    assert(atomic_load(&s_auth) && atomic_load(&s_close_code)==4401);
    free(data);
}
int main(void) {
    assert(link_client_init()==ESP_OK);
    run(0,0); run(1,0); run(2,0); run(3,0);
    sdk_log_frame=fake_log_frame;
    run(4,4401); run(4,4400); run(4,4409); run(4,4410);
    /* an older relay (welcome without `log`): never asked for a frame, nothing sent */
    assert(logs==0 && log_calls==0);
    run(5,0);
    test_receive_bounds();
    disconnect("test"); free(s_rxq); cJSON_Delete(sdk_sent);
    /* every disconnect ended the question, with the link already marked down (no event) */
    assert(ask_ticks>0 && ask_cancels>0 && !ask_cancels_connected);
    assert(welcomes>0);
    assert(!relay_adopts);   /* a welcome without listen_adopt: no shake to talk */
    assert(hints==1 && !strcmp(hint_job,"j1") && !strcmp(hint_kind,"question"));
    assert(!strcmp(error_code(LINK_CMD_REFUSED),"bad_url") && !strcmp(error_code(ESP_ERR_INVALID_STATE),"busy"));
    puts("LINK CLIENT PASS: code update without reconnect, forget ack/timeout/offline, close codes, 5-minute auth retry, API status, welcome → self-update, refusal codes, log uploads only for a relay that asks (prev boot first, backlog bound, job end, 2 s gap, time sync)");
}
