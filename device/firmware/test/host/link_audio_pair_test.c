#include "link_sdk_stub.h"
#include "link_protocol.h"
#include "../../main/link/link_audio.c"
#include "cute_act.h"
#include "../../main/link/link_pair.c"
#include <assert.h>
#include <stdio.h>
#include <math.h>

static unsigned rotations, sent;
static esp_err_t send_error;
static const char *field(const char *key) { cJSON *j=cJSON_GetObjectItem(sdk_sent,key); assert(cJSON_IsString(j)); return j->valuestring; }
static bool link_up=true;
bool link_client_connected(void) { return link_up; }
esp_err_t link_send_json(const cJSON *j) {
    cJSON_Delete(sdk_sent); sdk_sent=cJSON_Duplicate(j,true); sent++; return send_error;
}
static unsigned bin_frames, bin_bytes, bin_ends, bin_seq;
static char bin_reason[16]; static int bin_speech_ms;   /* -1: speech_ms absent */
static char bin_end_header[512];                          /* the exact end-frame header */
/* resend tests: sends fail while send_fail_left > 0 (or while send_fail_until says so);
 * gaps_ok: seqs may jump (frames the device dropped), never go back */
static unsigned send_fail_left, send_fails, bin_max_rt; static bool (*send_fail_until)(void); static bool gaps_ok;
static int16_t bin_first_pcm[4800]; static size_t bin_first_n; static unsigned bin_first_ct;
esp_err_t link_send_binary(const void *data,size_t size) {
    if (send_fail_left || (send_fail_until && send_fail_until())) { if (send_fail_left) send_fail_left--; send_fails++; return ESP_FAIL; }
    link_frame_t h; const uint8_t *pcm; size_t bytes;
    assert(link_frame_parse(data,size,&h,&pcm,&bytes) && size<=16384);
    assert(!bin_ends);                                    /* nothing after end */
    if (gaps_ok) { assert(h.seq>=bin_seq); bin_seq=h.seq+1; } else assert(h.seq==bin_seq++);   /* ordered */
    /* the end frame alone carries {reason, speech_ms?}: speech_ms only when measured */
    const char *nl=memchr(data,'\n',size); cJSON *hj=cJSON_ParseWithLength(data,nl?(size_t)(nl-(const char *)data):size);
    cJSON *r=cJSON_GetObjectItem(hj,"reason"), *sm=cJSON_GetObjectItem(hj,"speech_ms");
    assert(h.end ? cJSON_IsString(r) && (!sm || cJSON_IsNumber(sm)) : !r && !sm);
    /* the trace on every listen frame: ct, wq always; rt only after a failed try; tr only at the end */
    cJSON *ct=cJSON_GetObjectItem(hj,"ct"), *rt=cJSON_GetObjectItem(hj,"rt"), *tr=cJSON_GetObjectItem(hj,"tr");
    assert(cJSON_IsNumber(ct) && cJSON_IsNumber(cJSON_GetObjectItem(hj,"wq")) && (h.end ? cJSON_IsObject(tr) : !tr));
    if (rt) { assert(rt->valueint>0); if ((unsigned)rt->valueint>bin_max_rt) bin_max_rt=(unsigned)rt->valueint; }
    if (!bin_frames) { bin_first_n=bytes/2; memcpy(bin_first_pcm,pcm,bytes); bin_first_ct=(unsigned)ct->valueint; }
    if (h.end) { snprintf(bin_reason,sizeof(bin_reason),"%s",r->valuestring); bin_speech_ms=sm?sm->valueint:-1; }
    if (h.end) snprintf(bin_end_header,sizeof(bin_end_header),"%.*s",(int)(nl?(size_t)(nl-(const char *)data):size),(const char *)data);
    cJSON_Delete(hj);
    bin_frames++; bin_bytes+=bytes; bin_ends+=h.end; return ESP_OK;
}
/* a number in the end frame's "tr" (-1: absent) */
static int tr_num(const char *key) {
    cJSON *hj=cJSON_Parse(bin_end_header), *v=cJSON_GetObjectItem(cJSON_GetObjectItem(hj,"tr"),key);
    int out=cJSON_IsNumber(v)?v->valueint:-1; cJSON_Delete(hj); return out;
}
static void listen_job_with(unsigned ms, const char *extra, audio_job_t *job) {
    char a[192]; snprintf(a,sizeof(a),"{\"job\":\"listen1\",\"ms\":%u,\"rate\":16000%s}",ms,extra);
    cJSON *args=cJSON_Parse(a),*res=NULL;
    assert(link_audio_listen(args,&res)==ESP_OK); cJSON_Delete(args); cJSON_Delete(res);
    assert(xQueueReceive(s_jobs,job,0)==pdTRUE && job->listen);
    bin_frames=bin_bytes=bin_ends=bin_seq=0; bin_reason[0]=0; bin_speech_ms=-1;
}
static void listen_job(unsigned ms, audio_job_t *job) { listen_job_with(ms,"",job); }
/* cmd listen with these exact args (no ms injected): its ack's "ms", or -1 when refused */
static double listen_raw(const char *a, audio_job_t *job) {
    cJSON *args=cJSON_Parse(a),*res=NULL;
    esp_err_t e=link_audio_listen(args,&res); cJSON_Delete(args);
    if (e!=ESP_OK) { assert(!res && !link_audio_busy()); return -1; }
    cJSON *m=cJSON_GetObjectItem(res,"ms"); assert(cJSON_IsNumber(m)); double v=m->valuedouble; cJSON_Delete(res);
    assert(xQueueReceive(s_jobs,job,0)==pdTRUE && job->listen);
    bin_frames=bin_bytes=bin_ends=bin_seq=0; bin_reason[0]=0; bin_speech_ms=-1;
    return v;
}
static bool listen_rejects(const char *extra) {
    char a[192]; snprintf(a,sizeof(a),"{\"job\":\"listen1\",\"ms\":5000,\"rate\":16000%s}",extra);
    cJSON *args=cJSON_Parse(a),*res=NULL;
    esp_err_t e=link_audio_listen(args,&res); cJSON_Delete(args); cJSON_Delete(res);
    if (e==ESP_OK) { audio_job_t j; assert(xQueueReceive(s_jobs,&j,0)==pdTRUE); s_busy=false; }
    return e==ESP_ERR_INVALID_ARG && !link_audio_busy();
}
/* 0.5 s room, 1.5 s of speech-ish tone at about -25 dBFS, then room again */
static void speech_fill(int16_t *buf,size_t n,unsigned chunk) {
    static uint32_t rng=1;
    for (size_t i=0;i<n;i++) {
        rng=rng*1664525u+1013904223u;
        int noise=(int)(rng>>27)-16;                                 /* ~-65 dBFS */
        unsigned t=chunk*1600+(unsigned)i;
        int tone=(t>=8000 && t<32000) ? (int)(1800.0*sin(t*0.055)) : 0;
        buf[i]=(int16_t)(noise+tone);
    }
}
void link_identity_rotate(void) { rotations++; }
/* the screen's listening ring (mole_fluid.c) and the pending question (link_ask.c) */
static bool ring_on; static unsigned ring_ons, ring_levels; static float ring_peak, ring_last;
void mole_fluid_listen(bool on) { if (on) { assert(!ring_on); ring_ons++; } ring_on=on; }
void mole_fluid_listen_level(float l) { assert(ring_on && l>=0.f && l<=1.f); ring_levels++; ring_last=l; if (l>ring_peak) ring_peak=l; }
static int32_t ring_color = -9; static unsigned ring_colors;
void mole_fluid_listen_color(int32_t rgb) { assert(!ring_on); ring_colors++; ring_color=rgb; }
static uint32_t listen_setting = 30000;   /* the console's setting (mole_fluid.c, NVS) */
uint32_t mole_fluid_listen_window(void) { return listen_setting; }
/* the reader following a say (mole_fluid.c): every cue with the playback position it came at */
static int cue_n, cue_last=-9, cue_ends, cue_hold; static int cue_w[64]; static uint32_t cue_at[64];
void mole_fluid_text_cue(int w) {
    if (w<0) { cue_hold++; cue_last=-1; assert(!sdk_text[0]); return; }   /* held BEFORE the text is set */
    assert(w>cue_last && cue_n<64); cue_w[cue_n]=w; cue_at[cue_n++]=molty_speaker_stream_played(); cue_last=w;
}
void mole_fluid_text_cue_end(void) { cue_ends++; }
/* a say's marks (mole_fluid.c): what fired, at which playback position, after which word */
static int mark_n; static char mark_what[32][16]; static uint32_t mark_at[32]; static int mark_word[32]; static unsigned spoke;
static void mark_log(const char *w) { assert(mark_n<32); strcpy(mark_what[mark_n],w); mark_at[mark_n]=molty_speaker_stream_played(); mark_word[mark_n++]=cue_last; }
void mole_fluid_act(int a) { mark_log(cute_act_name(a)); }
/* the reader's text version (mole_fluid.c): bumped by every text set; a mood mark carries
 * the version its say set, so a newer text (an update's progress) is never tinted */
static unsigned text_ver = 7;
unsigned mole_fluid_text_ver(void) { return text_ver; }
static unsigned mark_ver;
void mole_fluid_cue_mood(int m, unsigned v) { mark_ver = v; mark_log(link_mood_name(m)); }
void mole_fluid_spoke(void) { spoke++; }
static unsigned ask_cancels;
void link_ask_cancel(void) { ask_cancels++; }
const char *link_identity_code(void) { return "ABCDEFGH"; }
static double sdk_hold_s;
esp_err_t link_cmd_mood(const char *mood,double hold_s) { strcpy(sdk_mood,mood); sdk_hold_s=hold_s; return ESP_OK; }
static unsigned mood_gen;            /* link_cmd.c's explicit-mood count (link_cmd_test.c) */
unsigned link_cmd_mood_gen(void) { return mood_gen; }
static void explicit_mood(const char *mood,double hold_s) { link_cmd_mood(mood,hold_s); mood_gen++; }
void link_cmd_mood_unless(unsigned gen0,const char *mood,double hold_s) { if (mood_gen==gen0) link_cmd_mood(mood,hold_s); }
void link_cmd_mood_idle_unless(unsigned gen0) { link_cmd_mood_unless(gen0,"idle",-1); }
static bool ask_pending;             /* link_ask.c: a question on screen */
bool link_ask_pending(void) { return ask_pending; }
static int idle_setting = 2;   /* lively (fluid_director.h FD_IDLE_LIVELY) */
int mole_fluid_idle_life(void) { return idle_setting; }
static int ack_setting = LINK_ACK_CHIRP;   /* the console's acknowledgement setting (mole_fluid.c, NVS) */
int mole_fluid_ack_mode(void) { return ack_setting; }
static uint32_t filler_setting = 2500;    /* the console's auto filler delay (mole_fluid.c, NVS) */
uint32_t mole_fluid_filler_ms(void) { return filler_setting; }
/* shake to talk: the console's setting (mole_fluid.c, NVS), an update writing the spare slot
 * (self_update.c), and link_cmd.c's refusal codes */
static bool shake_setting = true, update_busy;
bool mole_fluid_shake_talk(void) { return shake_setting; }
bool self_update_busy(void) { return update_busy; }
static const char *refused;
esp_err_t link_cmd_refuse(const char *code) { refused=code; return LINK_CMD_REFUSED; }
esp_err_t link_ui_set_text(const char *text,int len) { assert(len>=0 && len<=4096); memcpy(sdk_text,text,len+1); text_ver++; return ESP_OK; }

