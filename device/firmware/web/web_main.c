/*
 * SPDX-License-Identifier: MIT
 *
 * web_main.c: the native player of web_molty.c, for the fidelity check (tools/web-molty/fidelity.mjs runs the
 * WebAssembly build through the same script and wants the same pixels).
 *
 *   web_molty <script> [dump dir]
 *
 * The script, a line a step, runs top to bottom (# starts a comment):
 *   creature <id> <file>            the creature package (before start: the one a start shows)
 *   package <file>                  a scenes package, for scene
 *   start <px> <seed> <boot 0|1>
 *   frames <n> <dt us>              n frames; each prints its number and the FNV-1a hash of the panel's pixels
 *   touch <down 0|1> <x> <y>
 *   shake
 *   state <json>                    POST /api/creature/state's body
 *   listen <0|1>      level <0..1>
 *   ask <json>        ask_end
 *   scene <layout> <ttl s> [params json]      scene_clear
 * With a dump dir every frame is also written there as <number>.ppm.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int   wm_creature(const char *id, const char *json, size_t len);
int   wm_package(const char *json, size_t len);
int   wm_start(int px, uint32_t seed, int boot);
void  wm_frame(uint32_t dt_us);
const uint8_t *wm_pixels(void);
void  wm_touch(int down, int x, int y);
void  wm_shake(void);
int   wm_state(const char *json);
void  wm_listen(int on);
void  wm_listen_level(float level);
int   wm_ask(const char *json);
void  wm_ask_end(void);
int   wm_scene(const char *layout, const char *params, float ttl_s);
void  wm_scene_clear(void);

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "web_molty: cannot read %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    b[n] = 0; *len = (size_t)n;
    fclose(f);
    return b;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: web_molty <script> [dump dir]\n"); return 2; }
    size_t slen;
    char *script = slurp(argv[1], &slen), *save = NULL;
    int px = 0, frame = 0;
    for (char *line = strtok_r(script, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char op[16] = "", a[64] = "", b[512] = "";
        int n = 0;
        if (sscanf(line, "%15s %n", op, &n) < 1 || op[0] == '#') continue;
        const char *rest = line + n;
        int rc = 0;
        if (!strcmp(op, "creature") && sscanf(rest, "%63s %511s", a, b) == 2) { size_t l; char *j = slurp(b, &l); rc = wm_creature(a, j, l); free(j); }
        else if (!strcmp(op, "package") && sscanf(rest, "%511s", b) == 1) { size_t l; char *j = slurp(b, &l); rc = wm_package(j, l); free(j); }
        else if (!strcmp(op, "start")) { unsigned seed; int boot; if (sscanf(rest, "%d %u %d", &px, &seed, &boot) != 3) rc = -1; else rc = wm_start(px, seed, boot); }
        else if (!strcmp(op, "frames")) {
            int k; unsigned dt;
            if (sscanf(rest, "%d %u", &k, &dt) != 2) rc = -1;
            for (int i = 0; !rc && i < k; i++, frame++) {
                wm_frame(dt);
                const uint8_t *p = wm_pixels();
                uint64_t h = 0xcbf29ce484222325ull;
                for (size_t q = 0; q < (size_t)px * (size_t)px * 4; q++) { h ^= p[q]; h *= 0x100000001b3ull; }
                printf("%d %016llx\n", frame, (unsigned long long)h);
                if (argc > 2) {
                    char path[600];
                    snprintf(path, sizeof(path), "%s/%05d.ppm", argv[2], frame);
                    FILE *f = fopen(path, "wb");
                    if (!f) return 2;
                    fprintf(f, "P6\n%d %d\n255\n", px, px);
                    for (int q = 0; q < px * px; q++) fwrite(p + q * 4, 1, 3, f);
                    fclose(f);
                }
            }
        }
        else if (!strcmp(op, "touch")) { int d, x, y; if (sscanf(rest, "%d %d %d", &d, &x, &y) != 3) rc = -1; else wm_touch(d, x, y); }
        else if (!strcmp(op, "shake")) wm_shake();
        else if (!strcmp(op, "state")) rc = wm_state(rest);
        else if (!strcmp(op, "listen")) wm_listen(atoi(rest));
        else if (!strcmp(op, "level")) wm_listen_level((float)atof(rest));
        else if (!strcmp(op, "ask")) rc = wm_ask(rest);
        else if (!strcmp(op, "ask_end")) wm_ask_end();
        else if (!strcmp(op, "scene")) { float ttl; int m = 0; if (sscanf(rest, "%63s %f %n", a, &ttl, &m) < 2) rc = -1; else rc = wm_scene(a, rest[m] ? rest + m : NULL, ttl); }
        else if (!strcmp(op, "scene_clear")) wm_scene_clear();
        else rc = -1;
        if (rc) { fprintf(stderr, "web_molty: step failed: %s\n", line); return 1; }
    }
    return 0;
}
