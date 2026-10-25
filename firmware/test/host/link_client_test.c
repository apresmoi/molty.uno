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
    return calloc(1,sizeof(struct sdk_ws));
}
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t ws,int id,void (*fn)(void *,esp_event_base_t,int32_t,void *),void *arg) {
    (void)ws;(void)id;(void)arg; callback=fn; return ESP_OK;
}
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t ws) {
    starts++; ws->connected=true; callback(NULL,NULL,WEBSOCKET_EVENT_CONNECTED,NULL);
    text_event("{\"t\":\"welcome\"}"); return ESP_OK;
}
esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t ws) { ws->connected=false; stops++; return ESP_OK; }
esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t ws) { free(ws); return ESP_OK; }
bool esp_websocket_client_is_connected(esp_websocket_client_handle_t ws) { return ws->connected; }
int esp_websocket_client_send_text(esp_websocket_client_handle_t ws,const char *text,int n,unsigned timeout) {
    (void)ws;(void)timeout;
    cJSON *j=cJSON_Parse(text); assert(j);
    const char *type=cJSON_GetObjectItem(j,"t")->valuestring;
    if(!strcmp(type,"hello")) hellos++;
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
int esp_websocket_client_send_bin(esp_websocket_client_handle_t ws,const void *p,int n,unsigned ticks) { (void)ws;(void)p;(void)ticks;return n; }
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
void link_pair_cancel(void) {}
void link_audio_tick(void) {}
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
    run(4,4401); run(4,4400); run(4,4409); run(4,4410);
    test_receive_bounds();
    disconnect("test"); free(s_rxq); cJSON_Delete(sdk_sent);
    puts("LINK CLIENT PASS: code update without reconnect, forget ack/timeout/offline, close codes, 5-minute auth retry, API status");
}