static cJSON *say_args(void) { return cJSON_Parse("{\"job\":\"molty-abcdefghij.j_1\",\"text\":\"hello world\",\"ms\":640,\"rate\":16000}"); }
static void begin_say(void) {
    cJSON *args=say_args(), *res=NULL;
    assert(link_audio_say(args,&res)==ESP_OK && res);
    assert(!strcmp(sdk_mood,"speaking") && sdk_hold_s<0 && !strcmp(sdk_state,"{\"state\":\"text\"}"));
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
    /* 10 ms < VAD calibration: nothing was classified, so speech_ms is unknown (absent) */
    assert(!strcmp(bin_reason,"max") && bin_speech_ms==-1);
    /* Streamed uplink: 5 s = 80000 samples in 300 ms frames, sent while recording.
     * vad:false records the full window whatever it hears. */
    listen_job_with(5000,",\"vad\":false",&job); sdk_rx_chunks=0;
    assert(!job.vad);
    complete(record(&job));
    assert(bin_bytes==160000 && bin_ends==1 && bin_frames==17 && sdk_rx_chunks==50);
    assert(!strcmp(bin_reason,"max") && !strcmp(s_end_reason,"max"));
    /* VAD off measured nothing: no speech_ms (a 0 made the relay skip STT). This exact
     * header is the fixture in services/relay/test/talk.test.ts ("vad:false end frame"). */
    assert(bin_speech_ms==-1);
    /* ...plus the turn trace (cap "trace", docs/AGENT-LINK.md "Turn trace"): ct, lv (two
     * 100 ms chunks of digital silence), wq, and the end frame's tr */
    assert(!strcmp(bin_end_header,"{\"s\":\"listen1\",\"seq\":16,\"kind\":\"pcm16\",\"rate\":16000,\"end\":true,\"reason\":\"max\","
                   "\"ct\":4800,\"lv\":\"0000\",\"wq\":0,\"tr\":{\"wall\":0,\"cap\":5000,\"sent\":17,\"fail\":0,\"drop\":0,\"dropms\":0,"
                   "\"r0\":-40,\"r1\":-40,\"gain\":8,\"pk\":0}}"));
    /* End of speech (default on): nobody speaks → the listen ends at no_speech_ms 4 s
     * (13 full frames + a 100 ms tail with end:true), the mic stops there. */
    listen_job(8000,&job); sdk_rx_chunks=0;
    assert(job.vad && job.end_silence_ms==700 && job.no_speech_ms==4000);
    complete(record(&job));
    assert(sdk_rx_chunks==40 && bin_bytes==128000 && bin_frames==14 && bin_ends==1);
    assert(!strcmp(bin_reason,"no_speech") && bin_speech_ms==0);
    /* ...an utterance 0.5..2 s → ends ~700 ms after it (not at 8 s), reason silence */
    listen_job(8000,&job); sdk_rx_chunks=0; sdk_rx_fill=speech_fill;
    complete(record(&job)); sdk_rx_fill=NULL;
    assert(sdk_rx_chunks>=27 && sdk_rx_chunks<=28 && bin_ends==1 && bin_bytes==sdk_rx_chunks*3200u);
    assert(!strcmp(bin_reason,"silence") && bin_speech_ms>=1200 && bin_speech_ms<=1500);
    /* custom tuning reaches the job; out-of-range / mistyped args are refused */
    listen_job_with(5000,",\"end_silence_ms\":300,\"no_speech_ms\":1000",&job);
    assert(job.vad && job.end_silence_ms==300 && job.no_speech_ms==1000);
    complete(record(&job));
    assert(!strcmp(bin_reason,"no_speech") && bin_bytes==32000);
    assert(listen_rejects(",\"end_silence_ms\":299") && listen_rejects(",\"end_silence_ms\":3001"));
    assert(listen_rejects(",\"no_speech_ms\":999") && listen_rejects(",\"no_speech_ms\":10001"));
    assert(listen_rejects(",\"end_silence_ms\":700.5") && listen_rejects(",\"vad\":0"));
    assert(listen_rejects(",\"no_speech_ms\":\"4000\""));
    /* Mic failure mid-stream fails fast with an end frame (relay: no 5 s wait). */
    listen_job(5000,&job); sdk_rx_error=ESP_ERR_TIMEOUT;
    esp_err_t le=record(&job); complete(le); sdk_rx_error=ESP_OK;
    assert(le==ESP_ERR_TIMEOUT && bin_frames==1 && bin_bytes==0 && bin_ends==1 && !strcmp(bin_reason,"error"));
}
/* An explicit mood that arrives while a listen / say runs outlives its completion
 * (it carries its own hold_s); with none, completion falls back to idle. */
/* The chirp itself (audio/chirp.h): 160 ms, soft edges, quiet, smooth, G5 then D6. */
static int crossings(const int16_t *b,size_t from_ms,size_t to_ms) {
    int c=0; for (size_t i=from_ms*16+1;i<to_ms*16;i++) c+=(b[i-1]<0)!=(b[i]<0); return c;
}
static void test_chirp_sound(void) {
    static int16_t b[CHIRP_SAMPLES+8];
    assert(CHIRP_SAMPLES==2560 && chirp_fill(b,0,CHIRP_SAMPLES+8)==CHIRP_SAMPLES);
    assert(chirp_fill(b,CHIRP_SAMPLES,8)==0 && chirp_fill(b,CHIRP_SAMPLES-3,8)==3);
    chirp_fill(b,0,CHIRP_SAMPLES);
    int peak=0,step=0;
    for (size_t i=0;i<CHIRP_SAMPLES;i++) {
        if (abs(b[i])>peak) peak=abs(b[i]);
        if (i && abs(b[i]-b[i-1])>step) step=abs(b[i]-b[i-1]);
    }
    assert(peak>=3000 && peak<=CHIRP_PEAK);                 /* ≈ −17 dBFS: quiet under speech */
    assert(step<2400);                                      /* no click anywhere */
    assert(abs(b[0])<50 && abs(b[CHIRP_SAMPLES-1])<50);     /* soft attack and release */
    int lo=crossings(b,5,45), hi=crossings(b,100,150);      /* 2 per cycle */
    assert(lo>=58 && lo<=68 && hi>=112 && hi<=123);          /* 784 Hz, then 1175 Hz */
    /* chunked synthesis is the same sound */
    static int16_t c[CHIRP_SAMPLES]; size_t at=0,n;
    while ((n=chirp_fill(c+at,at,256))>0) at+=n;
    assert(at==CHIRP_SAMPLES && !memcmp(b,c,sizeof(c)));
}

/* The turn acknowledgement: a listen run the way the worker runs it (record, done, then
 * the chirp), the link task completing it at the chirp's first write. */
