/*
 * SPDX-License-Identifier: MIT
 *
 * say_flow_test.c: a say's words against its voice, through mole_fluid.c itself (the web module's native build,
 * web_molty.c, included whole, as picture_flow_test.c does) at the device's frame rate, link_audio.c's part stood in
 * as it does it: the say's text and state, its first sound held by the gate (SAY_GATE below: its first words shown, then
 * the sound waits for the first on the glass), each word's cue revealed as the speaker reaches it less the reveal lead,
 * the say's end, the mood back to idle. Every frame the glass is read: a word counts as shown in a frame when the pixels
 * of its letters (as the reader shows it standing, read once per word before the run, in a process of its own) are lit
 * (where it rides the wave: 4 px up or down), 60 % of them at least, and 60 % at least of the lit pixels in its band are
 * its own (the word it is most like).
 *   - every word spoken is shown, the first on the glass before its sound (34 ms at most after it), every later one
 *     within 34 ms of its sound while the cat's dust leaves (a cue that comes later than that, the cloud's: two frames
 *     after it comes), and the one due on the glass on every frame of the say;
 *   - the voice never held more than 70 ms for the picture;
 *   - the change into the words and back: each one change, each at most WORDS_CHANGE_S, every frame within the device
 *     model's 33.4 ms, the fluid held still under the change into the words (s_sim->p.still from its second frame on);
 *   - the way back to the cat starts once the voice is over, the change that brought the words has ended and the last
 *     word was readable 0.4 s; never while a word is spoken;
 *   - the device's log line for the say ("say words= spoken= shown= first= gate= into=") says the same.
 * The cases: a filler ("Hmm.", "Good question."), a long say on the cloud voice (cues estimated, the audio there with
 * the command), the voice box's (the command first, the audio 300 ms later, its own word timing), a faster voice path
 * (the audio 40 ms after the command), the cloud's cues coming late (300 ms after the first sound), a say right after a
 * listen and its chirp.
 *
 *   say_flow_test <repo root> <case>   (say_flow_build.sh runs every case)
 */
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "web_sdk.h"
static int bg_create(void (*fn)(void *), void *arg, void **out);
#define xTaskCreatePinnedToCoreWithCaps(fn, name, stack, arg, prio, out, core, caps) bg_create(fn, arg, (void **)(out))
#include "../../web/web_molty.c"
static int bg_create(void (*fn)(void *), void *arg, void **out) { (void)fn; (void)arg; (void)out; return pdFALSE; }   /* made in place */

static int s_pass, s_fail;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define PX      466
#define FRAME   33.4                   /* ms: a frame on the device, two refreshes of its panel */
#define BRIGHT  120
#define BX0 20                         /* the reader's band, where a word's letters stand */
#define BX1 446
#define BY0 190
#define BY1 280
#define BW (BX1 - BX0)
#define BH (BY1 - BY0)
#define MAXW 24
#define TH 0.6f
#define LATE_MS 34.0                   /* a word on the glass this long after its sound at most (a frame) */
#define GATE_MS 70.0                   /* link_audio.c SAY_GATE_MS: the voice held for the picture at most */
#define TICK_MS 10.0                   /* .. a tick at a time (FreeRTOS at 100 Hz) */
#define LEAD_MS 50.0                   /* link_audio.c CUE_LEAD_SAMPLES: a word revealed this much before its cue */
#define WRITE_MS 15.0                  /* .. checked once a speaker write */
#define DRAIN_MS 90.0                  /* the speaker's DMA ring drained after the last sample */
#define WORDS_CHANGE_S 0.6             /* the change into a say's words and back: at most this long (mole_fluid.c
                                        * WORDS_IN_S, WORDS_OUT_S, with its frames that read the pictures) */
#ifdef SAY_TAG                         /* (the firmware with the gate: today's code has none, its voice starts at once) */
#define HAVE_GATE 1
#else
#define HAVE_GATE 0
#endif

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "say_flow_test: cannot read %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    b[n] = 0; *len = (size_t)n; fclose(f);
    return b;
}

static double s_t;                     /* ms since the say's command: the end of the last frame (its pixels on the glass) */
static void frame(void) { wm_frame((uint32_t)(FRAME * 1000. + 0.5)); s_t += FRAME; char *e = wm_events(); free(e); }

/* the letters' pixels in the band: bright, and solid (the 3 x 3 block from it all bright: a stroke is 4 px wide, a dot
 * of the haze 1 or 2) */
