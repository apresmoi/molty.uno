#include "link_sdk_stub.h"
#include "link_protocol.h"
#include "../../main/link/link_audio.c"
#include "../../main/link/link_pair.c"
#include <assert.h>
#include <stdio.h>
#include <math.h>

static unsigned rotations, sent;
static esp_err_t send_error;
static const char *field(const char *key) { cJSON *j=cJSON_GetObjectItem(sdk_sent,key); assert(cJSON_IsString(j)); return j->valuestring; }
bool link_client_connected(void) { return true; }
esp_err_t link_send_json(const cJSON *j) {
    cJSON_Delete(sdk_sent); sdk_sent=cJSON_Duplicate(j,true); sent++; return send_error;
}
static unsigned bin_frames, bin_bytes, bin_ends, bin_seq;
static char bin_reason[16]; static int bin_speech_ms;   /* -1: speech_ms absent */
static char bin_end_header[512];                          /* the exact end-frame header */
esp_err_t link_send_binary(const void *data,size_t size) {
    link_frame_t h; const uint8_t *pcm; size_t bytes;
    assert(link_frame_parse(data,size,&h,&pcm,&bytes) && size<=16384);
    assert(h.seq==bin_seq++ && !bin_ends);       /* ordered, nothing after end */
    /* the end frame alone carries {reason, speech_ms?}: speech_ms only when measured */
    const char *nl=memchr(data,'\n',size); cJSON *hj=cJSON_ParseWithLength(data,nl?(size_t)(nl-(const char *)data):size);
    cJSON *r=cJSON_GetObjectItem(hj,"reason"), *sm=cJSON_GetObjectItem(hj,"speech_ms");
    assert(h.end ? cJSON_IsString(r) && (!sm || cJSON_IsNumber(sm)) : !r && !sm);
    if (h.end) { snprintf(bin_reason,sizeof(bin_reason),"%s",r->valuestring); bin_speech_ms=sm?sm->valueint:-1; }
    if (h.end) snprintf(bin_end_header,sizeof(bin_end_header),"%.*s",(int)(nl?(size_t)(nl-(const char *)data):size),(const char *)data);
    cJSON_Delete(hj);
    bin_frames++; bin_bytes+=bytes; bin_ends+=h.end; return ESP_OK;
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
static int ack_setting = LINK_ACK_CHIRP;   /* the console's acknowledgement setting (mole_fluid.c, NVS) */
int mole_fluid_ack_mode(void) { return ack_setting; }
/* shake to talk: the console's setting (mole_fluid.c, NVS), an update writing the spare slot
 * (self_update.c), and link_cmd.c's refusal codes */
static bool shake_setting = true, update_busy;
bool mole_fluid_shake_talk(void) { return shake_setting; }
bool self_update_busy(void) { return update_busy; }
static const char *refused;
esp_err_t link_cmd_refuse(const char *code) { refused=code; return LINK_CMD_REFUSED; }
esp_err_t link_ui_set_text(const char *text,int len) { assert(len>=0 && len<=4096); memcpy(sdk_text,text,len+1); return ESP_OK; }

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
     * header is the fixture in relay/test/talk.test.ts ("vad:false end frame"). */
    assert(bin_speech_ms==-1);
    assert(!strcmp(bin_end_header,"{\"s\":\"listen1\",\"seq\":16,\"kind\":\"pcm16\",\"rate\":16000,\"end\":true,\"reason\":\"max\"}"));
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

/* Shake to talk: the shake starts a listen at once, on the device (mood listening, ring up);
 * the shake event names it; nothing goes up until the relay adopts it — then the relay's job
 * id, window and end-of-turn rule apply from the shake's first sample (what was recorded
 * replays first). Declined, never adopted, or the link gone: it ends quietly. Never over
 * another owner (say/listen, a question, a pairing, an update), never with the setting off,
 * never with a relay that does not adopt. */
#define RELAY_JOB "molty-abcdefghij.j_shake01"
static int adopt_at = -1, decline_at = -1, cancel_at = -1, stop_at = -1; static const char *adopt_args;
static esp_err_t adopt_e; static char adopt_ack[16]; static double adopt_ms;
static void shake_fill(int16_t *buf,size_t n,unsigned chunk) {
    speech_fill(buf,n,chunk);
    if ((int)chunk==adopt_at) {
        char a[256]; snprintf(a,sizeof(a),"{\"job\":\"" RELAY_JOB "\",\"rate\":16000,\"adopt\":\"shake-%u\"%s}",s_shake_n,adopt_args?adopt_args:"");
        cJSON *args=cJSON_Parse(a),*res=NULL;
        adopt_e=link_audio_listen(args,&res); cJSON_Delete(args);
        if (res) { snprintf(adopt_ack,sizeof(adopt_ack),"%s",cJSON_GetObjectItem(res,"ack")->valuestring); adopt_ms=cJSON_GetObjectItem(res,"ms")->valuedouble; }
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
static void test_shake_talk(void) {
    audio_job_t job;
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
    sdk_caps&=~MOLTY_CAP_MIC; assert(!link_audio_shake(2.4,5,900)); sdk_caps=0xFFFFFFFFu;
    /* back on: it talks again */
    assert(shake(&job) && talked()); shake_run(&job,-1,"");
    listen_setting=30000;
}

int main(void) {
    assert(link_audio_init()==ESP_OK && link_pair_init()==ESP_OK);
    test_say(); test_mood_survives(); test_pair(); test_unsupported(); test_ring(); test_ring_color(); test_ask_cancel(); test_listen_stop(); test_listen_window(); test_say_cues(); test_chirp_sound(); test_ack(); test_shake_talk();
    /* activity log: every accepted job (done, failed, cancelled, refused frames, adopted shakes) ended its tag */
    assert(sdk_job_begins>0 && sdk_job_begins==sdk_job_ends);
    cJSON_Delete(sdk_sent); free(s_jobs); free(s_done); free(s_ring);
    puts("LINK AUDIO/PAIR PASS: job tags begin/end on every path, paced PCM, final playback drain, errors, cmd-time text/mood, explicit moods outlive jobs, pairing reasons, board caps, listening ring (+ ring_color), ask cancellation, listen_stop (turn_end; no_speech never acknowledged), listen window (30 s, unlimited, setting), say word cues follow playback, turn acknowledgement (chirp + thinking: which ends ack, opt-outs, setting, screen owners, cut by the next job), shake to talk (a local listen at once, adopted by the relay with its job id + end of turn and the audio replayed, or ending quietly: unclaimed / declined / link down; never over another owner, the setting, or a relay that does not adopt)");
}
