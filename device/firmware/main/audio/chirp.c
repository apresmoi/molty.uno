/* SPDX-License-Identifier: MIT — see chirp.h */
#include "chirp.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct { float hz, start_ms, end_ms, tau_ms, release_ms, gain; } note_t;
static const note_t NOTES[2] = {
    {  784.f,  0.f,  65.f, 25.f,  8.f, 0.80f },   /* G5 */
    { 1175.f, 50.f, 160.f, 45.f, 20.f, 1.00f },   /* D6, a fifth up */
};
#define ATTACK_MS 6.f

static float note_at(const note_t *n, float t_ms)
{
    if (t_ms < n->start_ms || t_ms >= n->end_ms) return 0.f;
    const float u = t_ms - n->start_ms, left = n->end_ms - t_ms;
    float env = expf(-u / n->tau_ms);
    if (u < ATTACK_MS) env *= 0.5f - 0.5f * cosf((float)M_PI * u / ATTACK_MS);
    if (left < n->release_ms) env *= 0.5f - 0.5f * cosf((float)M_PI * left / n->release_ms);
    return n->gain * env * sinf(2.f * (float)M_PI * n->hz * u / 1000.f);
}

size_t chirp_fill(int16_t *out, size_t from, size_t n)
{
    if (!out || from >= CHIRP_SAMPLES) return 0;
    if (n > CHIRP_SAMPLES - from) n = CHIRP_SAMPLES - from;
    for (size_t i = 0; i < n; i++) {
        const float t_ms = (float)(from + i) * 1000.f / (float)CHIRP_RATE;
        float v = note_at(&NOTES[0], t_ms) + note_at(&NOTES[1], t_ms);
        v = fmaxf(-1.f, fminf(1.f, v));
        out[i] = (int16_t)lrintf(v * (float)CHIRP_PEAK);
    }
    return n;
}
