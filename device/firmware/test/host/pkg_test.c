/*
 * SPDX-License-Identifier: MIT
 *
 * pkg_test.c — host test for main/pkg: the strict parser (pkg_parse.c) and the store
 * (pkg_store.c) against a temporary directory and a fake download.
 *
 * Checks: the sample package (fixtures/shapes.json) parses with its assets, layouts, params
 * and bindings; malformed, oversized, unknown-key, unknown-kind, unresolved-asset and
 * bad-binding files are refused; pkg.install's arguments (the URL allowlist above all);
 * install verifies the size and SHA-256 before anything is written, keeps the quota, refuses
 * a file that is not the package asked for, survives a "reboot" (a fresh read of the index),
 * replaces an older version, and remove / a vanished file mean not installed.
 *
 * Build + run: test/host/pkg_build.sh
 */
#define _POSIX_C_SOURCE 200809L
#include "pkg_parse.h"
#include "pkg_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int s_fail = 0, s_pass = 0;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ── SHA-256 (FIPS 180-4), for the fake download's digest ──────────────────── */
typedef struct { uint32_t h[8]; uint8_t buf[64]; uint64_t len; } sha_t;
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void sha_block(sha_t *s, const uint8_t *p) {
    uint32_t w[64], a[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ w[i - 15] >> 3, s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ w[i - 2] >> 10;
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(a, s->h, sizeof(a));
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = a[7] + (ROR(a[4], 6) ^ ROR(a[4], 11) ^ ROR(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + K[i] + w[i];
        uint32_t t2 = (ROR(a[0], 2) ^ ROR(a[0], 13) ^ ROR(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
        memmove(a + 1, a, 7 * sizeof(uint32_t));
        a[4] += t1; a[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) s->h[i] += a[i];
}
static void sha_init(sha_t *s) {
    static const uint32_t H[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, H, sizeof(H)); s->len = 0;
}
static void sha_update(sha_t *s, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) { s->buf[s->len++ % 64] = p[i]; if (s->len % 64 == 0) sha_block(s, s->buf); }
}
static void sha_final(sha_t *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8; uint8_t one = 0x80, zero = 0;
    sha_update(s, &one, 1);
    while (s->len % 64 != 56) sha_update(s, &zero, 1);
    for (int i = 7; i >= 0; i--) { uint8_t b = (uint8_t)(bits >> (8 * i)); sha_update(s, &b, 1); }
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));
}
static void sha_hex(const uint8_t *p, size_t n, char hex[65]) {
    sha_t s; uint8_t d[32]; sha_init(&s); sha_update(&s, p, n); sha_final(&s, d);
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d[i]);
}

/* ── the fake download: serves `body`, counts what was asked ───────────────── */
typedef struct { const char *body; size_t len, off; int64_t cl; const char *open_err; int opens; sha_t sha; } fake_t;
static const char *f_open(void *c, const char *url, int64_t *len) { fake_t *f = c; (void)url; f->opens++; f->off = 0; *len = f->cl; return f->open_err; }
static int f_read(void *c, uint8_t *buf, int cap) {
    fake_t *f = c; size_t n = f->len - f->off; if (n > 700) n = 700; if ((int)n > cap) n = (size_t)cap;   /* short reads */
    memcpy(buf, f->body + f->off, n); f->off += n; return (int)n;
}
static void f_close(void *c) { (void)c; }
static void f_sha_start(void *c) { sha_init(&((fake_t *)c)->sha); }
static void f_sha_update(void *c, const uint8_t *b, size_t n) { sha_update(&((fake_t *)c)->sha, b, n); }
static void f_sha_finish(void *c, uint8_t out[32]) { sha_final(&((fake_t *)c)->sha, out); }

static char *read_file(const char *p, size_t *len) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); *len = fread(b, 1, (size_t)n, f); b[*len] = 0; fclose(f);
    return b;
}

static char *s_fix; static size_t s_fix_len;
#define IS(e, code) ((e) && !strcmp((e), (code)))