static size_t chirp_samples; static unsigned chirp_writes; static bool cut_with_say;
static char ack_mood[16]; static double ack_hold;
static void chirp_write(const int16_t *pcm,size_t n) {
    (void)pcm;
    if (!chirp_writes++) {
        /* the listen was reported done BEFORE the chirp's first sample: thinking is up and a
         * say is accepted right away (not answered busy) */
        assert(link_audio_busy()); link_audio_tick(); assert(!link_audio_busy());
        snprintf(ack_mood,sizeof(ack_mood),"%s",sdk_mood); ack_hold=sdk_hold_s;
        if (cut_with_say) begin_say();
    }
    chirp_samples+=n;
}
/* -> the listen answer's "ack", or NULL when the listen was refused */
static const char *ack_listen_ms(unsigned ms, const char *extra, void (*fill)(int16_t *,size_t,unsigned), void (*during)(void)) {
    static char acked[16];
    char a[192]; snprintf(a,sizeof(a),"{\"job\":\"listen1\",\"ms\":%u,\"rate\":16000%s}",ms,extra);
    cJSON *args=cJSON_Parse(a),*res=NULL;
    esp_err_t e=link_audio_listen(args,&res); cJSON_Delete(args);
    if (e!=ESP_OK) { assert(e==ESP_ERR_INVALID_ARG && !res && !link_audio_busy()); return NULL; }
    cJSON *ak=cJSON_GetObjectItem(res,"ack"); assert(cJSON_IsString(ak));
    /* every listen answer reports the auto filler setting, for the relay to act on */
    cJSON *fm=cJSON_GetObjectItem(res,"filler_ms"); assert(cJSON_IsNumber(fm) && fm->valuedouble==filler_setting);
    snprintf(acked,sizeof(acked),"%s",ak->valuestring); cJSON_Delete(res);
    audio_job_t job; assert(xQueueReceive(s_jobs,&job,0)==pdTRUE && job.listen);
    bin_frames=bin_bytes=bin_ends=bin_seq=0; bin_reason[0]=0; bin_speech_ms=-1;
    if (during) during();
    sdk_rx_chunks=0; sdk_rx_fill=fill; chirp_samples=0; chirp_writes=0; ack_mood[0]=0; sdk_drained=0;
    sdk_on_speaker_write=chirp_write;
    run_job(&job);
    sdk_on_speaker_write=NULL; sdk_rx_fill=NULL;
    link_audio_tick();                       /* no chirp: the completion happens here */
    assert(!link_audio_busy() || cut_with_say);
    return acked;
}
#define ack_listen(...) ack_listen_ms(8000,__VA_ARGS__)
static void mic_fails(void) { sdk_rx_error=ESP_ERR_TIMEOUT; }
static void stop_it(void) { cJSON *a=cJSON_Parse("{\"job\":\"listen1\"}"),*r=NULL; assert(link_audio_listen_stop(a,&r)==ESP_OK); cJSON_Delete(a); cJSON_Delete(r); }
static void stop_no_speech(void) { cJSON *a=cJSON_Parse("{\"job\":\"listen1\",\"reason\":\"no_speech\"}"),*r=NULL; assert(link_audio_listen_stop(a,&r)==ESP_OK); cJSON_Delete(a); cJSON_Delete(r); }
static void stop_turn_end(void) { cJSON *a=cJSON_Parse("{\"job\":\"listen1\",\"reason\":\"turn_end\"}"),*r=NULL; assert(link_audio_listen_stop(a,&r)==ESP_OK); cJSON_Delete(a); cJSON_Delete(r); }
static void cancel_it(void) { link_audio_cancel(); }
static void mood_meanwhile(void) { explicit_mood("happy",0); }
static void ask_meanwhile(void) { ask_pending=true; }
#define CHIRPED_FULL() (chirp_writes>0 && chirp_samples==CHIRP_SAMPLES && sdk_drained==1 && !strcmp(ack_mood,"thinking") && ack_hold==LINK_ACK_THINK_S && sdk_speaker_begins==sdk_speaker_ends)
#define THINKS_QUIETLY() (!chirp_writes && !strcmp(sdk_mood,"thinking") && sdk_hold_s==LINK_ACK_THINK_S)
#define NO_ACK() (!chirp_writes && !strcmp(sdk_mood,"idle"))
static void test_ack(void) {
    const char *k;
    /* heard speech (device VAD: silence after it) → chirp + thinking, out of the box */
    k=ack_listen("",speech_fill,NULL); assert(!strcmp(k,"chirp") && !strcmp(bin_reason,"silence") && CHIRPED_FULL());
    assert(!strcmp(sdk_mood,"thinking"));     /* the chirp's own end changes nothing on screen */
    /* the relay's end of turn (listen_stop → turn_end) */
    k=ack_listen("",speech_fill,stop_it); assert(!strcmp(bin_reason,"turn_end") && CHIRPED_FULL());
    k=ack_listen("",speech_fill,stop_turn_end); assert(!strcmp(bin_reason,"turn_end") && CHIRPED_FULL());
    /* the relay's no_speech (its STT heard nobody, device VAD off): ends with that reason and is
     * never acknowledged — no chirp, no thinking — even if the mic picked up sound */
    k=ack_listen(",\"vad\":false",speech_fill,stop_no_speech); assert(!strcmp(k,"chirp") && !strcmp(bin_reason,"no_speech") && NO_ACK());
    k=ack_listen("",NULL,stop_no_speech); assert(!strcmp(bin_reason,"no_speech") && NO_ACK());
    /* the window ran out while they were talking (measured speech) */
    k=ack_listen_ms(1500,"",speech_fill,NULL); assert(!strcmp(bin_reason,"max") && bin_speech_ms>=160 && CHIRPED_FULL());
    /* nothing heard / nothing known / failed / cancelled: no acknowledgement, idle as before */
    k=ack_listen("",NULL,NULL); assert(!strcmp(bin_reason,"no_speech") && NO_ACK());
    k=ack_listen_ms(1500,",\"vad\":false",speech_fill,NULL); assert(!strcmp(bin_reason,"max") && bin_speech_ms==-1 && NO_ACK());
    k=ack_listen("",speech_fill,mic_fails); sdk_rx_error=ESP_OK; assert(!strcmp(bin_reason,"error") && NO_ACK());
    k=ack_listen("",speech_fill,cancel_it); assert(!strcmp(s_end_reason,"error") && NO_ACK());
    /* the agent opts out per listen: false → nothing; "silent" → thinking without the chirp */
    k=ack_listen(",\"ack\":false",speech_fill,NULL); assert(!strcmp(k,"off") && NO_ACK());
    k=ack_listen(",\"ack\":\"silent\"",speech_fill,NULL); assert(!strcmp(k,"thinking") && THINKS_QUIETLY());
    k=ack_listen(",\"ack\":true",speech_fill,NULL); assert(!strcmp(k,"chirp") && CHIRPED_FULL());
    assert(!ack_listen(",\"ack\":\"loud\"",speech_fill,NULL) && !ack_listen(",\"ack\":1",speech_fill,NULL));
    assert(!ack_listen(",\"ack\":null",speech_fill,NULL));
    /* the human's setting: thinking only / off (an agent can only turn it down) */
    ack_setting=LINK_ACK_THINKING;
    k=ack_listen("",speech_fill,NULL); assert(!strcmp(k,"thinking") && THINKS_QUIETLY());
    k=ack_listen(",\"ack\":true",speech_fill,NULL); assert(!strcmp(k,"thinking") && THINKS_QUIETLY());
    ack_setting=LINK_ACK_OFF;
    k=ack_listen("",speech_fill,NULL); assert(!strcmp(k,"off") && NO_ACK());
    k=ack_listen(",\"ack\":true",speech_fill,NULL); assert(!strcmp(k,"off") && NO_ACK());
    k=ack_listen(",\"ack\":\"silent\"",speech_fill,NULL); assert(!strcmp(k,"off") && NO_ACK());
    ack_setting=LINK_ACK_CHIRP;
    /* someone else took the screen during the listen: an explicit mood stays, a question stays up */
    k=ack_listen("",speech_fill,mood_meanwhile); assert(!chirp_writes && !strcmp(sdk_mood,"happy") && sdk_hold_s==0);
    k=ack_listen("",speech_fill,ask_meanwhile); assert(!chirp_writes && !strcmp(sdk_mood,"idle")); ask_pending=false;
    /* the agent's next command replaces the thinking: say → speaking, its end → idle */
    k=ack_listen("",speech_fill,NULL); assert(CHIRPED_FULL());
    begin_say(); receive_pcm(0,true); complete(play()); assert(!strcmp(sdk_mood,"idle"));
    /* no speaker: thinking only; the speaker busy elsewhere: thinking, the chirp skipped */
    sdk_caps&=~MOLTY_CAP_SPEAKER;
    k=ack_listen("",speech_fill,NULL); assert(!strcmp(k,"chirp") && THINKS_QUIETLY());
    sdk_caps=0xFFFFFFFFu;
    unsigned b0=sdk_speaker_begins; sdk_speaker_begin_error=ESP_ERR_INVALID_STATE;
    k=ack_listen("",speech_fill,NULL); assert(THINKS_QUIETLY() && sdk_speaker_begins==b0);
    sdk_speaker_begin_error=ESP_OK;
    /* a say accepted while the chirp plays cuts it (faded, the DMA flushed with silence) and
     * starts only once the speaker is free: the stub asserts one owner at a time */
    cut_with_say=true;
    k=ack_listen("",speech_fill,NULL); cut_with_say=false;
    assert(link_audio_busy() && !strcmp(sdk_mood,"speaking"));       /* the say owns the screen */
    assert(chirp_samples>0 && chirp_samples<CHIRP_SAMPLES/2 && sdk_drained==1 && sdk_speaker_begins==sdk_speaker_ends);
    receive_pcm(0,true); complete(play()); assert(!strcmp(field("name"),"say.done") && !link_audio_busy());
}

static void test_mood_survives(void) {
    audio_job_t job; listen_job(10,&job); assert(!strcmp(sdk_mood,"listening"));
    explicit_mood("happy",0);                         /* held until the next mood */
    complete(record(&job)); assert(!link_audio_busy());
    assert(!strcmp(sdk_mood,"happy") && sdk_hold_s==0);
    listen_job(10,&job); complete(record(&job));      /* nothing chosen this time */
    assert(!strcmp(sdk_mood,"idle"));
    begin_say(); explicit_mood("thinking",-1); receive_pcm(0,true); complete(play());
    assert(!strcmp(field("name"),"say.done") && !strcmp(sdk_mood,"thinking") && sdk_hold_s<0);
    /* a mood chosen before the job started is the job's to replace */
    explicit_mood("sleepy",-1); begin_say(); receive_pcm(0,true); complete(play());
    assert(!strcmp(sdk_mood,"idle"));
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
    /* Static prompt: the code alone, as one held token (no return to the creature). */
    assert(!strcmp(sdk_state,"{\"state\":\"text\",\"text\":\"1234\",\"hold\":true}"));
    st=status(); cJSON *p=cJSON_GetObjectItem(st,"pair");
    assert(!strcmp(cJSON_GetObjectItem(p,"agent_name")->valuestring,"test agent") && !strcmp(cJSON_GetObjectItem(p,"code")->valuestring,"1234"));
    assert(cJSON_GetObjectItem(p,"expires_in_s")->valueint==110 && cJSON_IsNull(cJSON_GetObjectItem(st,"last_pair"))); cJSON_Delete(st);
    sdk_now+=30000000; link_pair_tick(); assert(link_pair_active());     /* 30 s is no longer the limit */
    st=status(); assert(cJSON_GetObjectItem(cJSON_GetObjectItem(st,"pair"),"expires_in_s")->valueint==80); cJSON_Delete(st);
    sdk_now+=79999999; link_pair_tick(); assert(link_pair_active());
    sdk_now+=1; link_pair_tick();
    assert(!link_pair_active() && !strcmp(field("reason"),"timeout") && !rotations);
    assert(!strcmp(sdk_state,"{\"state\":\"blob\"}"));          /* back to what was shown */
    st=status(); p=cJSON_GetObjectItem(st,"last_pair");
    assert(cJSON_IsNull(cJSON_GetObjectItem(st,"pair")) && cJSON_GetObjectItem(p,"seq")->valueint==1);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(p,"ok")) && !strcmp(cJSON_GetObjectItem(p,"reason")->valuestring,"timeout"));
    assert(!strcmp(cJSON_GetObjectItem(p,"agent_name")->valuestring,"test agent")); cJSON_Delete(st);
    /* The demo reel was running: the restore lands on the creature, never re-enables it. */
    sdk_saved_state="{\"state\":\"effect:wave\",\"autoplay\":true,\"reel_step\":11}";
    request(); sdk_gpio[6]=0; link_pair_tick(); sdk_gpio[6]=1;
    assert(!link_pair_active() && !strcmp(field("reason"),"rejected") && !rotations);
    assert(!strcmp(sdk_state,"{\"state\":\"creature\"}") && !strstr(sdk_state,"autoplay"));
    sdk_saved_state="{\"state\":\"blob\"}";
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
    /* A board with no reject button (the 1.28): GPIO 6 means nothing, only the timeout
     * rejects; accept still works on the button with no touch at all. */
    sdk_caps &= ~(MOLTY_CAP_BTN_REJECT | MOLTY_CAP_TOUCH);
    request(); sdk_gpio[6]=0; link_pair_tick(); sdk_gpio[6]=1; assert(link_pair_active());
    sdk_gpio[0]=0; link_pair_tick(); sdk_gpio[0]=1;
    assert(!link_pair_active() && rotations==4);
    /* Neither touch nor an accept button: refused at once, nothing shown. */
    sdk_caps &= ~MOLTY_CAP_BTN_ACCEPT; sdk_state[0]='\0';
    cJSON *nope=cJSON_Parse("{\"req\":\"noway\",\"agent_name\":\"test\",\"code\":\"1234\"}");
    link_pair_request(nope); cJSON_Delete(nope);
    assert(!link_pair_active() && !strcmp(field("reason"),"rejected") && !sdk_state[0]);
    sdk_caps = 0xFFFFFFFFu;
    begin_say(); unsigned before=sent;
    cJSON *args=cJSON_Parse("{\"req\":\"busy\",\"agent_name\":\"test\",\"code\":\"1234\"}");
    link_pair_request(args); cJSON_Delete(args);
    assert(sent==before+1 && !strcmp(field("reason"),"rejected"));
    link_audio_cancel(); complete(play());
}
/* No speaker / no mic on the board: say / listen answer "unsupported" before touching
 * the mood, the text or the audio worker. */
static void test_unsupported(void) {
    strcpy(sdk_mood,"idle"); sdk_text[0]='\0';
    sdk_caps &= ~MOLTY_CAP_SPEAKER;
    cJSON *args=say_args(),*res=NULL;
    assert(link_audio_say(args,&res)==ESP_ERR_NOT_SUPPORTED && !res && !link_audio_busy());
    assert(!strcmp(sdk_mood,"idle") && !sdk_text[0]); cJSON_Delete(args);
    sdk_caps = 0xFFFFFFFFu & ~MOLTY_CAP_MIC;
    args=cJSON_Parse("{\"job\":\"listen1\",\"ms\":100,\"rate\":16000}");
    assert(link_audio_listen(args,&res)==ESP_ERR_NOT_SUPPORTED && !res && !link_audio_busy());
    assert(!strcmp(sdk_mood,"idle")); cJSON_Delete(args);
    audio_job_t job; assert(xQueueReceive(s_jobs,&job,0)!=pdTRUE);
    sdk_caps = 0xFFFFFFFFu;
}
/* The listening ring is on exactly while the mic records a listen, fed the mic level
 * per 100 ms chunk (loud while someone speaks, ~0 in a quiet room), off however the
 * listen ends; say never lights it. */