static void band(uint8_t *out) {
    const uint8_t *p = wm_pixels();
    for (int y = BY0; y < BY1; y++) for (int x = BX0; x < BX1; x++) {
        bool all = true;
        for (int dy = 0; dy < 3 && all; dy++) for (int dx = 0; dx < 3 && all; dx++) {
            const uint8_t *q = p + ((size_t)(y + dy) * PX + x + dx) * 4;
            all = q[0] > BRIGHT || q[1] > BRIGHT || q[2] > BRIGHT;
        }
        out[(y - BY0) * BW + (x - BX0)] = all;
    }
}

/* a mask grown by the wave's reach: a letter rides the surface 3 px up or down (and a pixel across) */
static void dilate(const uint8_t *in, uint8_t *out) {
    for (int y = 0; y < BH; y++) for (int x = 0; x < BW; x++) {
        bool any = false;
        for (int dy = -4; dy <= 4 && !any; dy++) for (int dx = -1; dx <= 1 && !any; dx++) {
            const int yy = y + dy, xx = x + dx;
            any = yy >= 0 && yy < BH && xx >= 0 && xx < BW && in[yy * BW + xx];
        }
        out[y * BW + x] = any;
    }
}

static int split(const char *text, int off[MAXW], int len[MAXW]) {
    int n = 0, i = 0;
    const int L = (int)strlen(text);
    while (i < L && n < MAXW) {
        while (i < L && text[i] == ' ') i++;
        if (i >= L) break;
        off[n] = i;
        while (i < L && text[i] != ' ') i++;
        len[n] = i - off[n]; n++;
    }
    return n;
}

static char *s_cat; static size_t s_cat_len;
static void power_on(void) {
    if (wm_creature("cat", s_cat, s_cat_len) != 0 || wm_start(PX, 20260516, 0) != 0) { fprintf(stderr, "power on\n"); exit(2); }
    wm_device(1);
    for (int i = 0; i < 150; i++) frame();
}

/* each word as the reader shows it standing: its letters' pixels (in a process of its own: the module starts once) */
static void refs_of(const char *text, int nw, uint8_t (*ref)[BW * BH]) {
    uint8_t (*sh)[BW * BH] = mmap(NULL, sizeof(*sh) * MAXW, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sh == MAP_FAILED) exit(2);
    const pid_t pid = fork();
    if (pid == 0) {
        power_on();
        mole_fluid_text_cue(-1);
        mole_fluid_set_text(text, (int)strlen(text));
        wm_state("{\"state\":\"creature\",\"mood\":\"speaking\"}");
        wm_state("{\"state\":\"text\"}");
        for (int k = 0; k < nw; k++) {
            mole_fluid_text_cue(k);
            for (int i = 0; i < (k ? 30 : 120); i++) frame();
            band(sh[k]);
        }
        _exit(0);
    }
    int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st)) { fprintf(stderr, "refs failed\n"); exit(2); }
    memcpy(ref, sh, sizeof(*sh) * (size_t)nw);
    munmap(sh, sizeof(*sh) * MAXW);
}

/* the device's log line for the say (mole_fluid.c say_log) */
static char s_say_line[256];
#if HAVE_GATE
static void tap(const char *line) { if (!strncmp(line, "say ", 4)) snprintf(s_say_line, sizeof(s_say_line), "%s", line); }
#endif

typedef struct {
    const char *name, *text;
    double audio_at;                   /* ms after the command: its first audio in hand (the cloud: with it) */
    double audio_ms;                   /* the say's audio */
    const double *cue;                 /* each word's start in the audio, ms (NULL: in proportion to its characters, as
                                        * the relay estimates them for the cloud voice) */
    double cues_at;                    /* ms after the first sound: the cues reach the device (0: with their audio) */
    bool listen;                       /* a listen and its chirp first */
} say_case_t;

/* when a word must be on the glass: LATE_MS after its sound; a cue that reaches the device later than that (the cloud's,
 * made once a sentence's audio is whole: cues_at), two frames after it arrives (the frame that takes it, the one it shows) */
static double due_at(const say_case_t *c, double sound, double cue) {
    const double arrives = sound + c->cues_at, reveal = sound + cue - LEAD_MS;
    return arrives > reveal ? fmax(sound + cue + LATE_MS, arrives + 2. * FRAME) : sound + cue + LATE_MS;
}