/* the fixture with one substring replaced */
static const char *parse_with(const char *from, const char *to) {
    const char *at = strstr(s_fix, from);
    if (!at) { printf("fixture has no '%s'\n", from); s_fail++; return "?"; }
    size_t a = (size_t)(at - s_fix), fl = strlen(from), tl = strlen(to);
    char *b = malloc(s_fix_len - fl + tl + 1);
    memcpy(b, s_fix, a); memcpy(b + a, to, tl); memcpy(b + a + tl, at + fl, s_fix_len - a - fl + 1);
    const char *err;
    pkg_t *p = pkg_parse(b, strlen(b), &err);
    free(b);
    if (p) { pkg_unref(p); return NULL; }
    return err;
}

static void test_parse(void) {
    const char *err;
    pkg_t *p = pkg_parse(s_fix, s_fix_len, &err);
    CHECK(p != NULL, "the sample package parses (%s)", err ? err : "");
    if (!p) return;
    CHECK(!strcmp(p->id, "shapes") && !strcmp(p->name, "Shapes") && !strcmp(p->version, "1.0.0") && p->kind == PKG_SCENES, "header");
    CHECK(p->nassets == 2 && p->assets[0].kind == PA_MASK && p->assets[1].kind == PA_IMAGE && p->assets[1].npal == 3 &&
          p->assets[1].pal[1] == 0xff8040, "assets");
    CHECK(p->assets[0].data[0] == 0x18 && p->assets[0].data[3] == 0xff, "the mask's bits, MSB first");
    CHECK(p->assets[1].data[0] == 0x01 && p->assets[1].data[1] == 0x11 && p->assets[1].data[2] == 0x22, "the image's pixels, high nibble first");
    const pkg_layout_t *l = pkg_layout(p, "dial");
    CHECK(l && l->nitems == 9 && l->nparams == 3 && l->dur == 6.f, "the dial layout");
    if (l) {
        const pkg_item_t *g = &l->items[3];
        CHECK(g->k == PK_GLYPH && g->each == 1 && g->polar && g->f[PF_PA].src == PV_ELEM && !strcmp(g->f[PF_PA].s, "a") &&
              g->f[PF_PR].src == PV_NUM && g->f[PF_ASSET].src == PV_ELEM, "each / polar / @field");
        CHECK(l->items[4].f[PF_LIT].src == PV_PARAM && l->items[4].f[PF_LIT].idx == 2 && l->items[4].f[PF_ASSET].src == PV_ASSET, "$param");
        CHECK(l->items[7].k == PK_CURVE && l->items[7].npts == 3, "curve points");
    }
    CHECK(!pkg_layout(p, "nope"), "an unknown layout");
    pkg_ref(p); pkg_unref(p); pkg_unref(p);

    /* malformed / oversized */
    CHECK(!pkg_parse(s_fix, s_fix_len / 2, &err) && err, "truncated JSON");
    CHECK(!pkg_parse("[]", 2, &err) && err, "not an object");
    char *big = malloc(PKG_FILE_MAX + 2); memset(big, ' ', PKG_FILE_MAX + 1); memcpy(big, s_fix, s_fix_len);
    CHECK(!pkg_parse(big, PKG_FILE_MAX + 1, &err) && !strcmp(err, "package: size"), "oversized");
    free(big);
    CHECK(IS(parse_with("\"molty-pkg/1\"", "\"molty-pkg/2\""), "package: v"), "an unknown format version");
    CHECK(parse_with("\"id\": \"shapes\"", "\"id\": \"Shapes!\""), "a bad id");
    CHECK(parse_with("\"kind\": \"scenes\"", "\"kind\": \"code\""), "an unknown package kind");
    /* unknown anything */
    CHECK(IS(parse_with("\"v\":", "\"run\": 1, \"v\":"), "package: unknown key"), "an unknown top-level key");
    CHECK(IS(parse_with("{\"k\": \"ring\"", "{\"k\": \"star\""), "item: unknown kind"), "an unknown item kind");
    CHECK(IS(parse_with("{\"k\": \"ring\"", "{\"k\": \"ring\", \"eval\": 1"), "item: unknown key"), "an unknown item key");
    CHECK(IS(parse_with("{\"k\": \"ring\"", "{\"k\": \"ring\", \"rot\": 1"), "item: unknown key"), "a key of another kind");
    CHECK(IS(parse_with("\"kind\": \"mask\"", "\"kind\": \"svg\""), "asset: unknown kind"), "an unknown asset kind");
    CHECK(parse_with("\"dur\": 6", "\"dur\": 6, \"script\": \"x\""), "an unknown layout key");
    /* assets */
    CHECK(IS(parse_with("\"img\": \"halves\"", "\"img\": \"nothing\""), "item: unresolved asset"), "an unresolved asset");
    CHECK(IS(parse_with("\"asset\": \"plus\"", "\"asset\": \"pluss\""), "item: unresolved asset"), "an unresolved glyph asset");
    CHECK(IS(parse_with("\"img\": \"halves\"", "\"img\": \"plus\""), "item: img must be an image"), "a mask on a ball");
    CHECK(IS(parse_with("\"w\": 8, \"h\": 8, \"bits\"", "\"w\": 9, \"h\": 8, \"bits\""), "asset: data length"), "bits of the wrong length");
    CHECK(IS(parse_with("\"#4080ff\"]", "\"#4080ff\", \"red\"]"), "asset: palette colour"), "a bad palette colour");
    CHECK(IS(parse_with("\"#000000\", \"#ff8040\", \"#4080ff\"", "\"#000000\", \"#ff8040\""), "asset: index past the palette"), "a pixel past the palette");
    /* bindings */
    CHECK(IS(parse_with("\"str\": \"$title\"", "\"str\": \"$titel\""), "binding: undeclared param"), "an undeclared param");
    CHECK(IS(parse_with("\"lit\": \"$lit\"", "\"lit\": \"$title\""), "binding: param of the wrong type"), "a text param in a number");
    CHECK(IS(parse_with("\"str\": \"$title\"", "\"str\": \"$lit\""), "binding: param of the wrong type"), "a number param in a text");
    CHECK(IS(parse_with("\"str\": \"$title\"", "\"str\": \"@title\""), "binding: @field outside each"), "@field outside each");
    CHECK(IS(parse_with("\"each\": \"$marks\"", "\"each\": \"$title\""), "binding: each needs a list param"), "each over a text param");
    CHECK(IS(parse_with("\"each\": \"$marks\"", "\"each\": \"marks\""), "binding: each needs a declared $param"), "each without $");
    CHECK(IS(parse_with("\"lit\": \"number\"", "\"lit\": \"code\""), "layout: param type"), "an unknown param type");
    /* shapes and ranges */
    CHECK(parse_with("\"polar\": [\"@a\", 0.72]", "\"polar\": [\"@a\", 0.72], \"x\": 0"), "polar and x");
    CHECK(parse_with("\"polar\": [\"@a\", 0.72]", "\"polar\": [\"@a\"]"), "a one-number polar");
    CHECK(parse_with("\"n\": 12", "\"n\": 12.5"), "ticks n not whole");
    CHECK(parse_with("\"n\": 12", "\"n\": 1000"), "ticks n too many");
    CHECK(parse_with("\"r\": 0.9, \"w\": 0.03, \"color\"", "\"r\": 1e30, \"w\": 0.03, \"color\""), "a huge radius");
    CHECK(parse_with("\"color\": \"#ffffff\"", "\"color\": \"white\""), "a colour that is not #rrggbb");
    CHECK(parse_with("{\"k\": \"ring\", \"r\": 0.9,", "{\"k\": \"ring\","), "a ring without r");
    CHECK(parse_with("\"pts\": [[-0.6, 0.6], [0.6, 0.6]]", "\"pts\": [[-0.6, 0.6]]"), "a one-point line");
}