static void test_ring(void) {
    audio_job_t job; listen_job(2000,&job); ring_ons=ring_levels=0; ring_peak=0.f; sdk_rx_chunks=0;
    assert(!ring_on);                                 /* nothing until the mic actually records */
    complete(record(&job));
    assert(!ring_on && ring_ons==1 && ring_levels==sdk_rx_chunks && ring_peak<0.2f);
    listen_job(8000,&job); ring_ons=ring_levels=0; ring_peak=0.f; sdk_rx_chunks=0; sdk_rx_fill=speech_fill;
    complete(record(&job)); sdk_rx_fill=NULL;
    assert(!ring_on && ring_ons==1 && ring_levels==sdk_rx_chunks && ring_peak>0.8f && ring_last<0.2f);
    listen_job(5000,&job); sdk_rx_error=ESP_ERR_TIMEOUT;   /* a mic failure ends it too */
    complete(record(&job)); sdk_rx_error=ESP_OK; assert(!ring_on);
    listen_job(5000,&job); link_audio_cancel(); complete(record(&job)); assert(!ring_on);
    ring_ons=0; begin_say(); receive_pcm(0,true); complete(play()); assert(!ring_on && !ring_ons);
}
/* ring_color: optional "#rrggbb", set before the mic opens; every listen sets it (-1 = the
 * device default), so a colour never leaks into the next listen; anything else is
 * invalid_args before any screen change; say never touches it */
static void test_ring_color(void) {
    audio_job_t job; unsigned n=ring_colors;
    listen_job_with(10,",\"ring_color\":\"#B197fc\"",&job);
    assert(ring_colors==n+1 && ring_color==0xb197fc); complete(record(&job));
    listen_job(10,&job); assert(ring_colors==n+2 && ring_color==-1); complete(record(&job));
    listen_job_with(10,",\"ring_color\":\"#000000\"",&job); assert(ring_color==0); complete(record(&job));
    const char *bad[]={",\"ring_color\":\"violet\"",",\"ring_color\":\"#abc\"",",\"ring_color\":\"#12345g\"",
                       ",\"ring_color\":\"abcdef0\"",",\"ring_color\":11630588",",\"ring_color\":null",",\"ring_color\":\"#1234567\""};
    for (size_t i=0;i<sizeof(bad)/sizeof(*bad);i++) { unsigned c=ring_colors; strcpy(sdk_mood,"idle"); assert(listen_rejects(bad[i]) && ring_colors==c && !strcmp(sdk_mood,"idle")); }
    n=ring_colors; begin_say(); receive_pcm(0,true); complete(play()); assert(ring_colors==n);
}
/* say / listen take the screen from a pending question — only once they are accepted:
 * a refused one (bad args, busy, no hardware) leaves the question up. So does a pairing
 * that starts; a pairing refused at once does not. */
static void test_ask_cancel(void) {
    unsigned c=ask_cancels;
    begin_say(); assert(ask_cancels==c+1);
    cJSON *args=say_args(),*res=NULL;
    assert(link_audio_say(args,&res)==ESP_ERR_INVALID_STATE && ask_cancels==c+1);   /* busy */
    link_audio_cancel(); complete(play());
    cJSON_ReplaceItemInObject(args,"rate",cJSON_CreateNumber(8000));
    assert(link_audio_say(args,&res)==ESP_ERR_INVALID_ARG && ask_cancels==c+1); cJSON_Delete(args);
    assert(listen_rejects(",\"end_silence_ms\":299") && ask_cancels==c+1);
    sdk_caps &= ~MOLTY_CAP_MIC;
    args=cJSON_Parse("{\"job\":\"listen1\",\"ms\":100,\"rate\":16000}");
    assert(link_audio_listen(args,&res)==ESP_ERR_NOT_SUPPORTED && ask_cancels==c+1); cJSON_Delete(args);
    sdk_caps = 0xFFFFFFFFu;
    sdk_select_error=ESP_ERR_NO_MEM;                  /* the creature refuses the say's screen */
    args=say_args(); assert(link_audio_say(args,&res)==ESP_ERR_NO_MEM && ask_cancels==c+1); cJSON_Delete(args);
    sdk_select_error=ESP_OK;
    audio_job_t job; listen_job(10,&job); assert(ask_cancels==c+2); complete(record(&job));
    request(); assert(ask_cancels==c+3);
    link_pair_cancel();
    begin_say();                                      /* audio busy: pairing refused at once */
    args=cJSON_Parse("{\"req\":\"busy\",\"agent_name\":\"test\",\"code\":\"1234\"}");
    link_pair_request(args); cJSON_Delete(args);
    assert(!link_pair_active() && ask_cancels==c+4);
    link_audio_cancel(); complete(play());
}
/* cmd listen_stop {job}: {stopping} — true only for the running listen of that job */
static bool stop_cmd(const char *job) {
    char a[96]; snprintf(a,sizeof(a),"{\"job\":\"%s\"}",job);
    cJSON *args=cJSON_Parse(a),*res=NULL;
    assert(link_audio_listen_stop(args,&res)==ESP_OK && res); cJSON_Delete(args);
    assert(!strcmp(cJSON_GetObjectItem(res,"job")->valuestring,job));
    bool on=cJSON_IsTrue(cJSON_GetObjectItem(res,"stopping")); cJSON_Delete(res); return on;
}
static void stop_at_chunk_10(int16_t *buf,size_t n,unsigned chunk) {
    speech_fill(buf,n,chunk);
    if (chunk==10) { assert(!stop_cmd("other")); assert(stop_cmd("listen1")); }   /* the relay heard the turn end */
}
/* The relay's semantic end of turn: the listen ends within one chunk, as if the VAD fired,
 * with reason turn_end; what was recorded still goes up. Says and later listens are untouched. */
static void test_listen_stop(void) {
    audio_job_t job;
    listen_job(8000,&job); sdk_rx_chunks=0; sdk_rx_fill=stop_at_chunk_10;
    complete(record(&job)); sdk_rx_fill=NULL;
    /* 11 chunks read (the stop lands after the 11th): 3 full frames + the 2-chunk tail with end */
    assert(sdk_rx_chunks==11 && bin_bytes==11*3200u && bin_frames==4 && bin_ends==1);
    assert(!strcmp(bin_reason,"turn_end") && !strcmp(s_end_reason,"turn_end"));
    /* the ring is off; the turn was heard: Molty acknowledges it (thinking, test_ack) */
    assert(!ring_on && !link_audio_busy() && !strcmp(sdk_mood,"thinking") && sdk_hold_s==LINK_ACK_THINK_S);
    /* nothing running: a late stop is answered, and does not leak into the next listen */
    assert(!stop_cmd("listen1"));
    listen_job(1000,&job); sdk_rx_chunks=0;
    assert(!stop_cmd("listen2"));                 /* not this job */
    complete(record(&job));
    assert(!strcmp(bin_reason,"max") && sdk_rx_chunks==10);
    /* a say is never stopped by it: playback completes with say.done */
    begin_say(); assert(!stop_cmd("say1") && !stop_cmd("listen1"));
    receive_pcm(0,true); complete(play());
    assert(!strcmp(field("name"),"say.done") && !link_audio_busy());
    /* malformed */
    cJSON *bad=cJSON_Parse("{\"job\":7}"),*res=NULL;
    assert(link_audio_listen_stop(bad,&res)==ESP_ERR_INVALID_ARG && !res); cJSON_Delete(bad);
    /* reason: "turn_end" | "no_speech" (absent: turn_end); anything else is refused */
    const char *bad_reasons[]={"\"error\"","\"\"","7","null","true"};
    for (size_t i=0;i<sizeof bad_reasons/sizeof *bad_reasons;i++) {
        char a[96]; snprintf(a,sizeof(a),"{\"job\":\"listen1\",\"reason\":%s}",bad_reasons[i]);
        bad=cJSON_Parse(a); res=NULL;
        assert(link_audio_listen_stop(bad,&res)==ESP_ERR_INVALID_ARG && !res); cJSON_Delete(bad);
    }
}
/* The window: 30000 ok, 30001 refused, 0 = unlimited (records to the 300 s ceiling, reason
 * max, nothing sized by it), absent = the device's setting; the ack reports the window. */
static void test_listen_window(void) {
    audio_job_t job;
    assert(listen_raw("{\"job\":\"listen1\",\"ms\":30000,\"rate\":16000}",&job)==30000 && job.ms==30000);
    link_audio_cancel(); complete(record(&job));
    assert(listen_raw("{\"job\":\"listen1\",\"ms\":30001,\"rate\":16000}",&job)==-1);
    assert(listen_raw("{\"job\":\"listen1\",\"ms\":-1,\"rate\":16000}",&job)==-1);
    /* absent: the setting */
    listen_setting=12000;
    assert(listen_raw("{\"job\":\"listen1\",\"rate\":16000}",&job)==12000 && job.ms==12000);
    link_audio_cancel(); complete(record(&job));
    listen_setting=0;
    assert(listen_raw("{\"job\":\"listen1\",\"rate\":16000}",&job)==0 && job.ms==300000);
    link_audio_cancel(); complete(record(&job));
    listen_setting=30000;
    /* unlimited, VAD off (the voice box's end of turn decides): it records all 300 s, streamed
     * in 300 ms frames, and ends with reason max — the safety ceiling */
    assert(listen_raw("{\"job\":\"listen1\",\"ms\":0,\"rate\":16000,\"vad\":false}",&job)==0 && job.ms==300000);
    sdk_rx_chunks=0; complete(record(&job));
    assert(sdk_rx_chunks==3000 && bin_bytes==300000u*32 && bin_frames==1000 && bin_ends==1);
    assert(!strcmp(bin_reason,"max") && !ring_on && !link_audio_busy());
    /* unlimited with the VAD on: nobody speaking still ends at no_speech_ms (4 s) */
    assert(listen_raw("{\"job\":\"listen1\",\"ms\":0,\"rate\":16000}",&job)==0);
    sdk_rx_chunks=0; complete(record(&job));
    assert(!strcmp(bin_reason,"no_speech") && sdk_rx_chunks==40);
    /* ...and the relay's listen_stop ends it (turn_end) */
    assert(listen_raw("{\"job\":\"listen1\",\"ms\":0,\"rate\":16000,\"vad\":false}",&job)==0);
    sdk_rx_chunks=0; sdk_rx_fill=stop_at_chunk_10; complete(record(&job)); sdk_rx_fill=NULL;
    assert(!strcmp(bin_reason,"turn_end") && sdk_rx_chunks==11);
}
/* a say frame with word cues in its header ("w"), 5120 samples of PCM */
static esp_err_t receive_cued(unsigned seq,bool end,const char *w) {
    static uint8_t frame[LINK_MESSAGE_MAX];
    int n=snprintf((char *)frame,LINK_HEADER_MAX+1,"{\"s\":\"%s\",\"seq\":%u,\"kind\":\"pcm16\",\"rate\":16000,\"end\":%s%s%s}\n",
                   s_job,seq,end?"true":"false",w?",\"w\":":"",w?w:"");
    memset(frame+n,0x12,10240);
    return link_audio_receive(frame,n+10240);
}
static bool say_with(const char *extra) {
    char a[256]; snprintf(a,sizeof(a),"{\"job\":\"molty-abcdefghij.j_2\",\"text\":\"one two three four\",\"ms\":640,\"rate\":16000%s}",extra);
    cJSON *args=cJSON_Parse(a),*res=NULL; sdk_text[0]=0;
    esp_err_t e=link_audio_say(args,&res); cJSON_Delete(args); cJSON_Delete(res);
    if (e!=ESP_OK) return false;
    audio_job_t job; assert(xQueueReceive(s_jobs,&job,0)==pdTRUE && !job.listen); return true;
}
/* say {cues:true}: each word shows when the speaker has played up to its cue less the reveal
 * lead (CUE_LEAD_SAMPLES, the decision-to-glass latency) — never earlier than that, at most one
 * 240-sample write later — and the rest when playback is over */
