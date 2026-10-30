#include "link_audio.h"
#include "link_protocol.h"
#include "link_client.h"
#include "link_cmd.h"
#include "link_pair.h"
#include "link_ask.h"
#include "mole_fluid.h"
#include "vad.h"
#include "chirp.h"

#include "molty_board.h"
#include "mole_creature.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "mole_diag.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <limits.h>

typedef struct {
    char job[LINK_STREAM_MAX]; unsigned ms; bool listen;
    bool vad; unsigned end_silence_ms, no_speech_ms;   /* listen: end-of-speech (audio/vad.h) */
    link_ack_t ack;                                     /* listen: the turn acknowledgement it allows */
} audio_job_t;
static QueueHandle_t s_jobs, s_done;
static RingbufHandle_t s_ring;
static bool s_busy, s_listen;
static atomic_bool s_cancel, s_end;
/* listen_stop: the relay called the end of the listen (semantic end-of-turn from its STT,
 * or nobody spoke) — end the recording now, as if the VAD had fired, with that reason.
 * Listen only; reset per job. 0: not stopped; else the STOP_* reason. */
enum { STOP_NONE, STOP_TURN_END, STOP_NO_SPEECH };
static atomic_int s_stop;
static char s_job[LINK_STREAM_MAX];
static uint32_t s_seq;
static int64_t s_last_frame;
static volatile uint32_t s_stat_frames, s_stat_bytes;   /* diag: frames/bytes of the current job */
static const char *s_end_reason;                       /* diag: why the last listen ended */
static uint32_t s_speech_ms;
static unsigned s_mood_gen0;                           /* link_cmd_mood_gen() when the job started */
/* The turn acknowledgement (link_protocol.h link_ack_decide): what the listen that just ended
 * acknowledges, decided by the audio task at its end (before anyone could answer it) and read
 * by link_audio_tick once the job is done (s_done orders the two). The chirp then plays on the
 * audio task AFTER the job is reported done, so a say / listen sent meanwhile is accepted at
 * once: it waits in s_jobs, and s_chirp_cut (set when it is accepted) ends the chirp within
 * one 16 ms write, faded, its DMA flushed with silence — so the chirp never overlaps a say and
 * the next listen's mic never hears it. */
static link_ack_t s_ack;
static atomic_bool s_chirp_cut;

/* say word cues (cap say_cues): word i of the text is spoken from sample s_cue[i] of the
 * stream (UINT32_MAX = not cued yet), filled from the frames' headers; the play loop
 * reveals each word on screen once the speaker has played up to it */
static volatile uint32_t s_cue[LINK_CUE_WORDS];
static bool s_cued;                                    /* this say follows cues */
static int  s_cue_next;                                /* first word not shown yet */

bool link_audio_busy(void) { return s_busy; }
void link_audio_cancel(void) { atomic_store(&s_cancel, true); }

static void drain(void)
{
    size_t n; void *p;
    while ((p = xRingbufferReceive(s_ring, &n, 0))) vRingbufferReturnItem(s_ring, p);
}

/* A word is revealed this much before the speaker's estimated position reaches its cue: what
 * happens between that decision and the glass, so the word lands with its voice. The estimate
 * lags the DAC by ~7.5 ms on average (the writer blocks with 1200..1440 samples queued, not the
 * full 1440 it assumes), the reader picks the cue up on its next 30 ms frame (~15 ms), and the
 * check runs once per 15 ms write (~7.5 ms). */
#define CUE_LEAD_SAMPLES (16 * 30)
/* PCM per speaker write in play(): one DMA descriptor (240 samples, 15 ms), so the cue check
 * runs every 15 ms rather than every 32 ms */
#define PLAY_WRITE_BYTES 480

/* Show the words whose cue the speaker has played (in order: a word waits for the ones
 * before it). all: playback is over — every cued word. */
static void cue_follow(bool all)
{
    if (!s_cued) return;
    const uint32_t played = all ? UINT32_MAX : molty_speaker_stream_played() + CUE_LEAD_SAMPLES;
    int k = s_cue_next;
    while (k < LINK_CUE_WORDS && s_cue[k] != UINT32_MAX && s_cue[k] <= played) k++;
    if (k > s_cue_next) { s_cue_next = k; mole_fluid_text_cue(k - 1); }
}

