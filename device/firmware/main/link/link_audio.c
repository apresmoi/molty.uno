#include "link_audio.h"
#include "link_protocol.h"
#include "link_client.h"
#include "link_cmd.h"
#include "link_pair.h"
#include "link_ask.h"
#include "mole_fluid.h"
#include "vad.h"
#include "chirp.h"
#include "self_update.h"
#include "link_uplink.h"
#include "listen_react.h"
#include "shake_settle.h"
#include "cute_act.h"

#include "molty_board.h"
#include "mole_creature.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "mole_diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <limits.h>

typedef struct {
    char job[LINK_STREAM_MAX]; unsigned ms; bool listen;
    bool vad; unsigned end_silence_ms, no_speech_ms;   /* listen: end-of-speech (audio/vad.h) */
    link_ack_t ack;                                     /* listen: the turn acknowledgement it allows */
    bool pending;                                       /* listen: begun by a shake, not adopted yet */
} audio_job_t;
static QueueHandle_t s_jobs, s_done;
static RingbufHandle_t s_ring;
static bool s_busy, s_listen;
static atomic_bool s_cancel, s_end;
/* listen_stop: the relay called the end of the listen (semantic end-of-turn from its STT,
 * or nobody spoke) — end the recording now, as if the VAD had fired, with that reason.
 * Listen only; reset per job. 0: not stopped; else the STOP_* reason. */
enum { STOP_NONE, STOP_TURN_END, STOP_NO_SPEECH, STOP_TAP };
static atomic_int s_stop;
static char s_job[LINK_STREAM_MAX];
static uint32_t s_seq;
static int64_t s_last_frame;
static uint32_t s_job_t0_ms;                           /* the running job began (for a tap's guard) */
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

/* Shake to talk (link_protocol.h LINK_SHAKE_ADOPT_MS). The sense task hands a shake over
 * (link_audio_shake); the link task starts the local listen and sends the shake event naming
 * it. The listen is PEND_WAIT until the relay adopts it (cmd listen {adopt}, link task: CAS
 * WAIT → ADOPTED, after filling s_adopt) or declines it (cmd listen_stop: WAIT → DECLINED);
 * the audio task gives up on its own (WAIT → DEAD) when nobody did within the adopt window
 * or the link went down. One CAS decides each race, so an adoption either lands on a listen
 * that will stream or is refused (listen_gone). A listen the relay started is PEND_NONE. */
enum { PEND_NONE, PEND_WAIT, PEND_ADOPTED, PEND_DECLINED, PEND_DEAD };
static atomic_int s_pend;
typedef struct {
    char job[LINK_STREAM_MAX]; uint32_t rec_ms;
    bool vad; unsigned end_silence_ms, no_speech_ms; link_ack_t ack;
} adopt_t;
static adopt_t s_adopt;
static bool s_tagged;                                 /* the running job began an activity-log tag (link task) */                               /* written before WAIT → ADOPTED */
static atomic_bool s_relay_adopts;                    /* the relay's welcome lists listen_adopt */
static atomic_bool s_shake_gesture;                   /* ...it is a bound gesture's `talk`, no shake */
static atomic_bool s_shake_req;                       /* a shake handed over by the sense task */
static double s_shake_peak; static int s_shake_rev; static unsigned s_shake_ms;   /* before s_shake_req */
static unsigned s_shake_n;                            /* local listen ids: shake-<n> */

/* say word cues (cap say_cues): word i of the text is spoken from sample s_cue[i] of the
 * stream (UINT32_MAX = not cued yet), filled from the frames' headers; the play loop
 * reveals each word on screen once the speaker has played up to it */
static volatile uint32_t s_cue[LINK_CUE_WORDS];
static bool s_cued;                                    /* this say follows cues */
static int  s_cue_next;                                /* first word not shown yet */
/* say marks (cap say_marks, link_protocol.h): the relay's inline [mood] / [action] cues,
 * each fired when its word is shown (cue_follow, the audio task) — a mood tints the words
 * from there on and becomes the creature's mood when the say ends; an action plays on the
 * creature once it is back (mole_fluid_act). Never while a question, a pairing or an update
 * owns the screen. Set by start() before the job goes to the audio task. */
static link_mark_t s_marks[LINK_MARKS_MAX];
static int s_nmarks;
static uint32_t s_mark_fired;                          /* bit i: mark i fired */
static int s_mark_mood;                                /* the last mood mark fired, -1 none */
static unsigned s_say_text_ver;                        /* the say's words (mole_fluid_text_ver) */
/* a say whose last mood mark persists past its end holds it this long (happy / surprised /
 * idle keep their own default) */
#define MARK_MOOD_HOLD_S 8

static bool screen_owned(void) { return link_ask_pending() || link_pair_active() || self_update_busy(); }

/* fire the marks whose word is shown (all: playback is over — every one left), in order */
static void marks_follow(bool all)
{
    for (int i = 0; i < s_nmarks; i++) {
        if ((s_mark_fired >> i & 1u) || (!all && s_marks[i].word >= s_cue_next)) continue;
        s_mark_fired |= 1u << i;
        if (screen_owned()) continue;
        if (s_marks[i].act) mole_fluid_act(s_marks[i].id);
        else { mole_fluid_cue_mood(s_marks[i].id, s_say_text_ver); s_mark_mood = s_marks[i].id; }
    }
}

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
    marks_follow(all);
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
_Static_assert(REC_FRAME_SAMPLES == UPLINK_SLOT_SAMPLES, "one uplink slot holds one frame");

/* ── the uplink (link_uplink.h; docs/AGENT-LINK.md "Turn trace") ──────────────────────
 * The audio task only queues frames; a sender task sends them, oldest first, so the mic
 * read never waits on the network (a send blocks up to 2 s on weak Wi-Fi; the I2S DMA ring
 * holds ~90 ms). A failed send keeps the frame: it goes again after a back-off (100 → 400
 * ms), same seq, in order, while the link is up. Each frame's header is built when it goes
 * (rt / wq / the end frame's tr are known then). After the mic stops, the listen waits for
 * the queue to empty: UP_FLUSH_MS, then whatever is still queued (but the end frame) is
 * dropped and counted, the end frame gets UP_END_MS more. Cancel / link down: everything
 * goes. s_up_gen tells a send finishing late from the next listen's queue. */