static void test_say_cues(void) {
    static const uint32_t at[4]={0,3000,6000,9000};
    cue_n=0; cue_ends=cue_hold=0;
    assert(say_with(",\"cues\":true") && cue_hold==1 && !strcmp(sdk_text,"one two three four"));
    assert(receive_cued(0,false,"[[0,0],[1,3000]]")==ESP_OK);
    assert(receive_cued(1,true,"[[2,6000],[3,9000]]")==ESP_OK);
    complete(play());
    assert(cue_n>=2 && cue_ends==1 && cue_last==3);
    for (int i=0;i<cue_n;i++) {
        const uint32_t due=at[cue_w[i]];
        if (i==cue_n-1 && cue_w[i]==3) continue;          /* 9000 > the 8800 played before the drain: shown at the end */
        const uint32_t lead=16*30;
        assert(cue_at[i]+lead>=due);                       /* never more than the lead ahead of the voice */
        assert(due<=lead || cue_at[i]+lead-due<=240 || (i>0 && cue_at[i-1]+lead<at[cue_w[i]-1]));   /* within one 15 ms write (a cue under the lead: the first one) */
    }
    /* a plain say (no cues arg): the reader keeps its own pace, no cue calls */
    cue_n=0; cue_ends=cue_hold=0;
    assert(say_with("")); assert(receive_cued(0,true,"[[0,0]]")==ESP_OK); complete(play());
    assert(cue_n==0 && cue_ends==0 && cue_hold==0);
    /* cues not a bool: refused */
    assert(!say_with(",\"cues\":1") && !link_audio_busy());
    /* a malformed cue list ends the stream like any malformed frame */
    assert(say_with(",\"cues\":true"));
    assert(receive_cued(0,false,"[[0]]")==ESP_ERR_INVALID_ARG);
    complete(play()); assert(cue_ends==1 && !link_audio_busy());
}

/* say marks (cap say_marks): each fires when its word is shown — right after that word's cue,
 * at the same playback position — a mark past the last cued word when playback ends; the say
 * ends in its last mood mark (not idle) unless an explicit mood came meanwhile; never while a
 * question / update owns the screen; ignored without cues; malformed marks refuse the say */
static void test_say_marks(void) {
    static const uint32_t at[4]={0,3000,6000,9000};
    mark_n=0; cue_n=0; cue_last=-9; spoke=0;
    assert(say_with(",\"cues\":true,\"marks\":[[0,\"happy\"],[2,\"thinking\"],[2,\"nod\"],[4,\"wiggle\"]]"));
    const unsigned say_ver=text_ver; text_ver+=5;   /* another text (an update's "40%") replaced the say's meanwhile */
    assert(receive_cued(0,false,"[[0,0],[1,3000]]")==ESP_OK);
    assert(receive_cued(1,true,"[[2,6000],[3,9000]]")==ESP_OK);
    esp_err_t e=play();
    assert(mark_n==4);
    assert(!strcmp(mark_what[0],"happy") && mark_word[0]==0 && mark_at[0]+16*30>=at[0]);
    assert(mark_ver==say_ver && mark_ver!=text_ver);             /* the say's own words' version: mole_fluid tints nothing newer */
    assert(!strcmp(mark_what[1],"thinking") && !strcmp(mark_what[2],"nod"));
    assert(mark_word[1]==2 && mark_word[2]==2 && mark_at[1]+16*30>=at[2] && mark_at[1]+16*30-at[2]<=240);   /* with word 2's reveal */
    assert(!strcmp(mark_what[3],"wiggle") && mark_word[3]==3);   /* trailing: at the end */
    complete(e);
    assert(!strcmp(sdk_mood,"thinking") && sdk_hold_s==8 && spoke==1);   /* ends in its last mood, not idle */
    /* happy as the last mood keeps its own (quick) hold; an explicit mood meanwhile wins */
    mark_n=0; cue_last=-9;
    assert(say_with(",\"cues\":true,\"marks\":[[1,\"happy\"]]"));
    assert(receive_cued(0,true,"[[0,0],[1,3000],[2,6000],[3,9000]]")==ESP_OK);
    complete(play()); assert(!strcmp(sdk_mood,"happy") && sdk_hold_s<0);
    mark_n=0; cue_last=-9;
    assert(say_with(",\"cues\":true,\"marks\":[[1,\"happy\"]]"));
    assert(receive_cued(0,true,"[[0,0],[1,3000],[2,6000],[3,9000]]")==ESP_OK);
    e=play(); explicit_mood("sleepy",0); complete(e); assert(!strcmp(sdk_mood,"sleepy"));
    /* no marks: idle as before; a question / update owning the screen: none fire */
    mark_n=0; cue_last=-9;
    assert(say_with(",\"cues\":true")); assert(receive_cued(0,true,"[[0,0]]")==ESP_OK);
    unsigned g=mood_gen; complete(play()); assert(mood_gen==g && !strcmp(sdk_mood,"idle") && mark_n==0);
    update_busy=true; cue_last=-9;
    assert(say_with(",\"cues\":true,\"marks\":[[0,\"happy\"],[4,\"nod\"]]"));
    assert(receive_cued(0,true,"[[0,0],[1,3000],[2,6000],[3,9000]]")==ESP_OK);
    complete(play()); update_busy=false;
    assert(mark_n==0 && !strcmp(sdk_mood,"idle"));
    /* without cues the marks are ignored; a cancelled say fires none of the rest, no smile */
    assert(say_with(",\"marks\":[[0,\"happy\"]]")); assert(receive_cued(0,true,"[[0,0]]")==ESP_OK);
    complete(play()); assert(mark_n==0);
    spoke=0; cue_last=-9;
    assert(say_with(",\"cues\":true,\"marks\":[[3,\"happy\"],[4,\"nod\"]]"));
    assert(receive_cued(0,true,"[[0,0],[1,3000],[2,6000],[3,9000]]")==ESP_OK);
    link_audio_cancel(); complete(play()); assert(mark_n==0 && spoke==0);
    /* malformed marks: the say is refused, nothing changes */
    assert(!say_with(",\"cues\":true,\"marks\":[[0,\"dance\"]]") && !link_audio_busy());
    assert(!say_with(",\"cues\":true,\"marks\":{}") && !link_audio_busy());
}

/* Shake to talk: the shake starts a listen at once, on the device (mood listening, ring up);
 * the shake event names it; nothing goes up until the relay adopts it — then the relay's job
 * id, window and end-of-turn rule apply from the shake's first sample (what was recorded
 * replays first). Declined, never adopted, or the link gone: it ends quietly. Never over
 * another owner (say/listen, a question, a pairing, an update), never with the setting off,
 * never with a relay that does not adopt. */
#define RELAY_JOB "molty-abcdefghij.j_shake01"
static int adopt_at = -1, decline_at = -1, cancel_at = -1, stop_at = -1; static const char *adopt_args;
static esp_err_t adopt_e; static char adopt_ack[16]; static double adopt_ms;
/* the IMU while a shake's listen records (sense task → link_audio_imu, 50 Hz): shaken (3 g
 * strokes) until shake_imu_ms, still after; -2 = shaken all along; -3 = no IMU samples at all.
 * Handling noise (full-scale rattle) fills the audio while it is shaken. */
static int shake_imu_ms = 0;
static void shake_imu(unsigned chunk, int16_t *buf, size_t n) {
    if (shake_imu_ms == -3) return;
    const uint32_t t0 = s_settle.t0;
    for (unsigned k = 0; k < 5; k++) {
        const unsigned t = chunk * 100 + k * 20;
        const bool shaken = shake_imu_ms == -2 || (int)t < shake_imu_ms;
        link_audio_imu(shaken ? (k & 1 ? 3.f : -3.f) : 0.f, 0.f, shaken ? 0.5f : 1.f, t0 + t);
    }
    if (shake_imu_ms == 0) return;
    for (size_t i = 0; i < n; i++) {
        const unsigned t = (chunk * 1600 + (unsigned)i) / 16;
        if (shake_imu_ms == -2 || (int)t < shake_imu_ms) buf[i] = (int16_t)(i & 1 ? 30000 : -30000);
    }
}
static void shake_fill(int16_t *buf,size_t n,unsigned chunk) {
    speech_fill(buf,n,chunk);
    shake_imu(chunk,buf,n);
    if ((int)chunk==adopt_at) {
        char a[256]; snprintf(a,sizeof(a),"{\"job\":\"" RELAY_JOB "\",\"rate\":16000,\"adopt\":\"shake-%u\"%s}",s_shake_n,adopt_args?adopt_args:"");
        cJSON *args=cJSON_Parse(a),*res=NULL;
        adopt_e=link_audio_listen(args,&res); cJSON_Delete(args);
        if (res) { snprintf(adopt_ack,sizeof(adopt_ack),"%s",cJSON_GetObjectItem(res,"ack")->valuestring); adopt_ms=cJSON_GetObjectItem(res,"ms")->valuedouble;
                   assert(cJSON_GetObjectItem(res,"filler_ms")->valuedouble==filler_setting); }   /* the adopted listen reports the filler setting too */
        cJSON_Delete(res);
    }
    if ((int)chunk==decline_at) { char id[24]; snprintf(id,sizeof(id),"shake-%u",s_shake_n); assert(stop_cmd(id)); }
    if ((int)chunk==cancel_at) link_audio_cancel();
    if ((int)chunk==stop_at) assert(stop_cmd(RELAY_JOB));   /* the relay's end of turn, by its own job id */
}
/* a shake handed over → the link task's tick: the shake event it sent, and the listen it queued */
static bool shake(audio_job_t *job) {
    unsigned before=sent;
    if (!link_audio_shake(2.4,5,900)) return false;
    link_audio_tick();
    assert(sent==before+1 && !strcmp(field("t"),"evt") && !strcmp(field("name"),"shake"));
    cJSON *d=cJSON_GetObjectItem(sdk_sent,"data"), *l=cJSON_GetObjectItem(d,"listen");
    assert(cJSON_GetObjectItem(d,"peak_g")->valuedouble==2.4 && cJSON_GetObjectItem(d,"reversals")->valueint==5);
    bin_frames=bin_bytes=bin_ends=bin_seq=0; bin_reason[0]=0; bin_speech_ms=-1;
    if (!l) { assert(xQueueReceive(s_jobs,job,0)!=pdTRUE); return true; }
    char id[24]; snprintf(id,sizeof(id),"shake-%u",s_shake_n);
    assert(cJSON_IsString(l) && !strcmp(l->valuestring,id));
    assert(xQueueReceive(s_jobs,job,0)==pdTRUE && job->listen && job->pending && !strcmp(job->job,id));
    assert(link_audio_busy() && !strcmp(sdk_mood,"listening") && ring_color==-1);
    return true;
}
static bool talked(void) { return cJSON_GetObjectItem(cJSON_GetObjectItem(sdk_sent,"data"),"listen")!=NULL; }
static void shake_run(audio_job_t *job,int adopt,const char *args) {
    adopt_at=adopt; adopt_args=args; adopt_e=-99; adopt_ack[0]=0; sdk_rx_chunks=0; sdk_rx_fill=shake_fill;
    complete(record(job)); sdk_rx_fill=NULL; adopt_at=decline_at=cancel_at=stop_at=-1; adopt_args=NULL;
}
/* a tap: the rec stops with what it has, reason tap, acknowledged; a bound `talk`; a say's first-audio watchdog */
static void stop_tap_at_chunk_10(int16_t *buf, size_t n, unsigned i) { (void)buf; (void)n; if (i==10) { link_audio_request(link_audio_job_gen(), LINK_AUDIO_REQ_STOP); apply_request(); } }
/* a link drop: a held job survives it, the hello names it, the welcome picks it up or drops it */
static cJSON *welcome_with(const char *job, bool ok, int next) {
    cJSON *w=cJSON_CreateObject(), *f=cJSON_AddArrayToObject(w,"features");
    cJSON_AddItemToArray(f,cJSON_CreateString("resume"));
    if (job) { cJSON *r=cJSON_AddObjectToObject(w,"resume"); cJSON_AddStringToObject(r,"job",job); cJSON_AddBoolToObject(r,"ok",ok);
      if (next>=0) cJSON_AddNumberToObject(r,"next",next); }
    return w;
}
static void relay_resumes(bool on) { cJSON *w=cJSON_CreateObject(), *f=cJSON_AddArrayToObject(w,"features");
    if (on) cJSON_AddItemToArray(f,cJSON_CreateString("resume")); link_audio_resume_welcome(w); cJSON_Delete(w); }