static const char *ROOT;
static char s_sha[65];

static pkg_req_t req(const char *id, const char *body_sha, uint32_t size) {
    char args[1024];
    snprintf(args, sizeof(args), "{\"job\":\"j1\",\"id\":\"%s\",\"name\":\"Shapes\",\"url\":\"%spackages/%s/%s.json\",\"sha256\":\"%s\",\"size\":%u}",
             id, PKG_URL_PREFIX, id, id, body_sha, size);
    cJSON *a = cJSON_Parse(args);
    pkg_req_t r; const char *e = pkg_req_parse(a, &r);
    cJSON_Delete(a);
    if (e) { printf("req %s\n", e); s_fail++; }
    return r;
}

static const char *req_err(const char *key, const char *json_value) {
    char args[1024];
    snprintf(args, sizeof(args), "{\"id\":\"shapes\",\"name\":\"Shapes\",\"url\":\"%spackages/shapes/shapes.json\",\"sha256\":\"%s\",\"size\":%u,\"timeout_ms\":30000}",
             PKG_URL_PREFIX, s_sha, (unsigned)s_fix_len);
    cJSON *a = cJSON_Parse(args);
    cJSON_DeleteItemFromObject(a, key);
    if (json_value) cJSON_AddItemToObject(a, key, cJSON_Parse(json_value));
    pkg_req_t r; const char *e = pkg_req_parse(a, &r);
    cJSON_Delete(a);
    return e;
}