#define UP_FLUSH_MS   3000
#define UP_END_MS     1000
#define UP_BACKOFF_MIN 100
#define UP_BACKOFF_MAX 400
static uplink_t s_up;                                  /* under s_up_lock */
static SemaphoreHandle_t s_up_lock;
static QueueHandle_t s_up_kick;                        /* wakes the sender */
static uint8_t *s_up_frame;                            /* the sender's frame buffer */
static char s_up_stream[LINK_STREAM_MAX];              /* the job the frames go up under */
static uint32_t s_up_gen;
static bool s_up_abort;                                /* cancel: a failed send is dropped, not retried */
static const char *s_up_reason;                        /* the end frame's reason / speech_ms */
static uint32_t s_up_speech_ms; static bool s_up_speech_known;
static link_trace_end_t s_up_tr;                       /* the end frame's tr (sent/fail/drop at send time) */
static bool s_up_inline;                               /* host tests: no sender task, record() pumps */
/* the pumper's own (the sender task; the audio task in inline mode) — nobody else touches
 * them: a new listen is noticed by its generation (read under s_up_lock), never reset from outside */
static int64_t s_up_retry_at; static unsigned s_up_backoff; static uint32_t s_up_pump_gen;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void up_lock(void) { xSemaphoreTake(s_up_lock, portMAX_DELAY); }
static void up_unlock(void) { xSemaphoreGive(s_up_lock); }

/* One send: 0 nothing queued, 1 sent, -1 failed (kept, unless aborted). */
static int up_try(void)
{
    up_lock();
    const int16_t *pcm = NULL;
    const uplink_meta_t *m = uplink_peek(&s_up, &pcm);
    if (!m) { up_unlock(); return 0; }
    const uint32_t gen = s_up_gen;
    link_frame_t h = {.seq = m->seq, .end = m->end, .trace = true, .ct = m->ct, .cl = m->cl, .rt = m->rt,
                      .wq = now_ms() - m->ready_ms, .nlv = m->nlv};
    memcpy(h.lv, m->lv, sizeof(h.lv));
    strcpy(h.stream, s_up_stream);
    link_trace_end_t tr = s_up_tr;
    if (m->end) {
        h.reason = s_up_reason; h.speech_ms = s_up_speech_ms; h.speech_known = s_up_speech_known;
        tr.sent = s_up.sent + 1; tr.fail = s_up.fail; tr.drop = s_up.drop; tr.dropms = s_up.drop_samples / 16;
        h.tr = &tr;
    }
    const size_t n = m->n;
    int head = link_frame_header((char *)s_up_frame, LINK_MESSAGE_MAX, &h);
    if (head >= 0 && n) memcpy(s_up_frame + head, pcm, n * 2);
    up_unlock();
    esp_err_t e = head < 0 ? ESP_ERR_INVALID_ARG : link_send_binary(s_up_frame, (size_t)head + n * 2);
    up_lock();
    if (gen == s_up_gen) {
        if (e == ESP_OK || head < 0) {       /* sent (a frame that cannot be framed is lost, counted) */
            uplink_sent(&s_up);
            if (head < 0) { s_up.sent--; s_up.drop++; s_up.drop_samples += (uint32_t)n; }
            else { s_stat_frames++; s_stat_bytes += (uint32_t)n * 2; }
        } else {
            uplink_failed(&s_up);
            if (s_up_abort) uplink_drop_all(&s_up, false);
        }
    }
    up_unlock();
    return e == ESP_OK ? 1 : -1;
}

/* Send what is queued, oldest first, until it is empty, a send fails (back off) or the
 * link is down. */
static void up_pump(void)
{
    up_lock(); const uint32_t gen = s_up_gen; up_unlock();
    if (gen != s_up_pump_gen) { s_up_pump_gen = gen; s_up_retry_at = 0; s_up_backoff = 0; }   /* a new listen: no old back-off */
    while (link_client_connected()) {
        const int64_t now = esp_timer_get_time() / 1000;
        if (!s_up_inline && now < s_up_retry_at) return;
        const int r = up_try();
        if (r == 0) return;
        if (r < 0) {
            s_up_backoff = s_up_backoff ? (s_up_backoff * 2 > UP_BACKOFF_MAX ? UP_BACKOFF_MAX : s_up_backoff * 2) : UP_BACKOFF_MIN;
            s_up_retry_at = now + s_up_backoff;
            return;
        }
        s_up_backoff = 0;
    }
}

static void up_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t k;
        xQueueReceive(s_up_kick, &k, pdMS_TO_TICKS(50));
        up_pump();
    }
}

static void up_kick(void)
{
    if (s_up_inline) { up_pump(); return; }
    uint8_t k = 1;
    xQueueSend(s_up_kick, &k, 0);
}

/* A new listen: an empty queue under its job id (a send still finishing belongs to the old one). */
static void up_begin(const char *stream)
{
    up_lock();
    s_up_gen++;
    uplink_reset(&s_up);
    strcpy(s_up_stream, stream);
    s_up_abort = false; s_up_reason = NULL;
    memset(&s_up_tr, 0, sizeof(s_up_tr));
    up_unlock();
}

static void up_abort(void)
{
    up_lock();
    s_up_abort = true;
    uplink_drop_all(&s_up, false);
    up_unlock();
}

