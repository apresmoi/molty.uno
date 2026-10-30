/*
 * fluid_golden_test.c — the fluid's pixels, pinned.
 *
 * Renders deterministic scenarios (every state, palette and edge mode, three grids,
 * the speed-reader letters) into a 412×412 framebuffer and folds every frame (pixels,
 * dirty band, cell count) into one FNV-1a hash per scenario. The hashes are compared
 * with fluid_golden.txt, captured before the board layer made the panel size a runtime
 * value: a 412×412 panel must render bit-for-bit what it always did.
 *
 * The build disables multiply-add fusion (-ffp-contract=off), so the hashes depend only
 * on the source and libm: they matched on darwin-arm64 (clang 21) and linux-x86_64
 * (gcc 13, glibc 2.39 — the CI image) when captured. If a new platform's libm rounds
 * expf/powf/sinf differently, every scenario fails at once; a single scenario failing
 * is a real change in what the panel shows. Re-capture (fluid_golden_build.sh --print)
 * only from a commit whose output is known-good, and say why in the commit.
 *
 * Build + run: test/host/fluid_golden_build.sh [--print]
 */
#include "fluid_sim.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PANEL 412

static int s_fail = 0, s_pass = 0;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* a real-looking 5×7 font: every glyph differs, so a letter/ramp mix-up changes pixels */
static const uint8_t *hash_font(char c) {
    static uint8_t g[256][7];
    static int init = 0;
    if (!init) {
        for (int k = 0; k < 256; k++) for (int r = 0; r < 7; r++)
            g[k][r] = (uint8_t)(((unsigned)k * 2654435761u >> (r * 3)) & 0x1f);
        memset(g[' '], 0, 7);
        init = 1;
    }
    return g[(unsigned char)c];
}

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

enum { SC_BLOB, SC_TEXT, SC_RIPPLE, SC_VORTEX, SC_WAVE, SC_LABEL };

typedef struct {
    const char *name;
    int kind, W, H, palette, edge;
    bool strokes;
} scenario_t;

static const scenario_t SCEN[] = {
    { "blob-52x32-pastel-all",       SC_BLOB,   52, 32, FLUID_PAL_PASTEL, FLUID_EDGE_ALL,      false },
    { "text-52x32-pastel-all",       SC_TEXT,   52, 32, FLUID_PAL_PASTEL, FLUID_EDGE_ALL,      false },
    { "ripple-44x27-amber-contours", SC_RIPPLE, 44, 27, FLUID_PAL_AMBER,  FLUID_EDGE_CONTOURS, false },
    { "vortex-60x37-green-ascii",    SC_VORTEX, 60, 37, FLUID_PAL_GREEN,  FLUID_EDGE_OFF,      true  },
    { "wave-52x32-pastel-ascii",     SC_WAVE,   52, 32, FLUID_PAL_PASTEL, FLUID_EDGE_OFF,      true  },
    { "label-52x32-green-all",       SC_LABEL,  52, 32, FLUID_PAL_GREEN,  FLUID_EDGE_ALL,      false },
};
#define N_SCEN ((int)(sizeof(SCEN) / sizeof(SCEN[0])))