static void run(const say_case_t *c) {
    int off[MAXW], len[MAXW];
    const int nw = split(c->text, off, len);
    static uint8_t ref[MAXW][BW * BH], now[BW * BH];
    refs_of(c->text, nw, ref);
    static uint8_t refd[MAXW][BW * BH], nowd[BW * BH];
    int refn[MAXW];
    for (int k = 0; k < nw; k++) { refn[k] = 0; for (int i = 0; i < BW * BH; i++) refn[k] += ref[k][i]; dilate(ref[k], refd[k]); }
    double cue[MAXW];
    if (c->cue) for (int k = 0; k < nw; k++) cue[k] = c->cue[k];
    else {
        int chars = 0, before = 0;
        for (int k = 0; k < nw; k++) chars += len[k] + 1;
        for (int k = 0; k < nw; k++) { cue[k] = c->audio_ms * before / chars; before += len[k] + 1; }
    }

    power_on();
#if HAVE_GATE
    s_diag_tap = tap;
#endif
    if (c->listen) {                   /* a listen (its ring, the voice on it), then the turn's chirp: thinking */
        wm_listen(1);
        for (int i = 0; i < 60; i++) { wm_listen_level(i % 9 < 5 ? 0.5f : 0.1f); frame(); }
        wm_listen(0);
        wm_state("{\"mood\":\"thinking\"}");
        for (int i = 0; i < 9; i++) frame();
    }
    s_t = 0.;
    /* link_audio.c start(): following, the text, the mood, the state */
    mole_fluid_text_cue(-1);
    mole_fluid_set_text(c->text, (int)strlen(c->text));
    const unsigned ver = mole_fluid_text_ver();
    (void)ver;
    wm_state("{\"state\":\"creature\",\"mood\":\"speaking\"}");
    wm_state("{\"state\":\"text\"}");

    double sound = -1., gate = 0.;     /* the first sound's time; how long it waited */
    int next = 0;                      /* the next word to reveal */
    bool ended = false, home = false, was = false;
    double spoken_at[MAXW], shown_at[MAXW];
    for (int k = 0; k < nw; k++) spoken_at[k] = shown_at[k] = -1.;
    int changes = 0, cuts = 0, slow = 0, nchg[2] = { 0, 0 }, not_still = 0, missing = 0;
    double chg0[2] = { -1., -1. }, chg1[2] = { -1., -1. }, back_at = -1., last_shown_at = -1.;
    int after = 0;
    char miss[160] = "";
    for (int i = 0; i < 600; i++) {
        const double t0 = s_t;         /* this frame: from t0 to s_t */
        /* the audio task, before this frame's sim reads its inputs */
        if (sound < 0. && t0 >= c->audio_at) {
            if (!next && (c->cues_at <= 0. || cue[0] <= LEAD_MS)) { mole_fluid_text_cue(0); next = 1; }   /* (word 0: at the stream's start) */
            if (!HAVE_GATE) { sound = c->audio_at; }
        }
        if (sound >= 0.) {
            const double known = sound + c->cues_at;
            for (; next < nw && t0 >= known; next++) {
                /* (each 15 ms write: the words whose cue less the lead the speaker has played) */
                const double due = sound + cue[next] - LEAD_MS, at = sound + ceil((due - sound) / WRITE_MS) * WRITE_MS;
                if (t0 < at) break;
                mole_fluid_text_cue(next);
            }
            if (!ended && t0 >= sound + c->audio_ms + DRAIN_MS) {
                mole_fluid_text_cue_end();
#if HAVE_GATE
                mole_fluid_say_spoken(ver, nw);
#endif
                ended = true;
            }
            if (ended && !home && t0 >= sound + c->audio_ms + DRAIN_MS + 70.) { wm_state("{\"state\":\"creature\",\"mood\":\"idle\"}"); home = true; }
        }
        frame();
        const bool on = fluid_draw_morphing(s_draw);
        /* the gate (link_audio.c say_gate): a tick at a time, from the audio in hand, until the glass has the first word */
        if (HAVE_GATE && sound < 0. && s_t >= c->audio_at) {
#if HAVE_GATE
            const bool shown = mole_fluid_text_shown(ver, 0);
#else
            const bool shown = true;
#endif
            const double ticks = shown ? ceil((s_t - c->audio_at) / TICK_MS) : floor(GATE_MS / TICK_MS);
            if (shown || s_t - c->audio_at >= GATE_MS) {
                gate = fmin(ticks * TICK_MS, GATE_MS);
                sound = c->audio_at + gate;
#if HAVE_GATE
                mole_fluid_say_sound(ver, (int)gate);
#endif
            }
        }
        /* the word the voice is on, and the one due on the glass */
        int due = -1;
        for (int k = 0; sound >= 0. && k < nw; k++) {
            if (s_t >= sound + cue[k] && s_t < sound + c->audio_ms && spoken_at[k] < 0.) spoken_at[k] = sound + cue[k];
            if (s_t >= due_at(c, sound, cue[k])) due = k;
        }
        if (getenv("SAY_FLOW_DUMP")) {         /* (each frame as a PPM, its time since the command in its name) */
            char fn[1024];
            snprintf(fn, sizeof(fn), "%s/%s-%05.0f.ppm", getenv("SAY_FLOW_DUMP"), c->name, s_t);
            FILE *f = fopen(fn, "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", PX, PX);
                const uint8_t *p = wm_pixels();
                for (int k = 0; k < PX * PX; k++) fwrite(p + (size_t)k * 4, 1, 3, f);
                fclose(f);
            }
        }
        /* the words on the glass */
        band(now);
        int lit = 0;
        for (int q = 0; q < BW * BH; q++) lit += now[q];
        dilate(now, nowd);
        int best = -1;
        float bs = 0.f;
        for (int k = 0; k < nw; k++) {         /* (the word it is most like, if it is like it at all) */
            int got = 0, own = 0;
            for (int q = 0; q < BW * BH; q++) { got += ref[k][q] && nowd[q]; own += now[q] && refd[k][q]; }
            const float rec = refn[k] ? (float)got / (float)refn[k] : 0.f, prec = lit ? (float)own / (float)lit : 0.f;
            if (rec >= TH && prec >= TH && rec * prec > bs + 1e-4f) { best = k; bs = rec * prec; }
        }
        if (best >= 0) {                       /* (a word said twice looks the same: the first of them not shown yet) */
            for (int k = 0; k < nw; k++)
                if (len[k] == len[best] && !memcmp(c->text + off[k], c->text + off[best], (size_t)len[k]) && shown_at[k] < 0.) { best = k; break; }
            if (shown_at[best] < 0.) shown_at[best] = s_t;
        }
        if (best >= 0) last_shown_at = s_t;
        if (due >= 0 && best < due && s_cur == ST_TEXT && sound >= 0. && s_t < sound + c->audio_ms) {
            missing++;
            if (strlen(miss) < sizeof(miss) - 24) snprintf(miss + strlen(miss), sizeof(miss) - strlen(miss), " %.0f:%d/%d", s_t, best, due);
        }
        /* the changes of picture */
        const int dir = s_cur == ST_TEXT ? 0 : 1;
        if (on && !was) { changes++; chg0[dir] = t0; if (dir == 1) back_at = t0; }
        if (on && was && dir == 1 && chg0[1] < 0.) cuts++;
        if (on) {
            nchg[dir]++; chg1[dir] = s_t;
            slow += wm_frame_info()[FI_FRAME_MS] > 33.4f;
            if (dir == 0 && nchg[0] > 1) not_still += !s_sim->p.still;
        }
        was = on;
        if (!on && home && s_cur == ST_CREATURE && ++after > 15) break;
    }
    /* the summary */
    int lost = 0, first = -1;
    char lost_w[256] = "";
    for (int k = 0; k < nw; k++) {
        if (spoken_at[k] >= 0. && shown_at[k] < 0.) { lost++; snprintf(lost_w + strlen(lost_w), sizeof(lost_w) - strlen(lost_w), " %.*s", len[k], c->text + off[k]); }
        if (shown_at[k] >= 0. && (first < 0 || shown_at[k] < shown_at[first])) first = k;
    }
    const double late0 = first >= 0 ? shown_at[first] - sound : 1e9;
    double worst = 0.; int worst_k = -1;
    bool on_time = true;                   /* (each word by when it is due: its sound, or its cue's arrival when that is later) */
    for (int k = 0; k < nw; k++) {
        if (shown_at[k] >= 0. && spoken_at[k] >= 0. && shown_at[k] - spoken_at[k] > worst) { worst = shown_at[k] - spoken_at[k]; worst_k = k; }
        if (shown_at[k] >= 0. && spoken_at[k] >= 0. && shown_at[k] > due_at(c, sound, cue[k])) on_time = false;
    }
    const double into_s = chg0[0] >= 0. ? (chg1[0] - chg0[0]) / 1000. : 0., back_s = chg0[1] >= 0. ? (chg1[1] - chg0[1]) / 1000. : 0.;
    printf("%s: %d words, the first sound %.0f ms after the command (held %.0f ms); first word on the glass %+.0f ms from its sound; "
           "the latest word %+.0f ms (%d); spoken but never shown %d%s\n",
           c->name, nw, sound, gate, late0, worst, worst_k, lost, lost_w);
    printf("  change into the words %d frames %.2f s, back %d frames %.2f s (back %.0f ms after the voice ended, %.0f ms after the last word came);"
           " frames over 33.4 ms %d; due word missing on %d frames%s\n",
           nchg[0], into_s, nchg[1], back_s, back_at - (sound + c->audio_ms), back_at - last_shown_at, slow, missing, miss);
    if (s_say_line[0]) printf("  log: %s\n", s_say_line);
    if (getenv("SAY_FLOW_WORDS"))                      /* (each word: spoken at, shown at, ms after the command) */
        for (int k = 0; k < nw; k++) printf("    %2d %-10.*s %6.0f %6.0f\n", k, len[k], c->text + off[k], spoken_at[k], shown_at[k]);
    CHECK(lost == 0, "%s: %d words spoken but never shown:%s", c->name, lost, lost_w);
    CHECK(first == 0 && late0 <= LATE_MS, "%s: the first word on the glass %.0f ms after its sound (word %d)", c->name, late0, first);
    CHECK(on_time, "%s: a word on the glass after it was due (the latest %.0f ms after its sound, word %d)", c->name, worst, worst_k);
    CHECK(missing == 0, "%s: the word due on the glass missing on %d frames (t:shown/due)%s", c->name, missing, miss);
    CHECK(gate <= GATE_MS, "%s: the voice held %.0f ms for the picture", c->name, gate);
    CHECK(changes == 2 && cuts == 0, "%s: the words came and went by a change each (%d changes, %d turned back)", c->name, changes, cuts);
    CHECK(into_s <= WORDS_CHANGE_S && back_s <= WORDS_CHANGE_S, "%s: the changes %.2f s and %.2f s (at most %.2f)", c->name, into_s, back_s, WORDS_CHANGE_S);
    CHECK(slow == 0, "%s: %d frames of the changes over 33.4 ms in the device model", c->name, slow);
    CHECK(not_still == 0, "%s: the fluid moved under the change into the words on %d frames", c->name, not_still);
    CHECK(back_at >= sound + c->audio_ms, "%s: the way back started %.0f ms before the voice ended", c->name, sound + c->audio_ms - back_at);