/* After the mic stopped: until every frame went (or the deadlines). */
static void up_flush(void)
{
    int64_t deadline = esp_timer_get_time() / 1000 + UP_FLUSH_MS;
    bool late = false;
    for (;;) {
        up_lock(); const int left = uplink_pending(&s_up); up_unlock();
        if (!left) return;
        if (atomic_load(&s_cancel) || !link_client_connected()) { up_abort(); return; }
        const int64_t now = esp_timer_get_time() / 1000;
        if (now >= deadline) {
            if (late) { up_abort(); return; }
            late = true; deadline = now + UP_END_MS;
            up_lock(); uplink_drop_all(&s_up, true); up_unlock();   /* the end frame still goes */
        }
        up_kick();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static bool rssi_now(int *out)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    *out = ap.rssi;
    return true;
}

/* ── shake settle (shake_settle.h): the sense task feeds the IMU while a shake's listen
 * waits for the handling to stop; s_settle_res is the result (-1 = not yet: ms | capped << 16). */
static settle_t s_settle;                              /* under s_settle_lock */
static SemaphoreHandle_t s_settle_lock;
static atomic_bool s_settling;
static atomic_int s_settle_res;

void link_audio_imu(float ax, float ay, float az, uint32_t ms)
{
    if (!atomic_load(&s_settling) || !s_settle_lock) return;
    xSemaphoreTake(s_settle_lock, portMAX_DELAY);
    if (atomic_load(&s_settling) && settle_feed(&s_settle, ax, ay, az, ms)) {
        atomic_store(&s_settle_res, (int)(s_settle.at | (s_settle.capped ? 1u << 16 : 0u)));
        atomic_store(&s_settling, false);
    }
    xSemaphoreGive(s_settle_lock);
}

static void settle_begin(uint32_t t0)
{
    xSemaphoreTake(s_settle_lock, portMAX_DELAY);
    settle_init(&s_settle, NULL, t0);
    atomic_store(&s_settle_res, -1);
    atomic_store(&s_settling, true);
    xSemaphoreGive(s_settle_lock);
}

/* the cap, on the audio's own clock (ms of audio captured) */
static int settle_check(uint32_t audio_ms)
{
    int res = atomic_load(&s_settle_res);
    if (res >= 0) return res;
    xSemaphoreTake(s_settle_lock, portMAX_DELAY);
    if (atomic_load(&s_settling) && settle_tick(&s_settle, s_settle.t0 + audio_ms)) {
        atomic_store(&s_settle_res, (int)(s_settle.at | (s_settle.capped ? 1u << 16 : 0u)));
        atomic_store(&s_settling, false);
    }
    xSemaphoreGive(s_settle_lock);
    return atomic_load(&s_settle_res);
}

/* ── listener reactions (listen_react.h): fed by live chunks and the relay's hints ── */
static react_t s_react;                                /* the audio task's */
static atomic_int s_hint;                              /* REACT_HINT_* waiting, 0 none */
static atomic_uint s_ext_hold;                         /* the relay's reaction waiting: its ms, 0 none */

static void react_play(int what)
{
    switch (what) {
    case REACT_NOD:          mole_fluid_act(CA_NOD); break;
    case REACT_GLANCE_LEFT:  mole_fluid_act(CA_GLANCE_LEFT); break;
    case REACT_GLANCE_RIGHT: mole_fluid_act(CA_GLANCE_RIGHT); break;
    case REACT_PERK:         mole_fluid_act(CA_LOOK_UP); break;
    case REACT_BLINK:        mole_fluid_act(CA_BLINK); break;
    default: return;
    }
    mole_diag_event(false, "audio.react %d", what);
}

/* a live chunk heard at listen time t (ms since the mic opened), level 0..1 */
static void react_live(float level, uint32_t t)
{
    const bool blocked = screen_owned();
    const unsigned ext = atomic_exchange(&s_ext_hold, 0);
    if (ext) react_external(&s_react, t, ext);         /* the relay's reaction plays: ours wait */
    const int hint = atomic_exchange(&s_hint, 0);
    if (hint) react_play(react_hint(&s_react, hint, t, blocked));
    react_play(react_level(&s_react, level, t, blocked));
}

void link_audio_external_react(uint32_t hold_ms)
{
    if (!s_busy || !s_listen || !hold_ms) return;
    atomic_store(&s_ext_hold, hold_ms);
}

void link_audio_hint(const char *job, const char *kind)
{
    if (!job || !kind || !s_busy || !s_listen || strcmp(job, s_job)) return;
    const int k = !strcmp(kind, "pause") ? REACT_HINT_PAUSE : !strcmp(kind, "question") ? REACT_HINT_QUESTION : 0;
    if (k) atomic_store(&s_hint, k);
}

/* What record() carries from one mic chunk to the next. */
typedef struct {
    int16_t *acc;
    size_t have, done, total;               /* samples in acc; recorded; the window */
    vad_t vad; vad_verdict_t verdict;
    link_frame_t h;                         /* stream id + the next seq */
    uint32_t ct_base;                       /* samples trimmed before the first kept one */
    /* the frame being filled: chunk levels (one byte per 100 ms of the stream) + saturation */
    double lv_acc; uint32_t lv_n; uint8_t lv[UPLINK_LV_MAX]; uint8_t nlv; uint32_t clip;
    int32_t peak;
    link_ack_t ack; bool acked;             /* the acknowledgement: decided as the end frame is queued */
    uint32_t t_mic0;                        /* ms the mic opened */
    bool shake; uint32_t set; bool setc;    /* a shake's listen: its settle trim */
    bool has_r0; int r0;
} rec_t;

static void decide_ack(rec_t *r)
{
    /* heard speech and nobody else took the screen meanwhile (an explicit mood, an ask, an
     * update screen, a pairing): acknowledge it — decided BEFORE the end frame can leave, so
     * before the relay or an agent could react to it. A listen nobody adopted ("unclaimed" /
     * "declined") is no heard reason: it acknowledges nothing. */
    s_ack = link_ack_decide(r->ack, s_end_reason, vad_speech_known(&r->vad), s_speech_ms,
                            link_cmd_mood_gen() != s_mood_gen0 || link_ask_pending() || link_pair_active(),
                            molty_board_has(MOLTY_CAP_SPEAKER));
    r->acked = true;
}

/* The frame in acc goes into the uplink (end: the listen's last, with its reason). */
static void rec_emit(rec_t *r, bool end)
{
    if (end && r->lv_n && r->nlv < UPLINK_LV_MAX) r->lv[r->nlv++] = link_level_byte(r->lv_acc, r->lv_n);   /* a partial chunk */
    uplink_meta_t m = {.seq = r->h.seq++, .ct = (r->ct_base + (uint32_t)(r->done - r->have)) / 16, .ready_ms = now_ms(),
                       .n = (uint16_t)r->have, .cl = (uint16_t)(r->clip > 65535 ? 65535 : r->clip), .nlv = r->nlv, .end = end};
    memcpy(m.lv, r->lv, sizeof(m.lv));
    if (end) {
        decide_ack(r);
        link_trace_end_t tr = {.wall = now_ms() - r->t_mic0, .cap = (uint32_t)(r->done / 16), .gain = molty_mic_gain(),
                               .pk = (uint32_t)((int64_t)r->peak * 1000 / 32768), .shake = r->shake, .set = r->set, .setc = r->setc};
        tr.has_rssi0 = r->has_r0; tr.r0 = r->r0;
        tr.has_rssi1 = rssi_now(&tr.r1);
        up_lock();
        s_up_reason = s_end_reason; s_up_speech_ms = s_speech_ms; s_up_speech_known = r->h.speech_known;
        s_up_tr = tr;
        uplink_push(&s_up, &m, r->acc);
        up_unlock();
    } else {
        up_lock(); uplink_push(&s_up, &m, r->acc); up_unlock();
    }
    r->have = 0; r->nlv = 0; r->clip = 0;
    if (end) r->lv_n = 0, r->lv_acc = 0;
    up_kick();
}

/* One chunk of the listen (any length): into frames, through the VAD; a frame goes up when
 * full or when the VAD (or the window) ended the listen — the end frame carrying
 * {reason, speech_ms?} and the trace. Pieces split at frame and 100 ms level boundaries
 * (counted from the first kept sample), so a trimmed lead never loses a sample. */
static void rec_take(rec_t *r, const int16_t *pcm, size_t got)
{
    while (got && r->verdict == VAD_CONTINUE) {
        size_t n = got;
        if (n > r->total - r->done) n = r->total - r->done;
        if (n > REC_FRAME_SAMPLES - r->have) n = REC_FRAME_SAMPLES - r->have;
        if (n > REC_CHUNK_SAMPLES - r->lv_n) n = REC_CHUNK_SAMPLES - r->lv_n;
        if (!n) { r->verdict = VAD_END_MAX; break; }
        memcpy(r->acc + r->have, pcm, n * 2);
        for (size_t i = 0; i < n; i++) {
            const int32_t v = pcm[i];
            r->lv_acc += (double)v * v;
            if (v >= 32767 || v <= -32768) r->clip++;
            const int32_t a = v < 0 ? -v : v;
            if (a > r->peak) r->peak = a;
        }
        r->lv_n += (uint32_t)n;
        if (r->lv_n == REC_CHUNK_SAMPLES) {
            if (r->nlv < UPLINK_LV_MAX) r->lv[r->nlv++] = link_level_byte(r->lv_acc, r->lv_n);
            r->lv_acc = 0; r->lv_n = 0;
        }
        r->have += n; r->done += n; pcm += n; got -= n;
        r->verdict = vad_feed(&r->vad, pcm - n, n);
        if (r->verdict == VAD_CONTINUE && r->done == r->total) r->verdict = VAD_END_MAX;   /* window < one VAD frame */
        if (r->have < REC_FRAME_SAMPLES && r->verdict == VAD_CONTINUE) continue;
        const bool end = r->verdict != VAD_CONTINUE;
        if (end) {
            s_end_reason = vad_reason(r->verdict); s_speech_ms = r->vad.speech_ms;
            r->h.reason = s_end_reason; r->h.speech_ms = s_speech_ms;
            /* VAD off / ended inside calibration: no measurement, so no speech_ms
             * (a 0 would make the relay treat real speech as silence) */
            r->h.speech_known = vad_speech_known(&r->vad);
        }
        rec_emit(r, end);
    }
}

/* The listen's last frame, ended from outside the VAD (listen_stop, a capture error). */
static void rec_end(rec_t *r, const char *reason)
{
    s_end_reason = reason; s_speech_ms = r->vad.speech_ms;
    r->h.reason = reason; r->h.speech_ms = s_speech_ms; r->h.speech_known = vad_speech_known(&r->vad);
    r->verdict = VAD_END_MAX;   /* over */
    rec_emit(r, true);
}

static void rec_vad(rec_t *r, bool on, uint32_t ms, unsigned end_silence_ms, unsigned no_speech_ms)
{
    vad_cfg_t vc = VAD_CFG_DEFAULT;
    vc.enabled = on; vc.max_ms = ms;
    vc.end_silence_ms = end_silence_ms; vc.no_speech_ms = no_speech_ms;
    vad_init(&r->vad, &vc);
    /* 16 samples per ms, in 32 bits as on the S3: never 16000 * ms first (it wraps past
     * ~268 s, and an unlimited listen runs 300 s) */
    r->total = 16u * ms;
}

/* The leading handling noise of a shake's listen: dropped, the first 10 ms kept faded in. */
#define SETTLE_FADE_SAMPLES 160
typedef struct { size_t drop_left; unsigned fade; } trim_t;
/* applies the trim to buf[0..n) in place; returns how many leading samples to skip */
static size_t trim_apply(trim_t *t, int16_t *buf, size_t n)
{
    size_t skip = t->drop_left < n ? t->drop_left : n;
    t->drop_left -= skip;
    for (size_t i = skip; i < n && t->fade < SETTLE_FADE_SAMPLES; i++, t->fade++)
        buf[i] = (int16_t)((int32_t)buf[i] * (int32_t)t->fade / SETTLE_FADE_SAMPLES);
    return skip;
}

/* The listen ends at the first of: end of speech (vad.h: ~end_silence_ms of silence
 * after speech), nobody speaking by no_speech_ms, the `ms` window, or the relay's
 * `listen_stop` (its reason, "turn_end" or "no_speech", within one 100 ms chunk). Either way the
 * last frame carries end:true plus {reason, speech_ms}, so the relay starts STT at
 * once instead of after the full window. speech_ms is left out when it was never
 * measured (vad:false, or a listen shorter than VAD calibration).
 * A shake's listen (job->pending) records into `pre` and sends nothing until the relay adopts
 * it: then it takes the relay's job id, window, end-of-turn and acknowledgement, the audio so
 * far replays through them (frames go up at once), and it carries on as any listen. Declined,
 * not adopted within LINK_SHAKE_ADOPT_MS of audio, or the link gone: it ends quietly
 * ("unclaimed": no frame, no acknowledgement). Its first audio waits for the shake to settle
 * (shake_settle.h): what came before is handling noise, never sent nor heard by the VAD.
 * Frames go through the uplink (above): the mic never waits on the network. */
static esp_err_t record(const audio_job_t *job)
{
    _Static_assert(LINK_HEADER_MAX + 1 + REC_FRAME_SAMPLES * 2 <= LINK_MESSAGE_MAX, "uplink frame too big");
    _Static_assert(REC_FRAME_SAMPLES % REC_CHUNK_SAMPLES == 0, "chunks fill frames exactly");
    _Static_assert((16u * LINK_SHAKE_ADOPT_MS) % REC_CHUNK_SAMPLES == 0, "the adopt window is whole chunks");
    rec_t r = { .verdict = VAD_CONTINUE, .ack = job->ack };
    rec_vad(&r, job->vad, job->ms, job->end_silence_ms, job->no_speech_ms);
    bool pending = job->pending, unclaimed = false;
    const size_t pre_max = 16u * LINK_SHAKE_ADOPT_MS;
    size_t pre_n = 0, captured = 0;
    s_end_reason = "error"; s_speech_ms = 0;
    r.acc = heap_caps_malloc(REC_FRAME_SAMPLES * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *chunk = heap_caps_malloc(REC_CHUNK_SAMPLES * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *pre = pending ? heap_caps_malloc(pre_max * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    esp_err_t e = r.acc && chunk && (!pending || pre) ? ESP_OK : ESP_ERR_NO_MEM;
    r.has_r0 = rssi_now(&r.r0);
    up_begin(job->job);
    react_init(&s_react, NULL, mole_fluid_idle_life());
    atomic_store(&s_hint, 0);
    if (e == ESP_OK) e = molty_mic_stream_start(16000, REC_CHUNK_MS, molty_mic_gain());
    const bool streaming = e == ESP_OK;
    r.t_mic0 = now_ms();
    /* a shake's listen on a board that feels it: the audio waits for the shake to settle */
    bool settling = streaming && pending && molty_board_has(MOLTY_CAP_IMU);
    r.shake = pending;
    trim_t trim = { 0, SETTLE_FADE_SAMPLES };
    if (settling) settle_begin(r.t_mic0);
    if (streaming) mole_fluid_listen(true);   /* the screen's listening ring, while the mic records */
    strcpy(r.h.stream, job->job);
    while (e == ESP_OK && r.verdict == VAD_CONTINUE) {
        if (atomic_load(&s_cancel)) {
            int w = PEND_WAIT;
            if (pending && atomic_compare_exchange_strong(&s_pend, &w, PEND_DEAD)) unclaimed = true;   /* link down */
            e = ESP_ERR_INVALID_STATE; break;
        }
        if (pending) {
            int st = atomic_load(&s_pend);
            if (st == PEND_WAIT && captured >= pre_max) {
                int w = PEND_WAIT;   /* nobody adopted it in time; an adoption racing this one wins */
                if (atomic_compare_exchange_strong(&s_pend, &w, PEND_DEAD)) st = PEND_DEAD;
                else st = w;
            }
            if (st == PEND_DECLINED || st == PEND_DEAD) {
                unclaimed = true; s_end_reason = st == PEND_DECLINED ? "declined" : "unclaimed";
                break;
            }
            if (st == PEND_ADOPTED && !settling) {
                /* the relay's listen from here: its id, window, end of turn and acknowledgement,
                 * as if it had begun with them — the audio so far (after the settle) replays */
                pending = false;
                strcpy(r.h.stream, s_adopt.job);
                up_lock(); strcpy(s_up_stream, s_adopt.job); up_unlock();
                r.ack = s_adopt.ack;
                rec_vad(&r, s_adopt.vad, s_adopt.rec_ms, s_adopt.end_silence_ms, s_adopt.no_speech_ms);
                for (size_t at = 0; at < pre_n && r.verdict == VAD_CONTINUE; at += REC_CHUNK_SAMPLES)
                    rec_take(&r, pre + at, pre_n - at < REC_CHUNK_SAMPLES ? pre_n - at : REC_CHUNK_SAMPLES);
                continue;
            }
        } else {
            const int stop = atomic_load(&s_stop);
            if (stop != STOP_NONE) {
                /* the relay's end of the listen: what is buffered goes out as the final frame, with
                 * the relay's reason — no_speech is acknowledged like the device's own (no chirp) */
                rec_end(&r, stop == STOP_NO_SPEECH ? "no_speech" : stop == STOP_TAP ? "tap" : "turn_end");
                break;
            }
        }
        size_t got = 0;
        e = molty_mic_stream_read(chunk, REC_CHUNK_SAMPLES, &got);
        if (e != ESP_OK) break;
        if (!got) { e = ESP_ERR_TIMEOUT; break; }
        captured += got;
        if (pending) {
            /* before adoption the VAD only drives the ring (its verdict waits for the relay's
             * settings); the ring swells with the voice (~10 Hz), live audio drives the reactions */
            vad_feed(&r.vad, chunk, got);
            const float level = vad_level(&r.vad);
            mole_fluid_listen_level(level);
            react_live(level, now_ms() - r.t_mic0);
            /* kept for the relay (its verdict waits for the relay's settings) */
            if (got > pre_max - pre_n) got = pre_max - pre_n;
            memcpy(pre + pre_n, chunk, got * 2); pre_n += got;
            if (settling) {
                const int res = settle_check((uint32_t)(captured / 16));
                if (res >= 0) {
                    /* settled: the handling noise before it goes, the rest fades in */
                    settling = false;
                    r.set = (uint32_t)res & 0xFFFFu; r.setc = (res >> 16) & 1;
                    r.ct_base = 16u * r.set;
                    trim.drop_left = r.ct_base; trim.fade = 0;
                    const size_t skip = trim_apply(&trim, pre, pre_n);
                    memmove(pre, pre + skip, (pre_n - skip) * 2); pre_n -= skip;
                }
            }
            continue;
        }
        const size_t skip = trim_apply(&trim, chunk, got);   /* a trim the mic had not reached yet */
        rec_take(&r, chunk + skip, got - skip);
        const float level = vad_level(&r.vad);
        mole_fluid_listen_level(level);
        react_live(level, now_ms() - r.t_mic0);
    }
    if (settling) atomic_store(&s_settling, false);
    /* stop the mic as soon as the listen is over: the creature leaves `listening` and
     * the relay is already transcribing */
    if (streaming) { molty_mic_stream_stop(); mole_fluid_listen(false); }
    if (unclaimed) { if (s_end_reason[0] == 'e') s_end_reason = "unclaimed"; e = ESP_OK; }
    else if (e != ESP_OK) s_end_reason = "error";
    /* Fail fast: an end-only frame tells the relay the stream is over instead of
     * leaving it to time out into no_audio ms + 5 s later. */
    if (e != ESP_OK && !atomic_load(&s_cancel) && r.acc && !pending && link_client_connected()) {
        r.have = 0;
        rec_end(&r, "error");
        s_end_reason = "error";
    }
    if (!r.acked) decide_ack(&r);
    if (atomic_load(&s_cancel) || unclaimed) up_abort();
    else up_flush();
    heap_caps_free(pre); heap_caps_free(chunk); heap_caps_free(r.acc);
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
        mole_diag_event(e != ESP_OK || s_up.fail || s_up.drop,
                        "audio.listen end %s took=%ums frames=%u bytes=%u reason=%s speech_ms=%u ack=%s fail=%u drop=%u",
                        esp_err_to_name(e), took, (unsigned)s_stat_frames, (unsigned)s_stat_bytes,
                        s_end_reason, (unsigned)s_speech_ms, link_ack_name(s_ack), (unsigned)s_up.fail, (unsigned)s_up.drop);
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
    if (molty_board_has(MOLTY_CAP_MIC)) {
        /* the uplink: ~10 s of frames (326 KB PSRAM) + the sender's frame buffer */
        int16_t *slots = heap_caps_malloc((size_t)UPLINK_SLOTS * UPLINK_SLOT_SAMPLES * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_up_frame = heap_caps_malloc(LINK_MESSAGE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_up_lock = xSemaphoreCreateMutex();
        s_settle_lock = xSemaphoreCreateMutex();
        s_up_kick = xQueueCreate(1, sizeof(uint8_t));
        if (!slots || !s_up_frame || !s_up_lock || !s_settle_lock || !s_up_kick) return ESP_ERR_NO_MEM;
        uplink_init(&s_up, slots);
        if (xTaskCreatePinnedToCoreWithCaps(up_task, "link_up", 6144, NULL, 4, NULL, tskNO_AFFINITY,
                                            MALLOC_CAP_SPIRAM) != pdPASS) return ESP_ERR_NO_MEM;
    }
    /* PSRAM stack: no flash ops on this task; internal RAM is the scarce one */
    return xTaskCreatePinnedToCoreWithCaps(audio_task, "link_audio", 6144, NULL, 4, NULL, tskNO_AFFINITY,
                                           MALLOC_CAP_SPIRAM) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

/* The job's bookkeeping, then the audio task gets it. */
static void begin(const audio_job_t *job, bool cued)
{
    strcpy(s_job, job->job);
    s_job_t0_ms = now_ms();
    s_mood_gen0 = link_cmd_mood_gen();   /* after our own listening/speaking mood */
    s_seq = 0; s_stat_frames = 0; s_stat_bytes = 0; s_busy = true; s_listen = job->listen; s_last_frame = esp_timer_get_time();
    s_cued = cued; s_cue_next = 0;
    s_mark_fired = 0; s_mark_mood = -1;
    atomic_store(&s_end, false); atomic_store(&s_cancel, false); atomic_store(&s_stop, STOP_NONE);
    atomic_store(&s_pend, job->pending ? PEND_WAIT : PEND_NONE);
    atomic_store(&s_chirp_cut, true);    /* a chirp still playing ends now: this job is next */
    /* activity log: its lines carry the relay job (ended in link_audio_tick); a shake's own
     * listen has no relay job until it is adopted (adopt() begins the tag then) */
    s_tagged = !job->pending;
    if (s_tagged) mole_diag_job_begin(job->job);
    xQueueSend(s_jobs, job, 0);
}

static esp_err_t adopt(const cJSON *args, const cJSON *local, cJSON **out);

static esp_err_t start(const cJSON *args, bool listen, cJSON **out)
{
    /* no mic / no speaker on this board: "unsupported", before any mood or text change */
    if (!molty_board_has(listen ? MOLTY_CAP_MIC : MOLTY_CAP_SPEAKER)) return ESP_ERR_NOT_SUPPORTED;
    const cJSON *local = listen ? cJSON_GetObjectItemCaseSensitive(args, "adopt") : NULL;
    if (local) return adopt(args, local, out);
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
    /* say marks (cap say_marks): they ride the word cues; without cues they are ignored */
    link_mark_t marks[LINK_MARKS_MAX]; int nmarks = 0;
    if (!listen && !link_marks_parse(args, marks, &nmarks)) return ESP_ERR_INVALID_ARG;
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
    if (listen) cJSON_AddNumberToObject(result, "filler_ms", mole_fluid_filler_ms());   /* the relay's auto filler (0 = off) */
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
        if (e == ESP_OK) s_say_text_ver = mole_fluid_text_ver();   /* the tint is for these words only */
        /* mood first: it selects the creature view; the text state then wins. */
        if (e == ESP_OK) e = link_cmd_mood("speaking", -1);
        if (e == ESP_OK) e = mole_creature_state_set("{\"state\":\"text\"}");
    }
    if (e != ESP_OK) { if (cued) mole_fluid_text_cue_end(); cJSON_Delete(result); return e; }
    link_ask_cancel();   /* say / listen, once accepted, take the screen from a pending question */
    audio_job_t job = {.listen = listen, .ms = listen ? (unsigned)rec_ms : (unsigned)ms->valueint,
                       .vad = vad, .end_silence_ms = end_silence_ms, .no_speech_ms = no_speech_ms, .ack = ack};
    strcpy(job.job, j->valuestring);
    s_nmarks = cued ? nmarks : 0;
    memcpy(s_marks, marks, sizeof(marks[0]) * (size_t)s_nmarks);
    begin(&job, cued);
    *out = result;
    return ESP_OK;
}

/* cmd listen {job, adopt: <local id>, rate, ms?, vad?, end_silence_ms?, no_speech_ms?, ack?}:
 * the relay takes the shake's listen as its job `job` (link_protocol.h "Shake to talk"). The
 * args are checked as a listen's (ring_color too, though the ring is already up: ignored);
 * `ms` absent = the setting, counted from when the shake began. Refused "listen_gone" when that
 * listen is not waiting any more (ended, declined, or another job), never "busy". Answers
 * like a listen: {job, ms, ack}. */
static esp_err_t adopt(const cJSON *args, const cJSON *local, cJSON **out)
{
    cJSON *j = cJSON_GetObjectItemCaseSensitive(args, "job");
    cJSON *rate = cJSON_GetObjectItemCaseSensitive(args, "rate");
    if (!cJSON_IsString(local) || !local->valuestring[0]) return ESP_ERR_INVALID_ARG;
    if (!cJSON_IsString(j) || !j->valuestring[0] || strlen(j->valuestring) >= sizeof(s_job)) return ESP_ERR_INVALID_ARG;
    if (!cJSON_IsNumber(rate) || rate->valuedouble != 16000) return ESP_ERR_INVALID_ARG;
    adopt_t a = {0};
    uint32_t window = 0;
    if (!link_listen_window(args, mole_fluid_listen_window(), &window, &a.rec_ms)) return ESP_ERR_INVALID_ARG;
    const vad_cfg_t vad_def = VAD_CFG_DEFAULT;
    a.vad = vad_def.enabled; a.end_silence_ms = vad_def.end_silence_ms; a.no_speech_ms = vad_def.no_speech_ms;
    if (!link_listen_args(args, &a.vad, &a.end_silence_ms, &a.no_speech_ms)) return ESP_ERR_INVALID_ARG;
    int32_t ring = -1;
    if (!link_color_arg(args, "ring_color", &ring)) return ESP_ERR_INVALID_ARG;
    if (!link_ack_arg(args, (link_ack_t)mole_fluid_ack_mode(), &a.ack)) return ESP_ERR_INVALID_ARG;
    if (!(s_busy && s_listen && !strcmp(local->valuestring, s_job) && atomic_load(&s_pend) == PEND_WAIT))
        return link_cmd_refuse("listen_gone");
    cJSON *result = cJSON_CreateObject();
    if (!result) return ESP_ERR_NO_MEM;
    strcpy(a.job, j->valuestring);
    s_adopt = a;                          /* before the CAS: the audio task reads it after ADOPTED */
    int w = PEND_WAIT;
    if (!atomic_compare_exchange_strong(&s_pend, &w, PEND_ADOPTED)) { cJSON_Delete(result); return link_cmd_refuse("listen_gone"); }
    strcpy(s_job, a.job);                 /* listen_stop / diag name the relay's job from now on */
    mole_diag_job_begin(a.job); s_tagged = true;   /* activity log: its lines carry the relay's job from here */
    cJSON_AddStringToObject(result, "job", a.job);
    cJSON_AddNumberToObject(result, "ms", window);
    cJSON_AddStringToObject(result, "ack", link_ack_name(a.ack));
    cJSON_AddNumberToObject(result, "filler_ms", mole_fluid_filler_ms());
    *out = result;
    return ESP_OK;
}

/* No other owner of the speaker, mic or screen, a relay that adopts, a link that is up. */
static bool shake_talk_ok(void)
{
    return atomic_load(&s_relay_adopts) && link_client_connected() && !s_busy &&
           !link_ask_pending() && !link_pair_active() && !self_update_busy();
}

/* The shake's own listen, at once: the listening mood and ring, the device's window and
 * acknowledgement settings; nothing goes up until the relay adopts it. */
static esp_err_t local_listen(char id[LINK_STREAM_MAX])
{
    audio_job_t job = {.listen = true, .pending = true, .ack = (link_ack_t)mole_fluid_ack_mode()};
    const uint32_t w = mole_fluid_listen_window();
    job.ms = w == LINK_LISTEN_UNLIMITED ? LINK_LISTEN_CEILING_MS : link_listen_setting_ok(w) ? w : LINK_LISTEN_DEFAULT_MS;
    const vad_cfg_t vad_def = VAD_CFG_DEFAULT;
    job.vad = vad_def.enabled; job.end_silence_ms = vad_def.end_silence_ms; job.no_speech_ms = vad_def.no_speech_ms;
    mole_fluid_listen_color(-1);
    esp_err_t e = link_cmd_mood("listening", -1);
    if (e != ESP_OK) return e;
    link_ask_cancel();
    snprintf(job.job, sizeof(job.job), "shake-%u", ++s_shake_n);
    strcpy(id, job.job);
    begin(&job, false);
    return ESP_OK;
}

/* Link task: the shake the sense task handed over → its listen (when nothing else owns the
 * device) and the shake event, naming that listen. */
static void shake_talk(void)
{
    char id[LINK_STREAM_MAX] = "";
    const bool from_gesture = atomic_exchange(&s_shake_gesture, false);
    if (shake_talk_ok() && local_listen(id) != ESP_OK) id[0] = 0;
    mole_diag_event(false, "sense.%s talk=%s", from_gesture ? "gesture" : "shake", id[0] ? id : "no");
    if (!link_client_connected()) return;
    if (from_gesture && !id[0]) return;                /* nothing listens: not a shake to report */
    cJSON *j = link_evt_shake_frame(s_shake_peak, s_shake_rev, s_shake_ms, id[0] ? id : NULL);
    if (j) link_send_json(j);
    cJSON_Delete(j);
}

bool link_audio_gesture_talk(void)
{
    if (!s_jobs || !molty_board_has(MOLTY_CAP_MIC)) return false;
    s_shake_peak = 0; s_shake_rev = 0; s_shake_ms = 0;
    atomic_store(&s_shake_gesture, true);
    atomic_store(&s_shake_req, true);
    return true;
}

bool link_audio_listening(uint32_t *age_ms)
{
    if (!s_busy || !s_listen) return false;
    if (age_ms) *age_ms = now_ms() - s_job_t0_ms;
    return true;
}

bool link_audio_say_waiting(void) { return s_busy && !s_listen && s_stat_frames == 0; }

void link_audio_tap_stop(void) { atomic_store(&s_stop, STOP_TAP); }

void link_audio_stop_any(void)
{
    if (!s_busy) return;
    if (s_listen && link_client_connected()) link_audio_tap_stop();
    else link_audio_cancel();
}

/* cmd say_cancel {job}: the relay gave THAT say up (its job failed): stop waiting for audio
 * that will not come. Any other job, or none, is answered {cancelling:false}. */
esp_err_t link_audio_say_cancel(const cJSON *args, cJSON **out)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(args, "job");
    if (!cJSON_IsString(j) || !j->valuestring[0] || strlen(j->valuestring) >= sizeof(s_job)) return ESP_ERR_INVALID_ARG;
    const bool mine = s_busy && !s_listen && !strcmp(j->valuestring, s_job);
    if (mine) link_audio_cancel();
    cJSON *result = cJSON_CreateObject();
    if (!result) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(result, "job", j->valuestring);
    cJSON_AddBoolToObject(result, "cancelling", mine);
    *out = result;
    return ESP_OK;
}

bool link_audio_shake(double peak_g, int reversals, unsigned ms)
{
    if (!s_jobs || !molty_board_has(MOLTY_CAP_MIC) || !mole_fluid_shake_talk()) return false;
    s_shake_peak = peak_g; s_shake_rev = reversals; s_shake_ms = ms;
    atomic_store(&s_shake_req, true);
    return true;
}

void link_audio_relay_adopts(bool on) { atomic_store(&s_relay_adopts, on); }

esp_err_t link_audio_say(const cJSON *args, cJSON **out) { return start(args, false, out); }
esp_err_t link_audio_listen(const cJSON *args, cJSON **out) { return start(args, true, out); }

/* cmd listen_stop {job, reason?}: ends THAT listen early with `reason` — "turn_end" (the
 * default: the human finished; acknowledged) or "no_speech" (nobody spoke; never
 * acknowledged: no chirp, no thinking); any other reason is refused. Naming a shake's listen
 * still waiting to be adopted (its local id) declines it: it ends quietly, whatever the reason. Never touches a say,
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
    int w = PEND_WAIT;
    /* a shake's listen the relay will not adopt (any reason): it ends quietly */
    if (mine && atomic_load(&s_pend) == PEND_WAIT) mine = atomic_compare_exchange_strong(&s_pend, &w, PEND_DECLINED);
    else if (mine) atomic_store(&s_stop, why);
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
    if (atomic_exchange(&s_shake_req, false)) shake_talk();
    if (!s_busy) return;
    if (!s_listen && link_say_stalled((uint32_t)((esp_timer_get_time() - s_last_frame) / 1000), s_stat_frames)) link_audio_cancel();
    esp_err_t e;
    if (xQueueReceive(s_done, &e, 0) != pdTRUE) return;
    drain(); /* worker has acknowledged completion; discard any late PCM */
    s_busy = false;
    /* Back to idle only if nobody chose a mood while the job ran: an explicit mood
     * (with its own hold_s, fluid_director) outlives the listen/say it arrived in. */
    /* A listen that heard speech is acknowledged (s_ack, decided at its end): `thinking`
     * instead of idle, LINK_ACK_THINK_S at most, until the agent's say / mood / ask / listen
     * replaces it — a mood the device sets itself, so the next job still takes over. */
    /* A say whose mood marks fired ends in its last one (the creature shows it as the words
     * end), unless an explicit mood came meanwhile — the say's own housekeeping, like idle. */
    if (!link_pair_active()) {
        if (s_listen && s_ack != LINK_ACK_OFF) link_cmd_mood_unless(s_mood_gen0, "thinking", LINK_ACK_THINK_S);
        else if (!s_listen && s_mark_mood >= 0) {
            const int m = s_mark_mood;
            const bool quick = !strcmp(link_mood_name(m), "happy") || !strcmp(link_mood_name(m), "surprised") || m == 0;
            link_cmd_mood_unless(s_mood_gen0, link_mood_name(m), quick ? -1 : MARK_MOOD_HOLD_S);
        }
        else link_cmd_mood_idle_unless(s_mood_gen0);
    }
    /* played to its end: idle life may smile once the creature is back (fd_turn_spoken) */
    if (!s_listen && e == ESP_OK && !atomic_load(&s_cancel)) mole_fluid_spoke();
    /* A failed/cancelled stream must never report successful playback. Listen
     * completion is the uplink end flag, not a say event. */
    if (!s_listen && e == ESP_OK && !atomic_load(&s_cancel) && link_client_connected()) {
        cJSON *j = link_say_done_frame(s_job);
        if (j) { link_send_json(j); cJSON_Delete(j); }
    }
    if (s_tagged) mole_diag_job_end(s_job);   /* every end (done, failed, cancelled) passes here; the link uploads its lines */
    s_tagged = false;
}
