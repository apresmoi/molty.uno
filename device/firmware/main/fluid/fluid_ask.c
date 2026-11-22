/*
 * SPDX-License-Identifier: MIT
 *
 * fluid_ask.c — a question on the round screen: its layout, which answer a tap picks, its screen's phases
 * (fluid_ask.h). The layout is a header, then "Rim arcs" (1..3 answers) or "Underlined rows" (4..5).
 * Plain C: built into the firmware and by test/host/ask_test.c and the link tests.
 */
#include "fluid_ask.h"

#include <math.h>
#include <string.h>

#define PI     3.14159265f
#define HALF   206.f                 /* half the 412 px stage the sizes are in */
#define GAP    0.045f                /* radians each arc stops short of its neighbour */
#define ARC_R  0.962f                /* the arcs' line, of the radius */
#define FRAME_R 0.975f               /* rows: the faint ring round the rim */
static const uint32_t TITLE_RGB = 0xF4F0FA, DETAIL_RGB = 0x9692A8, ICE_RGB = 0x96A0BE;

/* services/relay/src/core/commands.ts COLORS: green, red, amber, blue, violet */
const uint32_t FLUID_ASK_COLORS[FLUID_ASK_MAX] = { 0x3ccf96, 0xff6b6b, 0xffa94d, 0x4dabf7, 0xb197fc };

int fluid_ask_label_max(int n) { return n <= 2 ? 14 : n == 3 ? 10 : 8; }

uint32_t fluid_ask_color(int32_t given, int i) {
    if (given >= 0) return (uint32_t)given & 0xFFFFFFu;
    return FLUID_ASK_COLORS[i < 0 ? 0 : i % FLUID_ASK_MAX];
}

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
/* a text's width in font pixels, and the font pixel that fits w stage px (at most mx) */
static float text_w(int len) { return (float)(len * 6 - 1); }
static float fit_px(int len, float w, float mx) { const float p = w / text_w(len < 1 ? 1 : len); return p < mx ? p : mx; }
/* the disc's chord at height y, stage px, times m */
static float chord(float y, float m) { return 2.f * sqrtf(fmaxf(0.f, 1.f - y * y)) * HALF * m; }

static fluid_ask_item_t *item(fluid_ask_layout_t *L, const char *s, int len, float x, float y, float px, uint32_t rgb, int role, int opt) {
    fluid_ask_item_t *it = &L->item[L->nitems++];
    if (len > FLUID_ASK_STR - 1) len = FLUID_ASK_STR - 1;
    for (int i = 0; i < len; i++) it->str[i] = s[i] >= 'a' && s[i] <= 'z' ? (char)(s[i] - 32) : s[i];   /* the font's capitals */
    it->str[len] = 0; it->len = len;
    it->x = x; it->y = y; it->px = px; it->rgb = rgb; it->role = (uint8_t)role; it->opt = (int8_t)opt;
    return it;
}

/* the header from `top` down: the title (two lines when one would be under 2.4 px), the detail, the code; its bottom */
static float header(const fluid_ask_q_t *q, fluid_ask_layout_t *L, float top) {
    float y = top;
    const char *t = q->title;
    int n = (int)strlen(t);
    if (n) {
        float px = fit_px(n, chord(y + 0.05f, 0.86f), 3.f);
        int at = -1;
        if (px < 2.4f) for (int i = 1; i < n; i++)      /* the space nearest the middle */
            if (t[i] == ' ' && (at < 0 || fabsf(i - n * 0.5f) < fabsf(at - n * 0.5f))) at = i;
        if (at > 0) {
            const int l2 = n - at - 1;
            px = fminf(fit_px(at, chord(y + 0.05f, 0.86f), 3.f), fit_px(l2, chord(y + 0.15f, 0.86f), 3.f));
            y += 7.f * px / HALF / 2.f; item(L, t, at, 0.f, y, px, TITLE_RGB, FLUID_ASK_TITLE, -1); y += 7.f * px / HALF / 2.f + 0.035f;
            y += 7.f * px / HALF / 2.f; item(L, t + at + 1, l2, 0.f, y, px, TITLE_RGB, FLUID_ASK_TITLE, -1); y += 7.f * px / HALF / 2.f + 0.035f;
        } else {
            y += 7.f * px / HALF / 2.f; item(L, t, n, 0.f, y, px, TITLE_RGB, FLUID_ASK_TITLE, -1); y += 7.f * px / HALF / 2.f + 0.035f;
        }
    }
    if ((n = (int)strlen(q->detail)) > 0) {
        const float dp = fit_px(n, chord(y + 0.04f, 0.86f), 1.7f);
        y += 7.f * dp / HALF / 2.f + 0.01f; item(L, q->detail, n, 0.f, y, dp, DETAIL_RGB, FLUID_ASK_DETAIL, -1); y += 7.f * dp / HALF / 2.f + 0.03f;
    }
    if ((n = (int)strlen(q->code)) > 0) {                 /* its digits spaced out */
        char sp[2 * FLUID_ASK_CODE_MAX];
        int m = 0;
        for (int i = 0; i < n && i < FLUID_ASK_CODE_MAX; i++) { if (m) sp[m++] = ' '; sp[m++] = q->code[i]; }
        const float cp = 5.5f;
        y += 7.f * cp / HALF / 2.f + 0.03f; item(L, sp, m, 0.f, y, cp, TITLE_RGB, FLUID_ASK_CODE, -1); y += 7.f * cp / HALF / 2.f + 0.03f;
    }
    return y;
}