static uint64_t run(const scenario_t *sc, long *lit_out) {
    fluid_sim_t *s = fluid_sim_create(sc->W, sc->H, FLUID_STAGE_REF);
    fluid_text_t *tx = s ? fluid_text_create(s) : NULL;
    fluid_draw_t *d = fluid_draw_create(sc->W, sc->H, FLUID_STAGE_REF, FLUID_STAGE_REF);
    fluid_frame_t *fr = fluid_frame_create(sc->W, sc->H);
    uint16_t *fb = calloc((size_t)PANEL * PANEL, 2);
    if (!s || !tx || !d || !fr || !fb) { printf("FAIL alloc %s\n", sc->name); s_fail++; return 0; }
    uint64_t h = 0xcbf29ce484222325ull;

    const bool text = sc->kind == SC_TEXT || sc->kind == SC_WAVE || sc->kind == SC_LABEL;
    if (text) {
        fluid_text_set_sentence(tx, sc->kind == SC_LABEL ? 2.f : 4.5f);
        if (sc->kind == SC_LABEL) fluid_text_set_word(tx, s, "4821", 4, -1);
        else fluid_text_set_word(tx, s, "molty.", 6, 1);
        for (int i = 0; i < tx->nletters; i++) {
            const fluid_letter_t *L = &tx->letters[i];
            float g[5] = { L->x, L->y, L->w, L->h, L->fs };
            h = fnv(h, g, sizeof(g));
        }
        h = fnv(h, tx->cov, (size_t)s->N * sizeof(float));
    }
    int fx = sc->kind == SC_RIPPLE ? FLUID_FX_RIPPLE : sc->kind == SC_VORTEX ? FLUID_FX_VORTEX : FLUID_FX_WAVE;
    float fxs = 0.f;
    const bool effect = sc->kind == SC_RIPPLE || sc->kind == SC_VORTEX || sc->kind == SC_WAVE;
    if (effect) fluid_effect_enter(s, fx);
    s->p.tilt_x = 4.f; s->p.tilt_y = -2.f;
    s->vibe.amp = 0.6f; s->vibe.freq = 9.7f;
    fluid_sim_impulse(s, 0.2f, -0.1f, 1.6f, 0.09f);
    fluid_sim_push(s, -0.3f, 0.2f, 40.f, -20.f, 0.16f);

    fluid_draw_opts_t o = { sc->palette, sc->edge, 0.12f, 0.f, 0.f, NULL, 0, " .,:;!+=*xX#@", hash_font };
    const float dt = 1.f / 60.f;
    for (int f = 0; f < 72; f++) {
        float t = f / 30.f, breath = 0.5f - 0.5f * cosf(t / 4.f * 6.2832f), swell = 1.f + 0.05f * (breath * 2.f - 1.f);
        s->p.hold = fminf(1.f, t / 1.5f);
        s->p.tint_rate = 3.f + 9.f * s->p.hold;
        switch (sc->kind) {
        case SC_BLOB:   fluid_target_blob(s, t, swell, 0.02f, -0.01f); break;
        case SC_TEXT:
        case SC_LABEL:  fluid_text_fill(tx, s, t); break;
        default:        fluid_target_effect(s, fx, t, swell, 0.f, 0.f); fluid_effect_tick(s, fx, 1.f / 30.f, &fxs); break;
        }
        fluid_sim_background(s, t, breath, (f & 1) == 0 || f < 2);
        if ((f & 1) == 1 || f < 2) fluid_sim_swirl_field(s);
        fluid_sim_step(s, dt); fluid_sim_step(s, dt);
        fluid_sim_color(s, 2.f * dt);
        fluid_sim_view(s);
        if (f % 24 == 12) fluid_sim_impulse(s, -0.2f, 0.15f, 1.2f, 0.1f);
        fluid_sim_snapshot(s, fr, sc->strokes);
        o.time = t;
        o.shimmer = 0.12f + (f > 36 ? 0.35f : 0.f);
        /* the wave scenario melts its letters away from frame 40 (mole_fluid's s_melt) */
        o.melt = sc->kind == SC_WAVE && f >= 40 ? (f - 40) / 30.f : 0.f;
        o.letters = text ? tx->letters : NULL;
        o.nletters = text && o.melt < 0.8f ? tx->nletters : 0;
        if (f == 50) { memset(fb, 0, (size_t)PANEL * PANEL * 2); d->force = true; }   /* full repaint */
        int y0 = -1, y1 = -1;
        int n = fluid_draw(d, fr, fb, &o, &y0, &y1);
        int32_t meta[3] = { n, y0, y1 };
        h = fnv(h, meta, sizeof(meta));
        h = fnv(h, fb, (size_t)PANEL * PANEL * 2);
    }
    long lit = 0;
    for (int p = 0; p < PANEL * PANEL; p++) lit += fb[p] != 0;
    *lit_out = lit;
    free(fb); fluid_frame_destroy(fr); fluid_draw_destroy(d); fluid_text_destroy(tx); fluid_sim_destroy(s);
    return h;
}

/* golden lines: "<scenario> <hash>" */
static int golden_lookup(const char *path, const char *name, uint64_t *out) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256], sc[96];
    unsigned long long v;
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || sscanf(line, "%95s %llx", sc, &v) != 2) continue;
        if (!strcmp(sc, name)) { *out = v; found = 1; }
    }
    fclose(f);
    return found;
}

int main(int argc, char **argv) {
    const char *golden = NULL;
    int print = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--print")) print = 1;
        else golden = argv[i];
    }
    if (!print) printf("fluid golden test (412x412)\n");
    for (int i = 0; i < N_SCEN; i++) {
        long lit = 0;
        uint64_t h = run(&SCEN[i], &lit);
        if (print) { printf("%s 0x%016" PRIx64 "\n", SCEN[i].name, h); continue; }
        CHECK(lit > 2000, "%s: only %ld px lit in the last frame", SCEN[i].name, lit);
        uint64_t want = 0;
        if (!golden || !golden_lookup(golden, SCEN[i].name, &want)) {
            CHECK(0, "%s: no golden (hash 0x%016" PRIx64 ")", SCEN[i].name, h);
            continue;
        }
        CHECK(h == want, "%s: hash 0x%016" PRIx64 " != golden 0x%016" PRIx64 " (412 output changed)",
              SCEN[i].name, h, want);
        if (h == want) printf("  %-30s 0x%016" PRIx64 "  ok\n", SCEN[i].name, h);
    }
    if (print) return 0;
    printf("%d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
