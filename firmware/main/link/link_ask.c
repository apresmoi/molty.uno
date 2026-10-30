/*
 * link_ask.c — a question answered by a tap (link_ask.h).
 *
 * Threading: everything but link_ask_tap runs on the link task. The touch path sees only
 * atomics: s_open_gen (the generation 1..255 of the pending question, 0 = none), s_open_seq
 * (its number on screen, mole_fluid_ask_show) and the tap mailbox s_mailbox (gen << 56 |
 * x << 44 | y << 32 | the capture time's low 32 bits in µs; 0 = empty), claimed by
 * compare-exchange so the first tap is the one that answers.
 *
 * A tap answers only the question the panel showed when it landed (mole_fluid_ask_shown):
 * screen updates are asynchronous, so right after a new question replaced an old one the
 * old labels (or the letters morphing out of them) are still what the finger aimed at —
 * such a tap answers nothing. The link task drains the mailbox every tick and drops a tap
 * whose generation is not the pending question's (one that raced a cancel); a tap captured
 * at or before the deadline wins over the timeout, one captured after it loses.
 */
#include "link_ask.h"
#include "link_protocol.h"
#include "link_client.h"
#include "mole_creature.h"
#include "mole_fluid.h"
#include "molty_board.h"
#include "fluid_ask.h"
#include "esp_timer.h"
#include "mole_diag.h"
#include <math.h>
#include <string.h>
#include <stdatomic.h>

_Static_assert(LINK_ASK_MAX == FLUID_ASK_MAX && LINK_ASK_LABEL_MAX == FLUID_ASK_LABEL_MAX, "link and screen agree on the limits");

static struct {
    bool open;
    unsigned gen, seq;          /* seq: its number on screen (mole_fluid_ask_show) */
    char job[LINK_STREAM_MAX];
    char label[LINK_ASK_MAX][LINK_ASK_LABEL_MAX + 1];
    int n;
    int64_t deadline;
    link_ask_done_t done;   /* NULL: the agent's ask (an `answer` event) */
} s_ask;
static atomic_uint s_open_gen, s_open_seq;
static _Atomic uint64_t s_mailbox;

bool link_ask_pending(void) { return atomic_load(&s_open_gen) != 0; }

bool link_ask_tap(float dx, float dy)
{
    uint32_t at = (uint32_t)esp_timer_get_time();   /* captured now: the deadline judges this */
    unsigned g = atomic_load(&s_open_gen);
    if (!g) return false;
    /* still not a `tap` event, whatever it lands on: a question is pending */
    unsigned shown = mole_fluid_ask_shown();
    if (!shown || shown != atomic_load(&s_open_seq)) return true;   /* aimed at another question's labels */
    long x = lrintf(dx) + 2048, y = lrintf(dy) + 2048;
    uint64_t v = (uint64_t)g << 56 | (uint64_t)(x < 0 ? 0 : x > 4095 ? 4095 : x) << 44 |
                 (uint64_t)(y < 0 ? 0 : y > 4095 ? 4095 : y) << 32 | at;
    uint64_t none = 0;
    atomic_compare_exchange_strong(&s_mailbox, &none, v);   /* a second tap before the tick: the first wins */
    return true;
}

static void emit_answer(int index, const char *reason)
{
    if (s_ask.done) { s_ask.done(index, reason); return; }   /* the firmware's own question */
    if (!link_client_connected()) return;   /* events go out only while the link is up */
    cJSON *j = link_evt_answer_frame(s_ask.job, index, index >= 0 ? s_ask.label[index] : NULL, reason);
    if (j) link_send_json(j);
    cJSON_Delete(j);
}

static void close_ask(void)
{
    s_ask.open = false;
    atomic_store(&s_open_gen, 0);
    atomic_store(&s_open_seq, 0);
    atomic_store(&s_mailbox, 0);
}

/* end the pending question unanswered; screen: let the fluid melt it back */
static void finish(const char *reason, bool screen)
{
    if (!s_ask.open) return;
    if (screen) mole_fluid_ask_end();
    close_ask();
    mole_diag_event(false, "ask.%s job=%s", reason, s_ask.job);
    emit_answer(-1, reason);
}