static esp_err_t play(void)
{
    esp_err_t e = molty_speaker_stream_begin(16000);
    if (e != ESP_OK) { if (s_cued) mole_fluid_text_cue_end(); return e; }
    while (!atomic_load(&s_cancel)) {
        size_t n;
        void *pcm = xRingbufferReceiveUpTo(s_ring, &n, pdMS_TO_TICKS(50), PLAY_WRITE_BYTES);
        /* end may have arrived just after the timed receive returned empty.
         * Recheck the ring after observing end before declaring it drained. */
        if (!pcm && atomic_load(&s_end)) {
            pcm = xRingbufferReceiveUpTo(s_ring, &n, 0, PLAY_WRITE_BYTES);
            if (!pcm) break;
        }
        if (pcm) {
            e = molty_speaker_stream_write(pcm, n / 2);
            vRingbufferReturnItem(s_ring, pcm);
            if (e != ESP_OK) break;
        }
        cue_follow(false);   /* each ≤ 15 ms write: the words the speaker has reached */
    }
    /* i2s_channel_write queues DMA; draining the software ring is not the
     * playback boundary. Keep ownership until the final PCM has left DMA. */
    if (e == ESP_OK && !atomic_load(&s_cancel)) e = molty_speaker_stream_drain();
    molty_speaker_stream_end();
    if (s_cued) {                        /* played out: the last words, then the reader's own pace */
        if (e == ESP_OK && !atomic_load(&s_cancel)) cue_follow(true);
        mole_fluid_text_cue_end();
    }
    return atomic_load(&s_cancel) ? ESP_ERR_INVALID_STATE : e;
}

/* Stream the mic up while recording: 300 ms frames (9.6 KB) leave as they are
 * captured. Recording the whole clip first and then bursting it (160 KB for
 * 5 s) cost a ~250 ms render hitch (TLS encrypt + send on prio 4) and put the
 * entire upload after the relay's clock had already been running for `ms`. */
#define REC_CHUNK_MS      100
#define REC_CHUNK_SAMPLES (16000 * REC_CHUNK_MS / 1000)
#define REC_FRAME_SAMPLES (3 * REC_CHUNK_SAMPLES)

static esp_err_t send_frame(uint8_t *frame, link_frame_t *h, const int16_t *pcm, size_t samples)
{
    int head = link_frame_header((char *)frame, LINK_MESSAGE_MAX, h);
    if (head < 0) return ESP_ERR_INVALID_ARG;
    if (samples) memcpy(frame + head, pcm, samples * 2);
    esp_err_t e = link_send_binary(frame, (size_t)head + samples * 2);
    if (e == ESP_OK) { s_stat_frames++; s_stat_bytes += samples * 2; h->seq++; }
    return e;
}

/* The listen ends at the first of: end of speech (vad.h: ~end_silence_ms of silence
 * after speech), nobody speaking by no_speech_ms, the `ms` window, or the relay's
 * `listen_stop` (its reason, "turn_end" or "no_speech", within one 100 ms chunk). Either way the
 * last frame carries end:true plus {reason, speech_ms}, so the relay starts STT at
 * once instead of after the full window. speech_ms is left out when it was never
 * measured (vad:false, or a listen shorter than VAD calibration). */