#if HAVE_GATE
    char want[96];
    snprintf(want, sizeof(want), "say words=%d spoken=%d shown=%d ", nw, nw, nw);
    CHECK(!strncmp(s_say_line, want, strlen(want)) && strstr(s_say_line, "into=") && !strstr(s_say_line, "into=0f"),
          "%s: the device's log line (\"%s\", want \"%s...\")", c->name, s_say_line, want);
#endif
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: say_flow_test <repo root> <case>\n"); return 2; }
    char path[1024];
    snprintf(path, sizeof(path), "%s/packages/cat/cat.json", argv[1]);
    s_cat = slurp(path, &s_cat_len);
    static const char B[] = "All good, Juan. I am timing each step of this chat. Tell me, what did you eat today?";
    static const char F[] = "Got it. Last one: count from one to five, slowly.";
    /* the voice box's own word timing for F: pauses at its stops, a quick run of short words */
    static const double FCUE[] = { 0., 190., 640., 930., 1380., 1690., 1880., 2090., 2230., 2760. };
    static const say_case_t cases[] = {
        { "filler",   "Hmm.",           5.,   320.,  NULL, 0.,   false },
        { "filler2",  "Good question.", 5.,   1160., NULL, 0.,   false },
        { "long",     B,                5.,   5200., NULL, 0.,   false },
        { "voicebox", F,                300., 3400., FCUE, 0.,   false },
        { "fast",     F,                40.,  3400., FCUE, 0.,   false },
        { "latecues", B,                5.,   5200., NULL, 300., false },
        { "listen",   B,                5.,   5200., NULL, 0.,   true  },
    };
    const char *c = argv[2];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        if (!strcmp(c, cases[i].name)) { run(&cases[i]); printf("say_flow_test %s: %d passed, %d failed\n", c, s_pass, s_fail); return s_fail ? 1 : 0; }
    fprintf(stderr, "say_flow_test: no case %s\n", c);
    return 2;
}