void link_ask_cancel(void) { finish("cancelled", true); }

static esp_err_t open_question(const char *job, char label[LINK_ASK_MAX][LINK_ASK_LABEL_MAX + 1], const int32_t *colors,
                               int n, unsigned timeout_ms, link_ask_done_t done)
{
    esp_err_t e = mole_creature_select("fluid");
    if (e != ESP_OK) return e;
    /* a pending question is cancelled first; the new labels replace it on screen */
    finish("cancelled", false);
    const char *labels[LINK_ASK_MAX];
    for (int i = 0; i < n; i++) labels[i] = label[i];
    unsigned seq = 0;
    e = mole_fluid_ask_show(n, labels, colors, &seq);
    if (e != ESP_OK) { mole_fluid_ask_end(); return e; }
    strcpy(s_ask.job, job);
    memcpy(s_ask.label, label, sizeof(s_ask.label));
    s_ask.n = n;
    s_ask.done = done;
    s_ask.deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    s_ask.gen = s_ask.gen % 255 + 1;
    s_ask.seq = seq;
    s_ask.open = true;
    atomic_store(&s_mailbox, 0);
    atomic_store(&s_open_seq, seq);
    atomic_store(&s_open_gen, s_ask.gen);
    return ESP_OK;
}

esp_err_t link_ask_cmd(const cJSON *args, cJSON **out)
{
    if (!molty_board_has(MOLTY_CAP_TOUCH)) return ESP_ERR_NOT_SUPPORTED;   /* nothing could answer */
    char job[LINK_STREAM_MAX], label[LINK_ASK_MAX][LINK_ASK_LABEL_MAX + 1];
    int32_t colors[LINK_ASK_MAX];
    int n; unsigned timeout_ms;
    if (!link_ask_args(args, job, label, colors, &n, &timeout_ms)) return ESP_ERR_INVALID_ARG;
    cJSON *result = cJSON_CreateObject();
    if (!result) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(result, "job", job);
    esp_err_t e = open_question(job, label, colors, n, timeout_ms, NULL);
    if (e != ESP_OK) { cJSON_Delete(result); return e; }
    *out = result;
    return ESP_OK;
}

esp_err_t link_ask_open(const char *job, const char *const *labels, const int32_t *colors, int n, unsigned timeout_ms, link_ask_done_t done)
{
    if (!molty_board_has(MOLTY_CAP_TOUCH)) return ESP_ERR_NOT_SUPPORTED;
    if (!job || !job[0] || strlen(job) >= LINK_STREAM_MAX || n < 1 || n > LINK_ASK_MAX || !done) return ESP_ERR_INVALID_ARG;
    char label[LINK_ASK_MAX][LINK_ASK_LABEL_MAX + 1];
    memset(label, 0, sizeof(label));
    for (int i = 0; i < n; i++) {
        if (!labels[i] || !labels[i][0] || strlen(labels[i]) > LINK_ASK_LABEL_MAX) return ESP_ERR_INVALID_ARG;
        strcpy(label[i], labels[i]);
    }
    return open_question(job, label, colors, n, timeout_ms, done);
}

void link_ask_tick(void)
{
    uint64_t t = atomic_exchange(&s_mailbox, 0);
    if (!s_ask.open) return;
    /* a tap of this question captured at or before the deadline (wrap-safe: ≤ 120 s apart) */
    if (t && t >> 56 == s_ask.gen && (int32_t)((uint32_t)t - (uint32_t)s_ask.deadline) <= 0) {
        float dx = (float)(t >> 44 & 4095u) - 2048.f, dy = (float)(t >> 32 & 4095u) - 2048.f;
        int index = fluid_ask_pick(s_ask.n, dx, dy);
        mole_fluid_ask_pick(index);   /* the screen first: the send may block on a slow socket */
        close_ask();
        mole_diag_event(false, "ask.answer job=%s index=%d", s_ask.job, index);
        emit_answer(index, NULL);
        return;
    }
    if (esp_timer_get_time() >= s_ask.deadline) finish("timeout", true);
}