static char drop_job[LINK_STREAM_MAX]; static int drop_at_chunk=-1, back_after_delays=-1, drop_next; static bool dropped;
static void drop_back(void) {
    link_up=true;
    { unsigned before=bin_frames; up_kick(); assert(bin_frames==before); }   /* nothing goes before the relay says from where */
    bin_seq=drop_next; bin_bytes=0;
    cJSON *w=welcome_with(drop_job,true,drop_next); link_audio_resume_welcome(w); cJSON_Delete(w);
}
static void drop_fill(int16_t *buf, size_t n, unsigned chunk) {
    (void)buf; (void)n;
    if ((int)chunk==drop_at_chunk && !dropped) { dropped=true; link_audio_link_down(); link_up=false; assert(link_audio_held()); }
}
static void drop_delay(void) {
    if (!dropped) return;
    if (back_after_delays>0 && --back_after_delays==0) { drop_back(); dropped=false; }
    if (back_after_delays<0) link_audio_tick();
}
static void test_link_drop(void) {
    /* a relay that cannot hold: as before */
    relay_resumes(false);
    begin_say(); link_audio_link_down(); assert(atomic_load(&s_cancel) && !link_audio_held()); complete(play()); link_audio_tick();
    /* a say mid-stream: held, named with the next frame it needs; resumed; the stream goes on */
    relay_resumes(true);
    begin_say(); receive_pcm(0,false); receive_pcm(1,false);
    link_audio_link_down(); assert(!atomic_load(&s_cancel) && link_audio_held());
    cJSON *h=cJSON_CreateObject(); link_audio_resume_hello(h);
    cJSON *r=cJSON_GetObjectItem(h,"resume");
    assert(r && !strcmp(cJSON_GetObjectItem(r,"job")->valuestring,s_job) && !strcmp(cJSON_GetObjectItem(r,"kind")->valuestring,"say") && cJSON_GetObjectItem(r,"next")->valueint==2);
    cJSON_Delete(h);
    cJSON *w=welcome_with(s_job,true,-1); link_audio_resume_welcome(w); cJSON_Delete(w);
    assert(!link_audio_held() && !atomic_load(&s_cancel));
    receive_pcm(2,true); assert(!atomic_load(&s_cancel)); complete(play()); link_audio_tick();
    /* refused: dropped */
    begin_say(); link_audio_link_down(); assert(link_audio_held());
    w=welcome_with(s_job,false,-1); link_audio_resume_welcome(w); cJSON_Delete(w);
    assert(!link_audio_held() && atomic_load(&s_cancel)); complete(play()); link_audio_tick();
    /* a welcome that does not name it (a relay that forgot): dropped */
    begin_say(); link_audio_link_down(); relay_resumes(true); assert(atomic_load(&s_cancel) && !link_audio_held()); complete(play()); link_audio_tick();
    /* the window: past it the job goes, and a second drop does not restart the clock */
    begin_say(); receive_pcm(0,false); link_audio_link_down();
    sdk_now+=LINK_RESUME_WINDOW_MS*1000ull/2; s_last_frame=sdk_now; link_audio_tick(); assert(!atomic_load(&s_cancel));
    link_audio_link_down();
    sdk_now+=LINK_RESUME_WINDOW_MS*1000ull/2+1000; s_last_frame=sdk_now; link_audio_tick();
    assert(atomic_load(&s_cancel) && !link_audio_held()); complete(play()); link_audio_tick();
    /* a say played out offline: the next hello says so, once */
    begin_say(); strcpy(s_job,"j_off"); link_audio_link_down(); link_up=false; complete(ESP_OK);
    h=cJSON_CreateObject(); link_audio_resume_hello(h); r=cJSON_GetObjectItem(h,"resume");
    assert(r && !strcmp(cJSON_GetObjectItem(r,"job")->valuestring,"j_off") && cJSON_IsTrue(cJSON_GetObjectItem(r,"done"))); cJSON_Delete(h);
    relay_resumes(true); link_up=true;
    h=cJSON_CreateObject(); link_audio_resume_hello(h); assert(!cJSON_GetObjectItem(h,"resume")); cJSON_Delete(h);
    /* a listen: recording goes on through the drop and past its end; nothing lost, the relay's next wins */
    relay_resumes(true);
    audio_job_t job; listen_job_with(3000,",\"vad\":false",&job); strcpy(drop_job,s_job); sdk_rx_chunks=0;
    drop_at_chunk=6; back_after_delays=3; drop_next=1; sdk_rx_fill=drop_fill; sdk_delay_hook=drop_delay;
    complete(record(&job)); sdk_rx_fill=NULL; sdk_delay_hook=NULL;
    assert(bin_ends==1 && !link_audio_held() && !strcmp(bin_reason,"max"));
    assert(bin_bytes==96000-9600);    /* frame 1 (the relay's next) to the end: 10 frames, the first one kept out */
    assert(tr_num("drop")==0);
    /* the link never comes back: the tick's window ends it, the flush gives up, nothing goes */
    listen_job_with(600,",\"vad\":false",&job); strcpy(drop_job,s_job); sdk_rx_chunks=0;
    drop_at_chunk=2; back_after_delays=-1; sdk_rx_fill=drop_fill; sdk_delay_hook=drop_delay; bin_frames=0;
    complete(record(&job)); sdk_rx_fill=NULL; sdk_delay_hook=NULL;
    assert(!link_audio_held()); link_up=true;
    link_audio_tick();
    relay_resumes(false);
}

static void test_tap_and_watchdog(void) {
    audio_job_t job; uint32_t age=99;
    listen_job(8000,&job); assert(link_audio_listening(&age)); sdk_rx_chunks=0; sdk_rx_fill=stop_tap_at_chunk_10;
    complete(record(&job)); sdk_rx_fill=NULL;
    assert(bin_ends==1 && !strcmp(bin_reason,"tap") && !strcmp(s_end_reason,"tap") && bin_bytes>0);
    assert(!link_audio_busy() && !strcmp(sdk_mood,"thinking"));        /* acknowledged: a normal end of turn */
    assert(!link_audio_listening(NULL));
    /* no audio yet: waiting, gives up after 8 s; the first frame ends the wait */
    begin_say(); assert(link_audio_say_waiting());
    sdk_now+=7000000; link_audio_tick(); assert(!atomic_load(&s_cancel));
    sdk_now+=1100000; link_audio_tick(); assert(atomic_load(&s_cancel));
    complete(play()); assert(!link_audio_busy() && !strcmp(sdk_mood,"idle"));
    begin_say(); receive_pcm(0,false); assert(!link_audio_say_waiting());
    sdk_now+=9000000; link_audio_tick(); assert(!atomic_load(&s_cancel));       /* mid-stream: the 30 s one */
    sdk_now+=21100000; link_audio_tick(); assert(atomic_load(&s_cancel));
    complete(play());
    /* say_cancel: only that say */
    begin_say(); cJSON *a=cJSON_CreateObject(),*r=NULL; cJSON_AddStringToObject(a,"job","other");
    assert(link_audio_say_cancel(a,&r)==ESP_OK && !cJSON_IsTrue(cJSON_GetObjectItem(r,"cancelling")) && !atomic_load(&s_cancel)); cJSON_Delete(r);
    cJSON_ReplaceItemInObject(a,"job",cJSON_CreateString("molty-abcdefghij.j_1")); r=NULL;
    assert(link_audio_say_cancel(a,&r)==ESP_OK && cJSON_IsTrue(cJSON_GetObjectItem(r,"cancelling")) && atomic_load(&s_cancel)); cJSON_Delete(r); cJSON_Delete(a);
    complete(play());
    /* a listen: say_cancel leaves it alone */
    listen_job(1000,&job); a=cJSON_CreateObject(); cJSON_AddStringToObject(a,"job",job.job); r=NULL;
    assert(link_audio_say_cancel(a,&r)==ESP_OK && !cJSON_IsTrue(cJSON_GetObjectItem(r,"cancelling")) && !atomic_load(&s_cancel)); cJSON_Delete(r); cJSON_Delete(a);
    complete(record(&job));
    /* stop: a listen ends and sends, a say is cut */
    begin_say(); link_audio_stop_any(); apply_request(); assert(atomic_load(&s_cancel)); complete(play());
    /* a tap on a job that ended is not applied to the next one */
    begin_say(); { uint32_t old=link_audio_job_gen(); link_audio_cancel(); complete(play()); link_audio_tick();
      begin_say(); link_audio_request(old, LINK_AUDIO_REQ_CANCEL); apply_request(); assert(!atomic_load(&s_cancel));
      link_audio_request(link_audio_job_gen(), LINK_AUDIO_REQ_CANCEL); apply_request(); assert(atomic_load(&s_cancel)); complete(play()); link_audio_tick(); }
    /* a bound talk is the shake's listen even with the shake setting off; nothing to report when none starts */
    shake_setting=false; sdk_caps&=~MOLTY_CAP_IMU; link_audio_relay_adopts(true);
    unsigned b=sent; assert(link_audio_gesture_talk()); link_audio_tick();
    assert(link_audio_busy() && sent==b+1 && !strcmp(field("name"),"shake"));
    assert(cJSON_IsNumber(cJSON_GetObjectItem(cJSON_GetObjectItem(sdk_sent,"data"),"peak_g")));
    b=sent; assert(link_audio_gesture_talk()); link_audio_tick(); assert(sent==b);   /* busy: no plain shake nudge */
    sdk_rx_fill=NULL; audio_job_t got; assert(xQueueReceive(s_jobs,&got,0)==pdTRUE); link_audio_cancel(); complete(record(&got));
    shake_setting=true; sdk_caps|=MOLTY_CAP_IMU; link_audio_relay_adopts(false);
}

