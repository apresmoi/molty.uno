/*
 * cute_spec.h — parsed .molty creature (the cute kit's defineCreature spec, resolved).
 *
 * Binary format: device/creatures/MOLTY-FORMAT.md (writer: device/creatures/tools/
 * molty-export.mjs). The file is fully resolved by the exporter — kit defaults applied,
 * sockets placed — so this is a flat data structure the runtime (cute_rig.c) reads.
 * No ESP-IDF dependencies (host-testable).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CUTE_MAGIC        "MLTY"
#define CUTE_VERSION      1
#define CUTE_MAX_FILE     16384
#define CUTE_MAX_PALETTE  48
#define CUTE_MAX_NAMES    48
#define CUTE_MAX_PARTS    48
#define CUTE_MAX_PARAMS   6
#define CUTE_MAX_BEHAV    32
#define CUTE_MAX_TERMS    4
#define CUTE_N_MOODS      7
#define CUTE_N_SOCKETS    13

enum { CM_IDLE, CM_LISTENING, CM_THINKING, CM_SPEAKING, CM_HAPPY, CM_SLEEPY, CM_SURPRISED };
extern const char *const CUTE_MOOD_NAMES[CUTE_N_MOODS];

/* SOCKETS order of kit.js */
enum { CS_ROOT, CS_BODY, CS_HEAD, CS_TOP, CS_NECK, CS_BACK, CS_TAIL, CS_JAW, CS_EAR, CS_SIDE, CS_FOOT, CS_ANTENNA, CS_HORN };
enum { CON_ROOT, CON_BODY, CON_HEAD };
enum { CP_ELLIPSOID, CP_TEARDROP, CP_CAPSULE, CP_CONE, CP_CYLINDER, CP_TORUS, CP_BOX, CP_DISC };
enum { CMO_NONE, CMO_WADDLE, CMO_HOP, CMO_SLITHER, CMO_FLOAT, CMO_FLUTTER, CMO_WIGGLE };
enum { CE_ROUND, CE_TALL, CE_WIDE };
enum { CMT_SMILE, CMT_CAT, CMT_JAW, CMT_NONE };
enum { CB_PART, CB_SOCKET, CB_ROOT, CB_HEAD, CB_EYES, CB_BODY };
enum { CC_RX, CC_RY, CC_RZ, CC_PX, CC_PY, CC_PZ, CC_S, CC_SX, CC_SY, CC_SZ, CC_SXZ, CC_LID };
enum { CO_ADD, CO_SET };
enum { CD_CONST, CD_SIN, CD_ABSSIN, CD_POSSIN, CD_SQUARE, CD_MOUTH, CD_LAGX, CD_LAGZ, CD_PULSE, CD_SPIN };
#define CT_SIDE_SIGN  1
#define CT_SIDE_PHASE 2
#define CT_EASE       4
#define CT_GAIN_B     8

typedef struct { uint8_t on; float pos[3], rot[3]; } cute_socket_t;

typedef struct {
    uint8_t type, socket, color, name, rot_order, seg_a, seg_b, np;
    bool mirror, hidden, has_scale, has_offset;
    float pos[3], rot[3], scale[3], offset[3];
    float params[CUTE_MAX_PARAMS];
} cute_part_t;

typedef struct {
    uint8_t driver, flags;
    float amp, freq, phase, ease, p0, p1;
    float gain[8], gain_b[8];           /* k0 + 7 mood coefficients */
} cute_term_t;

typedef struct {
    uint8_t kind, index, channel, op, nterms;
    cute_term_t terms[CUTE_MAX_TERMS];
} cute_behav_t;

typedef struct {
    char id[40], name[40];
    int  npal;
    char pal_key[CUTE_MAX_PALETTE][16];
    uint8_t pal_rgb[CUTE_MAX_PALETTE][3];
    float head_size[3], head_at[3], head_pivot[3], head_squash, head_pitch;
    bool  has_body;
    float body_size[3], body_at[3];
    float face_r[3], face_off[3];
    float eye_x, eye_y, eye_r, eye_tilt; uint8_t eye_shape, eye_color; bool eye_shine;
    uint8_t mouth_type; float mouth_y, mouth_w, mouth_open;
    bool  has_cheeks; float cheek_x, cheek_y, cheek_r;
    uint8_t col_shine, col_white, col_mouth, col_tongue, col_cheek;
    uint8_t motion; float motion_amp, motion_rate, motion_height;
    float limb_side, limb_ear, limb_tail, limb_foot, limb_antenna;
    cute_socket_t sockets[CUTE_N_SOCKETS];
    float neck_ring[2];
    uint8_t item_head, item_neck;
    int  nnames;
    char names[CUTE_MAX_NAMES][16];
    int  nparts;
    cute_part_t parts[CUTE_MAX_PARTS];
    int  nbehav;
    cute_behav_t behav[CUTE_MAX_BEHAV];
} cute_spec_t;

/**
 * Parse + validate a .molty buffer into *out. Returns 0 on success, otherwise a
 * negative code and a short reason in *err (static string): bad magic/version,
 * length mismatch, CRC mismatch, truncated, or out-of-range index.
 */
int cute_spec_parse(const uint8_t *buf, size_t len, cute_spec_t *out, const char **err);
uint32_t cute_crc32(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
