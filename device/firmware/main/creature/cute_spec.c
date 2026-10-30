/*
 * cute_spec.c — .molty parser/validator. Mirrors encode() in
 * device/creatures/tools/molty-export.mjs field by field.
 *
 * Target board: Waveshare ESP32-S3-Touch-LCD-1.46 (412x412 round LCD)
 * FQBN: esp32:esp32:esp32s3:FlashSize=16M,PSRAM=enabled,CDCOnBoot=cdc
 */
#include "cute_spec.h"

#include <string.h>

const char *const CUTE_MOOD_NAMES[CUTE_N_MOODS] = {
    "idle", "listening", "thinking", "speaking", "happy", "sleepy", "surprised",
};

uint32_t cute_crc32(const uint8_t *buf, size_t len) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < len; i++) {
        c ^= buf[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
    }
    return c ^ 0xffffffffu;
}

typedef struct { const uint8_t *b; size_t n, o; bool bad; } rd_t;

static uint8_t u8(rd_t *r) { if (r->o + 1 > r->n) { r->bad = true; return 0; } return r->b[r->o++]; }
static uint16_t u16(rd_t *r) { uint16_t lo = u8(r); return (uint16_t)(lo | (u8(r) << 8)); }
static float q(rd_t *r) { return (int16_t)u16(r) * (1.f / 4096.f); }
static float q8(rd_t *r) { return (int16_t)u16(r) * (1.f / 256.f); }
static void q3(rd_t *r, float *v) { v[0] = q(r); v[1] = q(r); v[2] = q(r); }
static void str(rd_t *r, char *dst, size_t cap) {
    int n = u8(r);
    for (int i = 0; i < n; i++) { char c = (char)u8(r); if ((size_t)i < cap - 1) dst[i] = c; }
    dst[(size_t)n < cap - 1 ? (size_t)n : cap - 1] = '\0';
}

#define FAIL(code, msg) do { if (err) *err = (msg); return (code); } while (0)