static void test_shake_talk(void) {
    audio_job_t job;
    /* the adoption flow, sample for sample: on a board without an IMU (no settle trim —
     * test_shake_settle pins the trim) */
    sdk_caps&=~MOLTY_CAP_IMU;
    link_audio_relay_adopts(true);
    /* adopted after 5 chunks (500 ms) with semantic end of turn (vad:false): the relay's job id
     * on every frame, every sample from the shake on (21 chunks read = 21 chunks up: the first
     * 5 replayed), ended by the relay's listen_stop on ITS job id → turn_end, acknowledged */
    listen_setting=5000;
    assert(shake(&job) && talked());
    unsigned b=sent;
    stop_at=20; shake_run(&job,5,",\"vad\":false");
    assert(adopt_e==ESP_OK && !strcmp(adopt_ack,"chirp") && adopt_ms==5000 && sent==b);   /* nothing but frames went up */
    assert(sdk_rx_chunks==21 && bin_bytes==21*3200u && bin_ends==1 && !strcmp(bin_reason,"turn_end"));
    assert(strstr(bin_end_header,"\"s\":\"" RELAY_JOB "\""));
    assert(!link_audio_busy() && !ring_on && !strcmp(sdk_mood,"thinking"));   /* heard: acknowledged */
    /* activity log: the adopted listen's lines carry the relay's job, ended with it */
    assert(!strcmp(sdk_job_last_end, RELAY_JOB));
    /* vad:false and nobody ends it: the device's 5 s window, counted from the shake */
    assert(shake(&job)); shake_run(&job,5,",\"vad\":false");
    assert(bin_bytes==160000 && !strcmp(bin_reason,"max") && sdk_rx_chunks==50);
    /* adopted with the device's own VAD (the cloud path): it ends on the silence after speech */
    assert(shake(&job)); shake_run(&job,2,"");
    assert(adopt_e==ESP_OK && !strcmp(bin_reason,"silence") && bin_ends==1 && bin_bytes<160000);
    /* the agent's ack applies: false → no acknowledgement */
    assert(shake(&job)); shake_run(&job,1,",\"vad\":false,\"ack\":false");
    assert(adopt_e==ESP_OK && !strcmp(adopt_ack,"off") && !strcmp(sdk_mood,"idle"));
    /* nobody adopts it: after LINK_SHAKE_ADOPT_MS of audio it ends — no frame, no chirp, idle */
    assert(shake(&job)); b=sent; shake_run(&job,-1,"");
    assert(sdk_rx_chunks==LINK_SHAKE_ADOPT_MS/100 && !bin_frames && !strcmp(s_end_reason,"unclaimed"));
    assert(sent==b && s_ack==LINK_ACK_OFF && !strcmp(sdk_mood,"idle") && !link_audio_busy() && !ring_on);
    /* ...and a late adoption is refused listen_gone (never busy) */
    char a[160]; snprintf(a,sizeof(a),"{\"job\":\"" RELAY_JOB "\",\"rate\":16000,\"adopt\":\"shake-%u\"}",s_shake_n);
    cJSON *args=cJSON_Parse(a),*res=NULL; refused=NULL;
    assert(link_audio_listen(args,&res)==LINK_CMD_REFUSED && !res && !strcmp(refused,"listen_gone") && !link_audio_busy());
    /* the relay declines it (listen_stop naming the local id): it ends at once, quietly */
    assert(shake(&job)); decline_at=2; shake_run(&job,-1,"");
    assert(sdk_rx_chunks==3 && !bin_frames && !strcmp(s_end_reason,"declined") && !strcmp(sdk_mood,"idle"));
    /* the link went down: ends quietly too */
    assert(shake(&job)); cancel_at=1; shake_run(&job,-1,"");
    assert(!bin_frames && !strcmp(s_end_reason,"unclaimed") && !strcmp(sdk_mood,"idle"));
    /* an adoption naming another listen, or a malformed one: refused, the shake's listen still waits */
    assert(shake(&job));
    cJSON_Delete(args); args=cJSON_Parse("{\"job\":\"" RELAY_JOB "\",\"rate\":16000,\"adopt\":\"shake-999\"}"); res=NULL;
    assert(link_audio_listen(args,&res)==LINK_CMD_REFUSED && !res && atomic_load(&s_pend)==PEND_WAIT);
    cJSON_Delete(args); args=cJSON_Parse("{\"job\":\"" RELAY_JOB "\",\"rate\":8000,\"adopt\":\"x\"}");
    assert(link_audio_listen(args,&res)==ESP_ERR_INVALID_ARG && !res && atomic_load(&s_pend)==PEND_WAIT);
    cJSON_Delete(args); args=cJSON_Parse("{\"job\":\"" RELAY_JOB "\",\"rate\":16000,\"adopt\":7}");
    assert(link_audio_listen(args,&res)==ESP_ERR_INVALID_ARG && !res); cJSON_Delete(args);
    /* a plain listen while it waits: busy, as ever */
    args=cJSON_Parse("{\"job\":\"listen9\",\"rate\":16000}");
    assert(link_audio_listen(args,&res)==ESP_ERR_INVALID_STATE && !res); cJSON_Delete(args);
    shake_run(&job,-1,"");
    /* never over another owner: a say playing, a question up, a pairing, an update — the plain
     * shake goes up instead (no listen) */
    begin_say(); assert(shake(&job) && !talked()); receive_pcm(0,true); complete(play());
    ask_pending=true; assert(shake(&job) && !talked() && !link_audio_busy()); ask_pending=false;
    update_busy=true; assert(shake(&job) && !talked() && !link_audio_busy()); update_busy=false;
    request(); assert(shake(&job) && !talked() && !link_audio_busy());
    link_pair_cancel(); assert(!link_pair_active());
    /* a relay that does not adopt (its welcome had no listen_adopt): the plain shake */
    link_audio_relay_adopts(false); assert(shake(&job) && !talked() && !link_audio_busy());
    link_audio_relay_adopts(true);
    /* the setting off, or no mic: not taken at all — the sense task sends the plain shake */
    shake_setting=false; b=sent; assert(!link_audio_shake(2.4,5,900)); link_audio_tick(); assert(sent==b && !link_audio_busy());
    shake_setting=true;
    sdk_caps&=~MOLTY_CAP_MIC; assert(!link_audio_shake(2.4,5,900)); sdk_caps=0xFFFFFFFFu&~MOLTY_CAP_IMU;
    /* back on: it talks again */
    assert(shake(&job) && talked()); shake_run(&job,-1,"");
    listen_setting=30000; sdk_caps=0xFFFFFFFFu;
}

/* The shake's handling noise (shake_settle.h): the listen's audio begins where the IMU shows
 * the shake settled (still for 150 ms, the run's start; 600 ms at most); the trimmed lead is
 * never sent, the first kept 10 ms fade in, the end frame's tr says how much went. */
static void test_shake_settle(void) {
    audio_job_t job;
    link_audio_relay_adopts(true); listen_setting=5000;
    /* shaken for 350 ms: still from the 360 ms sample on, confirmed 150 ms later → 510 ms trimmed
     * (adopted at 200 ms, before it settled: the replay waits for the settle) */
    shake_imu_ms=350;
    assert(shake(&job)); stop_at=20; shake_run(&job,2,",\"vad\":false");
    assert(adopt_e==ESP_OK && bin_ends==1 && !strcmp(bin_reason,"turn_end"));
    assert(bin_bytes==(21u*1600u-510u*16u)*2u);                     /* every sample after the settle, none before */
    assert(tr_num("set")==510 && tr_num("setc")==0 && bin_first_ct==510);
    assert(bin_first_pcm[0]==0);                                     /* faded in */
    int peak=0; for (size_t i=0;i<bin_first_n;i++) { int a=abs(bin_first_pcm[i]); if (a>peak) peak=a; }
    assert(peak<2000);                                               /* no 30000 rattle went up */
    /* shaken all along: capped at 600 ms */
    shake_imu_ms=-2;
    assert(shake(&job)); stop_at=20; shake_run(&job,2,",\"vad\":false");
    assert(tr_num("set")==600 && tr_num("setc")==1 && bin_bytes==(21u*1600u-600u*16u)*2u && bin_first_ct==600);
    /* no IMU sample at all (a stalled sensor): the cap, on the audio's clock */
    shake_imu_ms=-3;
    assert(shake(&job)); stop_at=20; shake_run(&job,2,",\"vad\":false");
    assert(tr_num("set")==600 && tr_num("setc")==1);
    /* a board without an IMU: nothing trimmed (no shake listen is possible there anyway) */
    sdk_caps&=~MOLTY_CAP_IMU;
    assert(shake(&job)); stop_at=20; shake_run(&job,2,",\"vad\":false");
    assert(tr_num("set")==0 && tr_num("setc")==0 && bin_bytes==21u*3200u && bin_first_ct==0);
    sdk_caps=0xFFFFFFFFu; shake_imu_ms=0; listen_setting=30000;
}

/* The uplink never waits on the network and never loses a frame it could still send: a send
 * that fails keeps its frame, the same seq goes again, in order, and the listen ends normally
 * (it used to end in error). A ring overflow drops the OLDEST unsent frames (a seq gap,
 * counted in tr); cancel drops everything. */
static unsigned rx_chunk_limit;
static bool fail_while_recording(void) { return sdk_rx_open && sdk_rx_chunks < rx_chunk_limit; }
static void test_resend(void) {
    audio_job_t job;
    /* 4 failed tries early in a 5 s listen: all 17 frames arrive, in order; the end says fail=4 */
    listen_job_with(5000,",\"vad\":false",&job); send_fail_left=4; send_fails=0; bin_max_rt=0;
    complete(record(&job));
    assert(send_fails==4 && bin_frames==17 && bin_bytes==160000 && bin_ends==1 && !strcmp(bin_reason,"max"));
    assert(bin_max_rt==4 && tr_num("fail")==4 && tr_num("drop")==0 && tr_num("sent")==17);
    /* the link stalls for a whole 30 s window (100 frames): the ring keeps 66 (19.8 s), the
     * oldest 34 go (seq 0..33, 10.2 s), the rest goes up in order once the link is back */
    listen_job_with(30000,",\"vad\":false",&job); sdk_rx_chunks=0; rx_chunk_limit=300; send_fail_until=fail_while_recording; gaps_ok=true;
    complete(record(&job)); send_fail_until=NULL;
    assert(bin_ends==1 && bin_frames==UPLINK_SLOTS && !strcmp(bin_reason,"max") && tr_num("drop")==34 && tr_num("dropms")==10200);
    assert(bin_first_ct==10200);                                     /* the first frame kept: seq 17 */
    /* stalled past the flush deadline (3 s + 1 s for the end frame): it gives up, nothing more */
    listen_job_with(1000,",\"vad\":false",&job); send_fail_left=100000; gaps_ok=false;
    complete(record(&job)); assert(!bin_frames && !link_audio_busy()); send_fail_left=0;
    /* the next listen starts clean: seq 0, nothing of the old queue */
    listen_job_with(1000,",\"vad\":false",&job); complete(record(&job));
    assert(bin_frames==4 && bin_ends==1 && tr_num("drop")==0 && tr_num("fail")==0);
    /* the sender's back-off belongs to its listen: a new listen (a new generation) sends its
     * first frame at once, not after the old listen's back-off (the sender task's path) */
    s_up_inline=false; bin_frames=bin_seq=bin_ends=0;
    up_begin("old"); uplink_meta_t m={.seq=0,.n=0}; up_lock(); uplink_push(&s_up,&m,NULL); up_unlock();
    send_fail_left=1; up_pump(); assert(send_fails && !bin_frames && s_up_retry_at>esp_timer_get_time()/1000);
    up_pump(); assert(!bin_frames);                                  /* still backing off */
    up_begin("new"); up_lock(); uplink_push(&s_up,&m,NULL); up_unlock();
    up_pump(); assert(bin_frames==1 && !uplink_pending(&s_up));     /* same instant: sent */
    s_up_inline=true;
}