static esp_err_t record(const audio_job_t *job)
{
    _Static_assert(LINK_HEADER_MAX + 1 + REC_FRAME_SAMPLES * 2 <= LINK_MESSAGE_MAX, "uplink frame too big");
    _Static_assert(REC_FRAME_SAMPLES % REC_CHUNK_SAMPLES == 0, "chunks fill frames exactly");
    /* 16 samples per ms, in 32 bits as on the S3: never 16000 * ms first (it wraps past
     * ~268 s, and an unlimited listen runs 300 s) */
    const uint32_t total = 16u * (uint32_t)job->ms;
    vad_cfg_t vc = VAD_CFG_DEFAULT;
    vc.enabled = job->vad; vc.max_ms = job->ms;
    vc.end_silence_ms = job->end_silence_ms; vc.no_speech_ms = job->no_speech_ms;
    vad_t vad;
    vad_init(&vad, &vc);
    vad_verdict_t verdict = VAD_CONTINUE;
    s_end_reason = "error"; s_speech_ms = 0;
    uint8_t *frame = heap_caps_malloc(LINK_MESSAGE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *acc = heap_caps_malloc(REC_FRAME_SAMPLES * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *chunk = heap_caps_malloc(REC_CHUNK_SAMPLES * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_err_t e = frame && acc && chunk ? ESP_OK : ESP_ERR_NO_MEM;
    if (e == ESP_OK) e = molty_mic_stream_start(16000, REC_CHUNK_MS, 8);
    bool streaming = e == ESP_OK;
    if (streaming) mole_fluid_listen(true);   /* the screen's listening ring, while the mic records */
    link_frame_t h = {0}; strcpy(h.stream, job->job);
    size_t done = 0, have = 0;
    while (e == ESP_OK && verdict == VAD_CONTINUE) {
        if (atomic_load(&s_cancel)) { e = ESP_ERR_INVALID_STATE; break; }
        const int stop = atomic_load(&s_stop);
        if (stop != STOP_NONE) {
            /* the relay's end of the listen: what is buffered goes out as the final frame, with
             * the relay's reason — no_speech is acknowledged like the device's own (no chirp) */
            s_end_reason = stop == STOP_NO_SPEECH ? "no_speech" : "turn_end"; s_speech_ms = vad.speech_ms;
            h.end = true; h.reason = s_end_reason; h.speech_ms = s_speech_ms; h.speech_known = vad_speech_known(&vad);
            e = send_frame(frame, &h, acc, have);
            have = 0;
            break;
        }
        size_t got = 0;
        e = molty_mic_stream_read(chunk, REC_CHUNK_SAMPLES, &got);
        if (e != ESP_OK) break;
        if (!got) { e = ESP_ERR_TIMEOUT; break; }
        if (got > total - done) got = total - done;
        if (got > REC_FRAME_SAMPLES - have) got = REC_FRAME_SAMPLES - have;
        memcpy(acc + have, chunk, got * 2);
        have += got; done += got;
        verdict = vad_feed(&vad, chunk, got);
        mole_fluid_listen_level(vad_level(&vad));   /* ~10 Hz: the ring swells with the voice */
        if (verdict == VAD_CONTINUE && done == total) verdict = VAD_END_MAX;   /* window < one VAD frame */
        if (have == REC_FRAME_SAMPLES || verdict != VAD_CONTINUE) {
            h.end = verdict != VAD_CONTINUE;
            if (h.end) {
                s_end_reason = vad_reason(verdict); s_speech_ms = vad.speech_ms;
                h.reason = s_end_reason; h.speech_ms = s_speech_ms;
                /* VAD off / ended inside calibration: no measurement, so no speech_ms
                 * (a 0 would make the relay treat real speech as silence) */
                h.speech_known = vad_speech_known(&vad);
            }
            e = send_frame(frame, &h, acc, have);
            have = 0;
        }
    }
    /* stop the mic as soon as the listen is over: the creature leaves `listening` and
     * the relay is already transcribing */
    if (streaming) { molty_mic_stream_stop(); mole_fluid_listen(false); }
    if (e != ESP_OK) s_end_reason = "error";
    /* heard speech and nobody else took the screen meanwhile (an explicit mood, an ask, an
     * update screen, a pairing): acknowledge it — decided now, before the relay or an agent
     * could react to the end frame */
    s_ack = link_ack_decide(job->ack, s_end_reason, vad_speech_known(&vad), s_speech_ms,
                            link_cmd_mood_gen() != s_mood_gen0 || link_ask_pending() || link_pair_active(),
                            molty_board_has(MOLTY_CAP_SPEAKER));
    /* Fail fast: an end-only frame tells the relay the stream is over instead of
     * leaving it to time out into no_audio ms + 5 s later. */
    if (e != ESP_OK && !atomic_load(&s_cancel) && frame && link_client_connected()) {
        h.end = true; h.reason = "error"; h.speech_ms = vad.speech_ms; h.speech_known = vad_speech_known(&vad);
        send_frame(frame, &h, NULL, 0);
    }
    heap_caps_free(chunk); heap_caps_free(acc); heap_caps_free(frame);
    return e;
}

/* The acknowledgement chirp (audio/chirp.h), on the audio task after the listen was reported
 * done. Skipped when the speaker is someone else's; cut (a 4 ms fade) when a new job arrives. */
#define CHIRP_CHUNK 256    /* 16 ms: the cut's latency once the DMA ring is full */
#define CHIRP_FADE  64     /* 4 ms */
static void chirp_play(void)
{
    if (molty_speaker_stream_begin(CHIRP_RATE) != ESP_OK) return;
    int16_t buf[CHIRP_CHUNK];
    size_t at = 0, n;
    esp_err_t e = ESP_OK;
    while (!atomic_load(&s_chirp_cut) && (n = chirp_fill(buf, at, CHIRP_CHUNK)) > 0) {
        e = molty_speaker_stream_write(buf, n);
        if (e != ESP_OK) break;
        at += n;
    }
    const bool cut = at < CHIRP_SAMPLES;
    if (e == ESP_OK && cut && (n = chirp_fill(buf, at, CHIRP_FADE)) > 0) {   /* no click */
        for (size_t i = 0; i < n; i++) buf[i] = (int16_t)((int32_t)buf[i] * (int32_t)(n - i) / (int32_t)(n + 1));
        e = molty_speaker_stream_write(buf, n);
    }
    if (e == ESP_OK) e = molty_speaker_stream_drain();   /* the DMA ring clocked out with silence */
    molty_speaker_stream_end();
    mole_diag_event(e != ESP_OK, "audio.ack chirp %s %s", cut ? "cut" : "played", esp_err_to_name(e));
}

/* One job, start to finish (the worker's loop body; host tests run it directly). */
static void run_job(const audio_job_t *job)
{
    int64_t t0 = esp_timer_get_time();
    mole_diag_event(false, "audio.%s start ms=%u int_free=%u", job->listen ? "listen" : "say", job->ms,
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    s_ack = LINK_ACK_OFF;
    esp_err_t e = job->listen ? record(job) : play();
    unsigned took = (unsigned)((esp_timer_get_time() - t0) / 1000);
    if (job->listen)
        mole_diag_event(e != ESP_OK, "audio.listen end %s took=%ums frames=%u bytes=%u reason=%s speech_ms=%u ack=%s",
                        esp_err_to_name(e), took, (unsigned)s_stat_frames, (unsigned)s_stat_bytes,
                        s_end_reason, (unsigned)s_speech_ms, link_ack_name(s_ack));
    else
        mole_diag_event(e != ESP_OK, "audio.say end %s took=%ums frames=%u bytes=%u",
                        esp_err_to_name(e), took, (unsigned)s_stat_frames, (unsigned)s_stat_bytes);
    drain();
    const bool chirp = job->listen && s_ack == LINK_ACK_CHIRP;
    atomic_store(&s_chirp_cut, false);   /* before done: only a job accepted after it cuts */
    xQueueSend(s_done, &e, portMAX_DELAY);
    if (chirp) chirp_play();
}

static void audio_task(void *arg)
{
    (void)arg;
    audio_job_t job;
    for (;;) {
        xQueueReceive(s_jobs, &job, portMAX_DELAY);
        run_job(&job);
    }
}

esp_err_t link_audio_init(void)
{
    /* No speaker and no mic: say/listen answer "unsupported" before ever touching the
     * worker, so skip it and its 128 KB PSRAM ring (2 MB boards notice). */
    if (!molty_board_has(MOLTY_CAP_SPEAKER) && !molty_board_has(MOLTY_CAP_MIC)) return ESP_OK;
    s_jobs = xQueueCreate(1, sizeof(audio_job_t));
    s_done = xQueueCreate(1, sizeof(esp_err_t));
    s_ring = xRingbufferCreateWithCaps(128 * 1024, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_jobs || !s_done || !s_ring) return ESP_ERR_NO_MEM;
    /* PSRAM stack: no flash ops on this task; internal RAM is the scarce one */
    return xTaskCreatePinnedToCoreWithCaps(audio_task, "link_audio", 6144, NULL, 4, NULL, tskNO_AFFINITY,
                                           MALLOC_CAP_SPIRAM) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t start(const cJSON *args, bool listen, cJSON **out)
{
    /* no mic / no speaker on this board: "unsupported", before any mood or text change */
    if (!molty_board_has(listen ? MOLTY_CAP_MIC : MOLTY_CAP_SPEAKER)) return ESP_ERR_NOT_SUPPORTED;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    cJSON *j = cJSON_GetObjectItemCaseSensitive(args, "job");
    cJSON *ms = cJSON_GetObjectItemCaseSensitive(args, "ms");
    cJSON *text = cJSON_GetObjectItemCaseSensitive(args, "text");
    cJSON *rate = cJSON_GetObjectItemCaseSensitive(args, "rate");
    if (!cJSON_IsString(j) || !j->valuestring[0] || strlen(j->valuestring) >= sizeof(s_job)) return ESP_ERR_INVALID_ARG;
    if (!cJSON_IsNumber(rate) || rate->valuedouble != 16000) return ESP_ERR_INVALID_ARG;
    /* listen: the window (link_protocol.h; absent = the device's setting, 0 = unlimited);
     * say: the clip's length */
    uint32_t window = 0, rec_ms = 0;
    if (listen) { if (!link_listen_window(args, mole_fluid_listen_window(), &window, &rec_ms)) return ESP_ERR_INVALID_ARG; }
    else if (!cJSON_IsNumber(ms) || ms->valuedouble < 0 || ms->valuedouble > INT_MAX ||
             ms->valuedouble != ms->valueint) return ESP_ERR_INVALID_ARG;
    if (!listen && (!cJSON_IsString(text) || !text->valuestring[0] || strlen(text->valuestring) > 4096)) return ESP_ERR_INVALID_ARG;
    /* say {cues: true} (cap say_cues): the frames carry word cues; the text follows the voice */
    const cJSON *cues = cJSON_GetObjectItemCaseSensitive(args, "cues");
    if (!listen && cues && !cJSON_IsBool(cues)) return ESP_ERR_INVALID_ARG;
    const bool cued = !listen && cJSON_IsTrue(cues);
    const vad_cfg_t vad_def = VAD_CFG_DEFAULT;
    bool vad = vad_def.enabled;
    unsigned end_silence_ms = vad_def.end_silence_ms, no_speech_ms = vad_def.no_speech_ms;
    if (listen && !link_listen_args(args, &vad, &end_silence_ms, &no_speech_ms)) return ESP_ERR_INVALID_ARG;
    int32_t ring = -1;                       /* the listening ring's colour; -1 = the device default */
    if (listen && !link_color_arg(args, "ring_color", &ring)) return ESP_ERR_INVALID_ARG;
    link_ack_t ack = LINK_ACK_OFF;           /* the turn acknowledgement: the agent's ack, else the setting */
    if (listen && !link_ack_arg(args, (link_ack_t)mole_fluid_ack_mode(), &ack)) return ESP_ERR_INVALID_ARG;
    cJSON *result = cJSON_CreateObject();
    if (!result) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(result, "job", j->valuestring);
    if (listen) cJSON_AddNumberToObject(result, "ms", window);   /* the window it applies (0 = unlimited) */
    if (listen) cJSON_AddStringToObject(result, "ack", link_ack_name(ack));   /* the relay leaves thinking to us */
    esp_err_t e;
    if (listen) {
        mole_fluid_listen_color(ring);       /* before the mic opens the ring (record) */
        e = link_cmd_mood("listening", -1);
    } else {
        e = mole_creature_select("fluid");
        if (e == ESP_OK && cued) {               /* no word on screen until the first one is cued */
            for (int i = 0; i < LINK_CUE_WORDS; i++) s_cue[i] = UINT32_MAX;
            mole_fluid_text_cue(-1);
        }
        if (e == ESP_OK) e = link_ui_set_text(text->valuestring, (int)strlen(text->valuestring));
        /* mood first: it selects the creature view; the text state then wins. */
        if (e == ESP_OK) e = link_cmd_mood("speaking", -1);
        if (e == ESP_OK) e = mole_creature_state_set("{\"state\":\"text\"}");
    }
    if (e != ESP_OK) { if (cued) mole_fluid_text_cue_end(); cJSON_Delete(result); return e; }
    link_ask_cancel();   /* say / listen, once accepted, take the screen from a pending question */
    audio_job_t job = {.listen = listen, .ms = listen ? (unsigned)rec_ms : (unsigned)ms->valueint,
                       .vad = vad, .end_silence_ms = end_silence_ms, .no_speech_ms = no_speech_ms, .ack = ack};
    strcpy(job.job, j->valuestring); strcpy(s_job, job.job);
    s_mood_gen0 = link_cmd_mood_gen();   /* after our own listening/speaking mood */
    s_seq = 0; s_stat_frames = 0; s_stat_bytes = 0; s_busy = true; s_listen = listen; s_last_frame = esp_timer_get_time();
    s_cued = cued; s_cue_next = 0;
    atomic_store(&s_end, false); atomic_store(&s_cancel, false); atomic_store(&s_stop, STOP_NONE);
    atomic_store(&s_chirp_cut, true);    /* a chirp still playing ends now: this job is next */
    xQueueSend(s_jobs, &job, 0);
    *out = result;
    return ESP_OK;
}

esp_err_t link_audio_say(const cJSON *args, cJSON **out) { return start(args, false, out); }
esp_err_t link_audio_listen(const cJSON *args, cJSON **out) { return start(args, true, out); }

/* cmd listen_stop {job, reason?}: ends THAT listen early with `reason` — "turn_end" (the
 * default: the human finished; acknowledged) or "no_speech" (nobody spoke; never
 * acknowledged: no chirp, no thinking); any other reason is refused. Never touches a say,
 * and never cancels: the frames recorded so far still go up, ending with end:true. A job
 * that is not the running listen (already ended, or another job) is answered
 * {stopping:false} — the relay sends it fire-and-forget and may race the device's own end. */
esp_err_t link_audio_listen_stop(const cJSON *args, cJSON **out)
{
    cJSON *j = cJSON_GetObjectItemCaseSensitive(args, "job");
    if (!cJSON_IsString(j) || !j->valuestring[0] || strlen(j->valuestring) >= sizeof(s_job)) return ESP_ERR_INVALID_ARG;
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(args, "reason");
    int why = STOP_TURN_END;
    if (r && cJSON_IsString(r) && !strcmp(r->valuestring, "no_speech")) why = STOP_NO_SPEECH;
    else if (r && !(cJSON_IsString(r) && !strcmp(r->valuestring, "turn_end"))) return ESP_ERR_INVALID_ARG;
    bool mine = s_busy && s_listen && !strcmp(j->valuestring, s_job);
    if (mine) atomic_store(&s_stop, why);
    cJSON *result = cJSON_CreateObject();
    if (!result) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(result, "job", j->valuestring);
    cJSON_AddBoolToObject(result, "stopping", mine);
    *out = result;
    return ESP_OK;
}

esp_err_t link_audio_receive(const void *data, size_t len)
{
    link_frame_t h; const uint8_t *pcm; size_t n;
    if (!s_busy || s_listen) return ESP_ERR_INVALID_STATE;
    if (!link_frame_parse(data, len, &h, &pcm, &n) || strcmp(h.stream, s_job) || h.seq != s_seq || atomic_load(&s_end)) {
        link_audio_cancel(); return ESP_ERR_INVALID_ARG;
    }
    /* Backpressure stays on link tasks. 16 KB takes 512 ms at PCM16/16k;
     * bounded waits let TCP pace a relay sending faster than the speaker. */
    if (s_cued) for (int i = 0; i < h.ncues; i++) s_cue[h.cue_word[i]] = h.cue_sample[i];   /* before its audio */
    if (n && xRingbufferSend(s_ring, pcm, n, pdMS_TO_TICKS(750)) != pdTRUE) {
        link_audio_cancel(); return ESP_ERR_NO_MEM;
    }
    s_seq++; s_last_frame = esp_timer_get_time();
    s_stat_frames++; s_stat_bytes += n;
    if (h.end) atomic_store(&s_end, true);
    return ESP_OK;
}

void link_audio_tick(void)
{
    if (!s_busy) return;
    if (!s_listen && esp_timer_get_time() - s_last_frame > 30000000) link_audio_cancel();
    esp_err_t e;
    if (xQueueReceive(s_done, &e, 0) != pdTRUE) return;
    drain(); /* worker has acknowledged completion; discard any late PCM */
    s_busy = false;
    /* Back to idle only if nobody chose a mood while the job ran: an explicit mood
     * (with its own hold_s, fluid_director) outlives the listen/say it arrived in. */
    /* A listen that heard speech is acknowledged (s_ack, decided at its end): `thinking`
     * instead of idle, LINK_ACK_THINK_S at most, until the agent's say / mood / ask / listen
     * replaces it — a mood the device sets itself, so the next job still takes over. */
    if (!link_pair_active()) {
        if (s_listen && s_ack != LINK_ACK_OFF) link_cmd_mood_unless(s_mood_gen0, "thinking", LINK_ACK_THINK_S);
        else link_cmd_mood_idle_unless(s_mood_gen0);
    }
    /* A failed/cancelled stream must never report successful playback. Listen
     * completion is the uplink end flag, not a say event. */
    if (!s_listen && e == ESP_OK && !atomic_load(&s_cancel) && link_client_connected()) {
        cJSON *j = link_say_done_frame(s_job);
        if (j) { link_send_json(j); cJSON_Delete(j); }
    }
}