int cute_spec_parse(const uint8_t *buf, size_t len, cute_spec_t *s, const char **err) {
    if (err) *err = "ok";
    if (!buf || !s) FAIL(-1, "null");
    if (len < 12 || len > CUTE_MAX_FILE) FAIL(-2, "size");
    if (memcmp(buf, CUTE_MAGIC, 4) != 0) FAIL(-3, "magic");
    if (buf[4] != CUTE_VERSION) FAIL(-4, "version");
    size_t flen = (size_t)buf[6] | ((size_t)buf[7] << 8);
    if (flen != len) FAIL(-5, "length");
    uint32_t want = (uint32_t)buf[len - 4] | (uint32_t)buf[len - 3] << 8 | (uint32_t)buf[len - 2] << 16 | (uint32_t)buf[len - 1] << 24;
    if (cute_crc32(buf, len - 4) != want) FAIL(-6, "crc");

    memset(s, 0, sizeof(*s));
    rd_t R = { buf, len - 4, 8, false }, *r = &R;
    uint8_t flags = buf[5];
    s->has_body = flags & 1; s->has_cheeks = (flags >> 1) & 1; s->eye_shine = (flags >> 2) & 1;
    str(r, s->id, sizeof(s->id)); str(r, s->name, sizeof(s->name));
    s->npal = u8(r);
    if (s->npal > CUTE_MAX_PALETTE) FAIL(-7, "palette");
    for (int i = 0; i < s->npal; i++) {
        str(r, s->pal_key[i], sizeof(s->pal_key[i]));
        s->pal_rgb[i][0] = u8(r); s->pal_rgb[i][1] = u8(r); s->pal_rgb[i][2] = u8(r);
    }
    q3(r, s->head_size); q3(r, s->head_at); q3(r, s->head_pivot); s->head_squash = q(r); s->head_pitch = q(r);
    if (s->has_body) { q3(r, s->body_size); q3(r, s->body_at); }
    q3(r, s->face_r); q3(r, s->face_off);
    s->eye_x = q(r); s->eye_y = q(r); s->eye_r = q(r); s->eye_shape = u8(r); s->eye_tilt = q(r); s->eye_color = u8(r);
    s->mouth_type = u8(r); s->mouth_y = q(r); s->mouth_w = q(r); s->mouth_open = q(r);
    s->cheek_x = q(r); s->cheek_y = q(r); s->cheek_r = q(r);
    s->col_shine = u8(r); s->col_white = u8(r); s->col_mouth = u8(r); s->col_tongue = u8(r); s->col_cheek = u8(r);
    s->motion = u8(r); s->motion_amp = q(r); s->motion_rate = q(r); s->motion_height = q(r);
    s->limb_side = q(r); s->limb_ear = q(r); s->limb_tail = q(r); s->limb_foot = q(r); s->limb_antenna = q(r);
    int nsock = u8(r);
    if (nsock != CUTE_N_SOCKETS) FAIL(-8, "sockets");
    for (int i = 0; i < nsock; i++) { s->sockets[i].on = u8(r); q3(r, s->sockets[i].pos); q3(r, s->sockets[i].rot); }
    s->neck_ring[0] = q(r); s->neck_ring[1] = q(r);
    s->item_head = u8(r); s->item_neck = u8(r);
    s->nnames = u8(r);
    if (s->nnames > CUTE_MAX_NAMES) FAIL(-9, "names");
    for (int i = 0; i < s->nnames; i++) str(r, s->names[i], sizeof(s->names[i]));
    s->nparts = u8(r);
    if (s->nparts > CUTE_MAX_PARTS) FAIL(-10, "parts");
    for (int i = 0; i < s->nparts; i++) {
        cute_part_t *p = &s->parts[i];
        p->type = u8(r); p->socket = u8(r); p->color = u8(r); p->name = u8(r);
        uint8_t pf = u8(r);
        p->mirror = pf & 1; p->hidden = (pf >> 1) & 1; p->has_scale = (pf >> 2) & 1; p->has_offset = (pf >> 3) & 1;
        p->rot_order = u8(r); p->seg_a = u8(r); p->seg_b = u8(r);
        q3(r, p->pos); q3(r, p->rot);
        if (p->has_scale) q3(r, p->scale); else p->scale[0] = p->scale[1] = p->scale[2] = 1.f;
        if (p->has_offset) q3(r, p->offset);
        p->np = u8(r);
        if (p->np > CUTE_MAX_PARAMS) FAIL(-11, "params");
        for (int k = 0; k < p->np; k++) p->params[k] = q(r);
        if (p->type > CP_DISC || p->socket >= CUTE_N_SOCKETS || p->color >= s->npal || p->name >= s->nnames || p->rot_order > 5)
            FAIL(-12, "part index");
        if (p->seg_a < 1 || p->seg_b < 1 || p->seg_a > 32 || p->seg_b > 32) FAIL(-13, "part segments");
    }
    s->nbehav = u8(r);
    if (s->nbehav > CUTE_MAX_BEHAV) FAIL(-14, "behaviours");
    for (int i = 0; i < s->nbehav; i++) {
        cute_behav_t *b = &s->behav[i];
        b->kind = u8(r); b->index = u8(r); b->channel = u8(r); b->op = u8(r); b->nterms = u8(r);
        if (b->nterms > CUTE_MAX_TERMS || b->kind > CB_BODY || b->channel > CC_LID || b->op > CO_SET) FAIL(-15, "behaviour");
        if ((b->kind == CB_PART && b->index >= s->nnames) || (b->kind == CB_SOCKET && b->index >= CUTE_N_SOCKETS)) FAIL(-16, "behaviour target");
        for (int t = 0; t < b->nterms; t++) {
            cute_term_t *T = &b->terms[t];
            T->driver = u8(r); T->flags = u8(r);
            T->amp = q(r); T->freq = q8(r); T->phase = q(r); T->ease = q8(r); T->p0 = q8(r); T->p1 = q8(r);
            for (int k = 0; k < 8; k++) T->gain[k] = q(r);
            if (T->flags & CT_GAIN_B) for (int k = 0; k < 8; k++) T->gain_b[k] = q(r);
            if (T->driver > CD_SPIN) FAIL(-17, "driver");
        }
    }
    if (s->eye_color >= s->npal || s->col_shine >= s->npal || s->col_white >= s->npal || s->col_mouth >= s->npal ||
        s->col_tongue >= s->npal || s->col_cheek >= s->npal) FAIL(-18, "face colour");
    if (r->bad) FAIL(-19, "truncated");
    if (r->o != r->n) FAIL(-20, "trailing bytes");
    return 0;
}