/* Listener reactions (listen_react.h) inside a real listen: the box's hints and the mic level
 * drive mole_fluid_act; never with idle life off or a question on screen. */
static int hint_at_chunk=-1; static const char *hint_kind="pause"; static int react_speech=-1;   /* first chunk of 0.8 s of voice, -1 none */
static bool react_talk=false;      /* a voice the whole listen: 300 ms of speech, a 100 ms dip, … (no pause nods) */
static int ext_at_chunk=-1;        /* the relay's reaction (a 2.2 s peek) arrives at this chunk */
static void react_fill(int16_t *buf,size_t n,unsigned chunk) {
    sdk_now+=100000;                                                 /* 100 ms per chunk */
    memset(buf,0,n*2);
    if ((react_speech>=0 && (int)chunk>=react_speech && (int)chunk<react_speech+8) || (react_talk && chunk%4!=3)) for (size_t i=0;i<n;i++) buf[i]=(int16_t)(3000.0*sin((chunk*1600+i)*0.06));
    if ((int)chunk==ext_at_chunk) link_audio_external_react(2200);
    if ((int)chunk==hint_at_chunk) link_audio_hint("listen1",hint_kind);
    if (chunk==hint_at_chunk+1u) link_audio_hint("someone-else",hint_kind);   /* not the running listen: ignored */
}
static void test_reactions(void) {
    audio_job_t job;
    /* a pause hint 1.5 s in: one nod */
    mark_n=0; listen_job_with(3000,",\"vad\":false",&job); sdk_rx_fill=react_fill; hint_at_chunk=15; hint_kind="pause";
    sdk_rx_chunks=0; complete(record(&job));
    assert(mark_n==1 && !strcmp(mark_what[0],"nod"));
    /* a question: a glance */
    mark_n=0; listen_job_with(3000,",\"vad\":false",&job); hint_kind="question";
    sdk_rx_chunks=0; complete(record(&job));
    assert(mark_n==1 && !strncmp(mark_what[0],"glance_",7));
    /* in the first second: nothing */
    mark_n=0; listen_job_with(3000,",\"vad\":false",&job); hint_at_chunk=3; hint_kind="pause";
    sdk_rx_chunks=0; complete(record(&job)); assert(mark_n==0);
    /* idle life off: nothing; a question on screen: nothing */
    idle_setting=0; mark_n=0; listen_job_with(3000,",\"vad\":false",&job); hint_at_chunk=15;
    sdk_rx_chunks=0; complete(record(&job)); assert(mark_n==0); idle_setting=2;
    ask_pending=true; mark_n=0; listen_job_with(3000,",\"vad\":false",&job);
    sdk_rx_chunks=0; complete(record(&job)); assert(mark_n==0); ask_pending=false;
    /* no box at all: the device's own pause (0.8 s of voice from the start, then quiet) → a nod */
    react_speech=0; hint_at_chunk=-1; mark_n=0; listen_job_with(3000,",\"vad\":false",&job);
    sdk_rx_chunks=0; complete(record(&job));
    assert(mark_n==1 && !strcmp(mark_what[0],"nod"));
    /* a voice after 1.2 s of quiet: Molty perks up (its pause 1.2 s later falls in the gap) */
    react_speech=12; mark_n=0; listen_job_with(3000,",\"vad\":false",&job);
    sdk_rx_chunks=0; complete(record(&job)); react_speech=-1;
    assert(mark_n==1 && !strcmp(mark_what[0],"look_up"));
    /* the relay's reaction (a reflex peek at 1.2 s, 2.2 s long) plays over the listen: the box's
     * pause hint at 1.5 s nods no more (it would cut the peek short) */
    ext_at_chunk=12; hint_at_chunk=15; hint_kind="pause"; mark_n=0; listen_job_with(3000,",\"vad\":false",&job);
    sdk_rx_chunks=0; complete(record(&job)); assert(mark_n==0); ext_at_chunk=-1; hint_at_chunk=-1;
    /* the attentive baseline: a voice the whole listen, no pause and no hint — Molty still moves,
     * a blink on a dip of the voice ~2.5 s in, a glance a beat later */
    react_talk=true; mark_n=0; listen_job_with(6000,",\"vad\":false",&job);
    sdk_rx_chunks=0; complete(record(&job)); react_talk=false;
    assert(mark_n==2 && !strcmp(mark_what[0],"blink") && !strcmp(mark_what[1],"glance_left"));
    /* ...not with idle life off */
    idle_setting=0; react_talk=true; mark_n=0; listen_job_with(6000,",\"vad\":false",&job);
    sdk_rx_chunks=0; complete(record(&job)); react_talk=false; idle_setting=2; assert(mark_n==0);
    /* the relay's reaction outside a listen: ignored */
    link_audio_external_react(2200); assert(atomic_load(&s_ext_hold)==0);
    /* a hint after the listen: ignored (no job running) */
    link_audio_hint("listen1","pause"); assert(atomic_load(&s_hint)==0);
    sdk_rx_fill=NULL; hint_at_chunk=-1;
}

/* Soak: an hour of turns, many times over — every allocation a turn makes is given back on
 * every path (the internal-RAM leak hunt of 2026-10-31: a starved heap killed Wi-Fi after ~1 h).
 * One round = what a Grok turn does on the link: a quick phrase / filler and a reply (say with
 * word cues + marks), a listen with partial reactions, box hints and a relay reaction, a shake
 * listen adopted and ended by the relay, one declined, a listen cancelled mid-way (link down),
 * one whose sends fail and resend, a listen_stop no_speech, a cancelled say, a say whose
 * stream is malformed, a say whose speaker fails. Live blocks (heap_caps_* + cJSON) must not
 * move across SOAK_ROUNDS rounds — one block lost per turn fails it. */
#define SOAK_ROUNDS 200
static void stop_at_chunk_10_no_speech(int16_t *buf,size_t n,unsigned chunk) {
    memset(buf,0,n*2);
    if (chunk==10) stop_no_speech();
}
static void soak_round(void) {
    audio_job_t job;
    /* the quick phrase / filler, then the reply: say {cues, marks} */
    mark_n=0; cue_n=0; cue_last=-9;
    assert(say_with(",\"cues\":true,\"marks\":[[0,\"happy\"],[2,\"nod\"],[4,\"wiggle\"]]"));
    assert(receive_cued(0,false,"[[0,0],[1,3000]]")==ESP_OK && receive_cued(1,true,"[[2,6000],[3,9000]]")==ESP_OK);
    complete(play());
    /* listen with the reflex reacting over the link, box hints, the device's own reactions */
    mark_n=0; listen_job_with(3000,",\"vad\":false",&job); sdk_rx_fill=react_fill; hint_at_chunk=15; hint_kind="pause"; ext_at_chunk=5;
    sdk_rx_chunks=0; complete(record(&job)); sdk_rx_fill=NULL; hint_at_chunk=-1; ext_at_chunk=-1;
    /* shake → local listen → adopted → the relay's end of turn; then one declined */
    if (shake(&job) && talked()) { stop_at=10; shake_run(&job,3,",\"vad\":false"); }
    if (shake(&job) && talked()) { decline_at=3; shake_run(&job,-1,NULL); }
    /* the link drops mid-listen (link_client.c disconnect → link_audio_cancel) */
    listen_job_with(3000,",\"vad\":false",&job); sdk_rx_chunks=0; sdk_rx_fill=shake_fill; cancel_at=5;
    complete(record(&job)); sdk_rx_fill=NULL; cancel_at=-1;
    /* weak Wi-Fi: sends fail, frames resend */
    listen_job_with(1000,",\"vad\":false",&job); send_fail_left=3; complete(record(&job)); send_fail_left=0;
    /* nobody spoke: the relay's listen_stop no_speech */
    listen_job_with(5000,",\"vad\":false",&job); sdk_rx_chunks=0; sdk_rx_fill=stop_at_chunk_10_no_speech;
    complete(record(&job)); sdk_rx_fill=NULL;
    /* a say cancelled, a say whose stream is malformed, a say whose speaker fails */
    begin_say(); receive_pcm(0,false); link_audio_cancel(); complete(play());
    assert(say_with(",\"cues\":true")); assert(receive_cued(0,false,"[[0]]")==ESP_ERR_INVALID_ARG); complete(play());
    begin_say(); receive_pcm(0,true); sdk_play_error=ESP_FAIL; complete(play()); sdk_play_error=ESP_OK;
    assert(!link_audio_busy());
}
static void test_soak(void) {
    link_audio_relay_adopts(true); listen_setting=5000;
    const uint32_t caps0=sdk_caps; sdk_caps&=~MOLTY_CAP_IMU;
    soak_round();                                     /* warm-up: lazily created state settles */
    const long live0=sdk_heap_live;
    for (int i=0;i<SOAK_ROUNDS;i++) soak_round();
    if (sdk_heap_live!=live0) { fprintf(stderr,"soak: %ld live blocks before, %ld after %d rounds\n",live0,sdk_heap_live,SOAK_ROUNDS); assert(0); }
    sdk_caps=caps0;
}

int main(void) {
    sdk_count_cjson();                    /* the soak counts cJSON blocks too */
    assert(link_audio_init()==ESP_OK && link_pair_init()==ESP_OK);
    s_up_inline = true;   /* no sender task on the host: record() sends as it queues */
    test_say(); test_mood_survives(); test_pair(); test_unsupported(); test_ring(); test_ring_color(); test_ask_cancel(); test_listen_stop(); test_listen_window(); test_say_cues(); test_say_marks(); test_chirp_sound(); test_ack(); test_shake_talk(); test_tap_and_watchdog(); test_link_drop(); test_shake_settle(); test_resend(); test_reactions(); test_soak();
    /* activity log: every accepted job (done, failed, cancelled, refused frames, adopted shakes) ended its tag */
    assert(sdk_job_begins>0 && sdk_job_begins==sdk_job_ends);
    cJSON_Delete(sdk_sent); free(s_jobs); free(s_done); free(s_ring);
    puts("LINK AUDIO/PAIR PASS: job tags begin/end on every path, paced PCM, final playback drain, errors, cmd-time text/mood, explicit moods outlive jobs, pairing reasons, board caps, listening ring (+ ring_color), ask cancellation, listen_stop (turn_end; no_speech never acknowledged), listen window (30 s, unlimited, setting), say word cues follow playback, turn acknowledgement (chirp + thinking: which ends ack, opt-outs, setting, screen owners, cut by the next job), shake to talk (a local listen at once, adopted by the relay with its job id + end of turn and the audio replayed, or ending quietly: unclaimed / declined / link down; never over another owner, the setting, or a relay that does not adopt), shake settle trim (510 ms: stillness confirmed, capped 600, no IMU), uplink resend in order + overflow drops oldest + flush deadline, listener reactions (hints, warmup, idle off, owners, the relay's reaction never cut short, the baseline while a voice talks), soak: 200 turn rounds (say cues/marks, reacts/hints, shake adopt/decline, link drop, resend, no_speech, failed says) give back every block");
}