static void test_request(void) {
    CHECK(!req_err("job", NULL), "a valid request (job optional)");
    CHECK(!req_err("timeout_ms", NULL), "timeout optional");
    /* the allowlist: only the project repo's raw files, nothing that walks out of it */
    const char *bad[] = {
        "\"https://example.com/packages/shapes/shapes.json\"",
        "\"http://raw.githubusercontent.com/apresmoi/molty.uno/main/packages/shapes/shapes.json\"",
        "\"https://raw.githubusercontent.com/apresmoi/molty.uno.evil/main/shapes.json\"",
        "\"https://raw.githubusercontent.com/apresmoi/other/main/shapes.json\"",
        "\"https://raw.githubusercontent.com/apresmoi/molty.uno/../../evil/x/shapes.json\"",
        "\"https://raw.githubusercontent.com/apresmoi/molty.uno//evil.com/shapes.json\"",
        "\"https://raw.githubusercontent.com/apresmoi/molty.uno/main/shapes.json?x=1\"",
        "\"https://raw.githubusercontent.com/apresmoi/molty.uno/main/sh%2e%2eapes.json\"",
        "\"https://raw.githubusercontent.com/apresmoi/molty.uno/main/shapes.bin\"",
        "\"https://raw.githubusercontent.com@evil.com/apresmoi/molty.uno/main/shapes.json\"",
        "\"https://raw.githubusercontent.com/apresmoi/molty.uno/main/shapes.json#.json\"",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) CHECK(IS(req_err("url", bad[i]), "bad_url"), "url %s refused", bad[i]);
    CHECK(!req_err("url", "\"https://raw.githubusercontent.com/apresmoi/molty.uno/main/packages/shapes/shapes.json\""), "the repo's raw url");
    CHECK(IS(req_err("size", "262145"), "too_big"), "over 256 KB");
    CHECK(!req_err("size", "262144"), "256 KB exactly");
    CHECK(IS(req_err("size", "0"), "invalid_args") && IS(req_err("size", "1.5"), "invalid_args") && IS(req_err("size", "\"9\""), "invalid_args"), "size");
    CHECK(IS(req_err("sha256", "\"abc\""), "invalid_args") && IS(req_err("sha256", NULL), "invalid_args"), "sha256");
    CHECK(IS(req_err("id", "\"../x\""), "invalid_args") && IS(req_err("id", "\"\""), "invalid_args"), "id");
    CHECK(IS(req_err("name", "\"\""), "invalid_args") && IS(req_err("name", "\"a name far longer than thirty-two\""), "invalid_args"), "name");
    CHECK(IS(req_err("timeout_ms", "10"), "invalid_args"), "timeout");
}