void fluid_ask_lay(const fluid_ask_q_t *q, fluid_ask_layout_t *L) {
    memset(L, 0, sizeof(*L));
    const int n = q->n < 1 ? 1 : q->n > FLUID_ASK_MAX ? FLUID_ASK_MAX : q->n, lmax = fluid_ask_label_max(n);
    L->hit.n = n; L->hit.rows = n > FLUID_ASK_ARCS_MAX;
    if (!L->hit.rows) {                                       /* rim arcs over the lower half */
        const float bottom = header(q, L, -0.6f), span = PI / (float)n;
        for (int i = 0; i < n; i++) {
            const float a1 = 1.5f * PI - (float)i * span - GAP, a0 = 1.5f * PI - (float)(i + 1) * span + GAP, am = (a0 + a1) * 0.5f;
            const float lr = n <= 2 ? 0.56f : 0.64f, lx = lr * sinf(am), ly = fmaxf(bottom + 0.08f, -lr * cosf(am));
            const float w = n <= 2 ? chord(ly, 0.42f) : 2.f * lr * sinf(span * 0.5f) * HALF * 0.86f;
            int len = (int)strlen(q->label[i]); if (len > lmax) len = lmax;
            const uint32_t c = q->rgb[i] & 0xFFFFFFu;
            item(L, q->label[i], len, lx, ly, fmaxf(FLUID_ASK_PX_MIN, fit_px(len, w, 2.8f)), c, FLUID_ASK_LABEL, i);
            L->tg[L->ntargets++] = (fluid_ask_target_t){ FLUID_ASK_ARC, (int8_t)i, ARC_R, a0, a1, 0.9f * sinf(am), -0.9f * cosf(am), c };
        }
    } else {                                              /* underlined rows, a faint ring round the rim */
        const float top = header(q, L, -0.64f) + 0.04f, rowh = fminf(0.26f, (0.84f - top) / (float)n);
        L->hit.top = top; L->hit.rowh = rowh;
        L->tg[L->ntargets++] = (fluid_ask_target_t){ FLUID_ASK_RING, -1, FRAME_R, 0.f, 0.f, 0.f, 0.f, ICE_RGB };
        for (int i = 0; i < n; i++) {
            int len = (int)strlen(q->label[i]); if (len > lmax) len = lmax;
            const float y = top + rowh * ((float)i + 0.42f), px = fmaxf(FLUID_ASK_PX_MIN, fit_px(len, chord(y, 0.7f), n <= 2 ? 2.8f : 2.4f));
            const float w = text_w(len) * px / HALF / 2.f + 0.07f, ly = y + 7.f * px / HALF / 2.f + 0.025f;
            const uint32_t c = q->rgb[i] & 0xFFFFFFu;
            item(L, q->label[i], len, 0.f, y, px, c, FLUID_ASK_LABEL, i);
            L->tg[L->ntargets++] = (fluid_ask_target_t){ FLUID_ASK_HLINE, (int8_t)i, ly, -w, w, 0.f, ly, c };
        }
    }
}

fluid_ask_hit_t fluid_ask_hit(const fluid_ask_q_t *q) {
    fluid_ask_layout_t L;
    fluid_ask_lay(q, &L);
    return L.hit;
}

int fluid_ask_pick(const fluid_ask_hit_t *L, float X, float Y) {
    const int n = L->n < 1 ? 1 : L->n;
    if (L->rows) {
        if (Y < L->top || fabsf(X) > 0.85f || L->rowh <= 0.f) return -1;
        const int i = (int)floorf((Y - L->top) / L->rowh);
        return i >= 0 && i < n ? i : -1;
    }
    if (hypotf(X, Y) < 0.2f) return -1;
    float a = atan2f(X, -Y);                              /* clockwise from 12 */
    if (a < 0.f) a += 2.f * PI;
    if (a < 0.5f * PI || a > 1.5f * PI) return -1;        /* the upper half holds the question */
    return (int)clampf(floorf((1.5f * PI - a) / (PI / (float)n)), 0.f, (float)(n - 1));
}

int fluid_ask_follow(fluid_ask_screen_t *s, bool active, unsigned seq, int pick, int n) {
    int did = 0;
    if (active && seq != s->seq) {                        /* a new question replaces any */
        s->phase = FLUID_ASK_SHOW; s->seq = seq; s->pick = -1; s->t = s->out_t = 0.f; s->whole = false;
        did |= FLUID_ASK_DID_SHOW;
    }
    if (s->phase == FLUID_ASK_SHOW && pick >= 0 && seq == s->seq) {   /* answered (maybe before it was drawn) */
        s->phase = FLUID_ASK_PICK; s->pick = pick < (n < 1 ? 1 : n) ? pick : 0; s->out_t = 0.f;
        did |= FLUID_ASK_DID_PICK;
    } else if (s->phase == FLUID_ASK_SHOW && (!active || seq != s->seq)) {   /* ended unanswered */
        s->phase = FLUID_ASK_OUT; s->out_t = 0.f;
        did |= FLUID_ASK_DID_LEAVE;
    }
    if ((s->phase == FLUID_ASK_PICK || s->phase == FLUID_ASK_OUT) && (s->whole ? s->out_t > 0.f : s->out_t >= FLUID_ASK_GONE_S)) {
        s->phase = FLUID_ASK_NONE;
        did |= FLUID_ASK_DID_GONE;
    }
    return did;
}

unsigned fluid_ask_tappable(const fluid_ask_screen_t *s, float speed) {
    return s->phase == FLUID_ASK_SHOW && (s->whole || s->t * speed >= FLUID_ASK_READY) ? s->seq : 0u;
}
