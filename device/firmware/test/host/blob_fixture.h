/*
 * SPDX-License-Identifier: MIT
 *
 * blob_fixture.h: a breathing metaball for the sim's host tests (fluid_test.c, fluid_golden_test.c): a smooth moving
 * shape to step, draw and hash. It was the firmware's "no creature yet" stand-in (fluid_target.c) until the boot word
 * took that place; the tests keep it as their shape, so their numbers (and fluid_golden.txt's blob line) stay as
 * they were.
 */
#pragma once
#include "fluid_sim.h"
#include <math.h>

#define BLOB_RGB(hex) { ((hex) >> 16 & 255) / 255.f, ((hex) >> 8 & 255) / 255.f, ((hex) & 255) / 255.f }
/* A breathing metaball: 4 orbiting radial sources summed, iso-surfaced with a soft
 * edge, textured inside by a field-synth patch (radial sin × noise, "Lava"-like). */
static const fluid_patch_t BLOB_PATCH = { 1.2f, 0.55f, 2, {
    { FV_NOISE, FW_SIN, 2.6f, 0.45f, 1.f },
    { FV_RADIAL, FW_SIN, 1.6f, 0.35f, 0.6f },
} };
static const float BLOB_A[3] = BLOB_RGB(0xffb3c7), BLOB_B[3] = BLOB_RGB(0xb9a4ff);

static void fluid_target_blob(fluid_sim_t *s, float t, float swell, float ox, float oy) {
    float C[4][3];
    for (int q = 0; q < 4; q++) {
        float w = 0.35f + q * 0.17f, ph = q * 1.7f, r = (0.19f + 0.025f * q) * swell;
        C[q][0] = 0.2f * cosf(t * w + ph) + ox;
        C[q][1] = 0.17f * sinf(t * w * 1.3f + ph) + oy;
        C[q][2] = r * r;
    }
    for (int j = 0; j < s->H; j++) for (int i = 0; i < s->W; i++) {
        const int k = j * s->W + i;
        if (!s->mask[k]) { s->T[k] = 0; continue; }
        float x = s->xs[i], y = s->ys[j], F = 0.f;
        for (int q = 0; q < 4; q++) {
            float dx = x - C[q][0], dy = y - C[q][1];
            F += C[q][2] / (dx * dx + dy * dy + 1e-4f);
        }
        float edge = fluid_smooth(0.85f, 1.6f, F);
        float tex = edge > 0.f ? fluid_patch(&BLOB_PATCH, x - ox, y - oy, t) : 0.f;
        s->T[k] = fq(edge * (0.5f + 0.5f * tex), FQ_UNIT);
        float m = fluid_clamp01(tex);
        s->tcol[k] = fluid_pack_rgba8(BLOB_A[0] + (BLOB_B[0] - BLOB_A[0]) * m, BLOB_A[1] + (BLOB_B[1] - BLOB_A[1]) * m,
                                      BLOB_A[2] + (BLOB_B[2] - BLOB_A[2]) * m, 0.f);
    }
}