static void test_store(void) {
    uint8_t *buf = malloc(PKG_FILE_MAX + 1);
    fake_t f = { .body = s_fix, .len = s_fix_len, .cl = (int64_t)s_fix_len };
    const pkg_io_t io = { &f, f_open, f_read, f_close, f_sha_start, f_sha_update, f_sha_finish };
    pkg_entry_t e, list[PKG_INSTALLED_MAX];
    CHECK(pkg_store_list(ROOT, list, PKG_INSTALLED_MAX) == 0, "nothing installed at first");

    /* refusals leave nothing behind */
    char wrong[65]; memcpy(wrong, s_sha, 65); wrong[0] = wrong[0] == '0' ? '1' : '0';
    pkg_req_t r = req("shapes", wrong, (uint32_t)s_fix_len);
    CHECK(IS(pkg_install(ROOT, &r, &io, buf, &e), "hash_mismatch"), "a hash mismatch");
    r = req("shapes", s_sha, (uint32_t)s_fix_len - 1);
    f.cl = -1;
    CHECK(IS(pkg_install(ROOT, &r, &io, buf, &e), "too_big"), "more bytes than declared (chunked)");
    f.cl = (int64_t)s_fix_len;
    CHECK(IS(pkg_install(ROOT, &r, &io, buf, &e), "too_big"), "a Content-Length over the declared size");
    r = req("shapes", s_sha, (uint32_t)s_fix_len + 1);
    CHECK(IS(pkg_install(ROOT, &r, &io, buf, &e), "hash_mismatch"), "fewer bytes than declared");
    f.open_err = "tls";
    r = req("shapes", s_sha, (uint32_t)s_fix_len);
    CHECK(IS(pkg_install(ROOT, &r, &io, buf, &e), "tls"), "a connection failure");
    f.open_err = NULL;
    r = req("other", s_sha, (uint32_t)s_fix_len);
    CHECK(IS(pkg_install(ROOT, &r, &io, buf, &e), "bad_package"), "a file that is another package");
    const char *junk = "{\"v\":\"molty-pkg/1\"}";
    char jsha[65]; sha_hex((const uint8_t *)junk, strlen(junk), jsha);
    fake_t fj = { .body = junk, .len = strlen(junk), .cl = -1 };
    const pkg_io_t ioj = { &fj, f_open, f_read, f_close, f_sha_start, f_sha_update, f_sha_finish };
    r = req("shapes", jsha, (uint32_t)strlen(junk));
    CHECK(IS(pkg_install(ROOT, &r, &ioj, buf, &e), "bad_package"), "a file that is not a valid package");
    CHECK(pkg_store_list(ROOT, list, PKG_INSTALLED_MAX) == 0, "no refusal left anything");

    /* install, then a "reboot": everything is read back from the directory */
    r = req("shapes", s_sha, (uint32_t)s_fix_len);
    CHECK(!pkg_install(ROOT, &r, &io, buf, &e) && !strcmp(e.id, "shapes") && !strcmp(e.version, "1.0.0") && e.size == s_fix_len, "installed");
    CHECK(pkg_store_list(ROOT, list, PKG_INSTALLED_MAX) == 1 && !strcmp(list[0].name, "Shapes") && list[0].kind == PKG_SCENES, "listed");
    size_t len = 0;
    char *back = pkg_store_read(ROOT, "shapes", &len);
    CHECK(back && len == s_fix_len && !memcmp(back, s_fix, len), "the stored bytes are the downloaded ones");
    pkg_store_free(back);
    CHECK(!pkg_store_read(ROOT, "nope", &len), "an unknown id reads nothing");
    char p[256]; snprintf(p, sizeof(p), "%s/%.8s.json", ROOT, s_sha);
    CHECK(access(p, F_OK) == 0, "the file is named by its hash");

    /* the quota: another 1 MB does not fit beside it; replacing it with the same size does */
    CHECK(IS(pkg_store_room(ROOT, "big", PKG_QUOTA - (uint32_t)s_fix_len + 1), "no_space"), "over the quota");
    CHECK(!pkg_store_room(ROOT, "big", PKG_QUOTA - (uint32_t)s_fix_len), "up to the quota");
    CHECK(!pkg_store_room(ROOT, "shapes", PKG_QUOTA), "a reinstall replaces its own bytes");

    /* a new version replaces the old one and its file */
    char *v2 = strdup(s_fix); char *at = strstr(v2, "\"1.0.0\""); memcpy(at, "\"1.0.1\"", 7);
    char v2sha[65]; sha_hex((const uint8_t *)v2, s_fix_len, v2sha);
    fake_t f2 = { .body = v2, .len = s_fix_len, .cl = (int64_t)s_fix_len };
    const pkg_io_t io2 = { &f2, f_open, f_read, f_close, f_sha_start, f_sha_update, f_sha_finish };
    r = req("shapes", v2sha, (uint32_t)s_fix_len);
    CHECK(!pkg_install(ROOT, &r, &io2, buf, &e) && !strcmp(e.version, "1.0.1"), "upgraded");
    CHECK(pkg_store_list(ROOT, list, PKG_INSTALLED_MAX) == 1 && !strcmp(list[0].version, "1.0.1"), "one entry, the new version");
    CHECK(access(p, F_OK) != 0, "the old file is gone");
    free(v2);

    /* the index lost mid-rewrite: index.tmp is read back */
    char ip[256], tp[256]; snprintf(ip, sizeof(ip), "%s/index.json", ROOT); snprintf(tp, sizeof(tp), "%s/index.tmp", ROOT);
    rename(ip, tp);
    CHECK(pkg_store_find(ROOT, "shapes", &e), "found through index.tmp");
    rename(tp, ip);

    /* remove; then a vanished file (SPIFFS rewritten) is not installed either */
    CHECK(IS(pkg_store_remove(ROOT, "nope"), "not_installed"), "remove an unknown id");
    CHECK(!pkg_store_remove(ROOT, "shapes") && pkg_store_list(ROOT, list, PKG_INSTALLED_MAX) == 0, "removed");
    CHECK(IS(pkg_store_remove(ROOT, "shapes"), "not_installed"), "removed twice");
    r = req("shapes", s_sha, (uint32_t)s_fix_len);
    CHECK(!pkg_install(ROOT, &r, &io, buf, &e), "reinstalled");
    remove(p);
    CHECK(!pkg_store_find(ROOT, "shapes", NULL) && pkg_store_list(ROOT, list, PKG_INSTALLED_MAX) == 0, "a vanished file is not installed");
    CHECK(IS(pkg_store_remove(ROOT, "shapes"), "not_installed"), "...and removing it says so");
    cJSON *j = pkg_entry_json(&e); char *t = cJSON_PrintUnformatted(j);
    char want[160]; snprintf(want, sizeof(want), "{\"id\":\"shapes\",\"name\":\"Shapes\",\"version\":\"1.0.0\",\"kind\":\"scenes\",\"size\":%u}", (unsigned)s_fix_len);
    CHECK(!strcmp(t, want), "list entry json: %s", t);
    free(t); cJSON_Delete(j);
    free(buf);
}

int main(int argc, char **argv) {
    if (argc < 2 || !(s_fix = read_file(argv[1], &s_fix_len))) { printf("usage: pkg_test fixtures/shapes.json\n"); return 2; }
    sha_hex((const uint8_t *)s_fix, s_fix_len, s_sha);
    static char root[200];
    snprintf(root, sizeof(root), "%s/molty-pkg-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    if (mkdir(root, 0700) != 0) { printf("cannot create %s\n", root); return 2; }
    ROOT = root;
    test_parse();
    test_request();
    test_store();
    char cmd[300]; snprintf(cmd, sizeof(cmd), "rm -rf %s", ROOT); if (system(cmd)) {}
    free(s_fix);
    printf("pkg_test: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
