/*
 * improv_test.c — host test for the Improv Wi-Fi Serial framing (main/net/improv_proto.c).
 *
 * Both directions against the reference client's rules (improv-wifi-serial-sdk 2.8.1,
 * dist/serial.js): its writePacketToStream() framing is what the device must parse, and
 * a C port of its _processInput()/_handleIncomingPacket() reads what the device sends —
 * including between console log lines, which share the UART.
 *
 * Build + run: test/host/improv_build.sh
 */
#include "improv_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail = 0, s_pass = 0;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* the client's writePacketToStream: header, type, length, data, checksum, "\n" */
static size_t client_packet(uint8_t *out, uint8_t type, const uint8_t *data, uint8_t len) {
    size_t n = 0;
    memcpy(out, "IMPROV", 6); n = 6;
    out[n++] = 1; out[n++] = type; out[n++] = len;
    memcpy(out + n, data, len); n += len;
    unsigned sum = 0;
    for (size_t i = 0; i < n; i++) sum += out[i];
    out[n++] = (uint8_t)sum;
    out[n++] = 10;
    return n;
}

/* port of the client's line-based reader: returns how many valid packets it saw; the
 * last one's type/data land in *type/data */
typedef struct { int packets, bad; uint8_t type, data[256], len; } seen_t;
static void client_read(const uint8_t *bytes, size_t n, seen_t *s) {
    uint8_t line[300]; size_t ln = 0; int is_improv = -1; size_t want = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = bytes[i];
        if (is_improv == 0) { if (b == 10) is_improv = -1; continue; }
        if (is_improv == 1) {
            line[ln++] = b;
            if (ln == want) {
                uint8_t plen = line[8], sum = 0;
                for (size_t k = 0; k < ln - 1; k++) sum = (uint8_t)(sum + line[k]);
                if (line[6] != 1 || sum != line[9 + plen]) s->bad++;
                else { s->packets++; s->type = line[7]; s->len = plen; memcpy(s->data, line + 9, plen); }
                is_improv = -1; ln = 0;
            }
            continue;
        }
        if (b == 10) { ln = 0; continue; }
        line[ln++] = b;
        if (ln != 9) continue;
        is_improv = memcmp(line, "IMPROV", 6) == 0;
        if (!is_improv) { ln = 0; continue; }
        want = 9 + (size_t)line[8] + 1;
    }
}

/* the client's RPC result decoding */
static int decode_result(const uint8_t *data, uint8_t len, char out[][64], int max) {
    (void)len;
    int count = 0; size_t idx = 2, total = data[1];
    while (idx < 2 + total && count < max) {
        uint8_t l = data[idx];
        memcpy(out[count], data + idx + 1, l); out[count][l] = '\0';
        count++; idx += l + 1u;
    }
    return count;
}

static int feed_all(improv_rx_t *rx, const uint8_t *b, size_t n, uint8_t *type, uint8_t *data, uint8_t *len, int *bad) {
    int packets = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *d; uint8_t t, l;
        improv_rx_result_t r = improv_rx_feed(rx, b[i], &t, &d, &l);
        if (r == IMPROV_RX_PACKET) { packets++; *type = t; *len = l; memcpy(data, d, l); }
        if (r == IMPROV_RX_BAD) (*bad)++;
    }
    return packets;
}

static void test_device_parses_client(void) {
    /* the spec's example: SSID MyWirelessAP, password mysecurepassword */
    const char *ex = "\x01\x1e\x0cMyWirelessAP\x10mysecurepassword";
    uint8_t pkt[300], data[256], type = 0, len = 0;
    size_t n = client_packet(pkt, IMPROV_TYPE_RPC, (const uint8_t *)ex, 32);
    improv_rx_t rx; improv_rx_reset(&rx);
    /* garbage, a stray "IMPRO", then the packet */
    const char *noise = "\r\nIIMPRO\x00\xffIMP";
    int bad = 0;
    feed_all(&rx, (const uint8_t *)noise, strlen(noise) + 2, &type, data, &len, &bad);
    int got = feed_all(&rx, pkt, n, &type, data, &len, &bad);
    CHECK(got == 1 && bad == 0 && type == IMPROV_TYPE_RPC && len == 32, "spec example: got %d bad %d type %d len %d", got, bad, type, len);
    uint8_t cmd, plen; const uint8_t *payload;
    CHECK(improv_rpc(data, len, &cmd, &payload, &plen) && cmd == IMPROV_CMD_WIFI_SETTINGS && plen == 30, "rpc split");
    char ssid[33], pass[65];
    CHECK(improv_wifi_settings(payload, plen, ssid, pass) && !strcmp(ssid, "MyWirelessAP") && !strcmp(pass, "mysecurepassword"),
          "wifi settings: '%s' '%s'", ssid, pass);

    /* corrupt checksum: reported bad, and the stream recovers for the next packet */
    pkt[n - 2] ^= 0x55;
    bad = 0;
    got = feed_all(&rx, pkt, n, &type, data, &len, &bad);
    CHECK(got == 0 && bad == 1, "bad checksum: got %d bad %d", got, bad);
    pkt[n - 2] ^= 0x55;
    got = feed_all(&rx, pkt, n, &type, data, &len, &bad);
    CHECK(got == 1, "recovery after a bad packet");
    /* wrong version */
    pkt[6] = 2; pkt[n - 2] = (uint8_t)(pkt[n - 2] + 1);
    bad = 0;
    got = feed_all(&rx, pkt, n, &type, data, &len, &bad);
    CHECK(got == 0 && bad == 1, "version 2 accepted");

    /* a zero-length command (get state) */
    const uint8_t gs[2] = { IMPROV_CMD_GET_STATE, 0 };
    n = client_packet(pkt, IMPROV_TYPE_RPC, gs, 2);
    got = feed_all(&rx, pkt, n, &type, data, &len, &bad);
    CHECK(got == 1 && improv_rpc(data, len, &cmd, &payload, &plen) && cmd == IMPROV_CMD_GET_STATE && plen == 0, "get state");
}

static void test_rpc_validation(void) {
    uint8_t cmd, plen; const uint8_t *payload;
    const uint8_t badlen[] = { 0x01, 0x05, 0x01 };
    CHECK(!improv_rpc(badlen, 3, &cmd, &payload, &plen), "inner length mismatch accepted");
    CHECK(!improv_rpc(badlen, 1, &cmd, &payload, &plen), "1-byte rpc accepted");
    char ssid[33], pass[65];
    const uint8_t empty_ssid[] = { 0, 0 };
    CHECK(!improv_wifi_settings(empty_ssid, 2, ssid, pass), "empty ssid accepted");
    uint8_t long_ssid[40] = { 33 };
    memset(long_ssid + 1, 'a', 33); long_ssid[34] = 0;
    CHECK(!improv_wifi_settings(long_ssid, 35, ssid, pass), "33-byte ssid accepted");
    const uint8_t trunc[] = { 3, 'a', 'b' };
    CHECK(!improv_wifi_settings(trunc, 3, ssid, pass), "truncated ssid accepted");
    const uint8_t extra[] = { 1, 'a', 1, 'p', 'x' };
    CHECK(!improv_wifi_settings(extra, 5, ssid, pass), "trailing bytes accepted");
    const uint8_t nul[] = { 2, 'a', 0, 0 };
    CHECK(!improv_wifi_settings(nul, 4, ssid, pass), "embedded NUL accepted");
    const uint8_t open[] = { 1, 'a', 0 };
    CHECK(improv_wifi_settings(open, 3, ssid, pass) && !strcmp(ssid, "a") && pass[0] == 0, "open network rejected");
}

static void test_client_parses_device(void) {
    /* device info between log lines, as it appears on the console UART */
    const char *info[] = { "molty.UNO", "0.1.0", "waveshare-esp32-s3-touch-lcd-1.28/esp32-s3", "Waveshare ESP32-S3-Touch-LCD-1.28" };
    uint8_t res[IMPROV_MAX_DATA], frame[IMPROV_FRAME_MAX], stream[2048];
    size_t rl = improv_result(res, IMPROV_CMD_GET_INFO, info, 4);
    CHECK(rl > 0, "info result does not fit");
    size_t fl = improv_frame(frame, IMPROV_TYPE_RPC_RESULT, res, (uint8_t)rl);
    CHECK(frame[0] == '\n' && frame[fl - 1] == '\n' && !memcmp(frame + 1, "IMPROV", 6), "frame not newline-wrapped");
    size_t n = 0;
    const char *log1 = "I (1234) board: Waveshare ESP32-S3-Touch-LCD-1.28 (240x240 round): display touch\r\nI (1240) net: WiFi STA";
    memcpy(stream + n, log1, strlen(log1)); n += strlen(log1);        /* a line still open: the leading \n closes it */
    memcpy(stream + n, frame, fl); n += fl;
    const char *log2 = "W (1300) mole_fluid: a log line right after\r\n";
    memcpy(stream + n, log2, strlen(log2)); n += strlen(log2);
    const uint8_t st = IMPROV_STATE_READY;
    n += improv_frame(stream + n, IMPROV_TYPE_CURRENT_STATE, &st, 1);
    seen_t s = { 0 };
    client_read(stream, n, &s);
    CHECK(s.packets == 2 && s.bad == 0, "client saw %d packets, %d bad", s.packets, s.bad);
    CHECK(s.type == IMPROV_TYPE_CURRENT_STATE && s.len == 1 && s.data[0] == IMPROV_STATE_READY, "state packet");
    /* the info packet alone, decoded as the client does */
    seen_t s1 = { 0 };
    client_read(frame, fl, &s1);
    char strs[8][64];
    int c = decode_result(s1.data, s1.len, strs, 8);
    CHECK(s1.packets == 1 && s1.data[0] == IMPROV_CMD_GET_INFO && c == 4 && !strcmp(strs[0], "molty.UNO") &&
          !strcmp(strs[3], "Waveshare ESP32-S3-Touch-LCD-1.28"), "info decode: %d strings", c);
    /* a byte value of 10 inside a packet (length, data, checksum) must not split it */
    const char *ten[] = { "\n\n\n\n\n\n\n\n" };                         /* data full of 0x0A */
    rl = improv_result(res, IMPROV_CMD_GET_NETWORKS, ten, 1);
    fl = improv_frame(frame, IMPROV_TYPE_RPC_RESULT, res, (uint8_t)rl);
    seen_t s2 = { 0 };
    client_read(frame, fl, &s2);
    CHECK(s2.packets == 1 && s2.bad == 0, "0x0A inside a packet broke it");
    /* the end-of-scan marker: an empty result */
    rl = improv_result(res, IMPROV_CMD_GET_NETWORKS, NULL, 0);
    CHECK(rl == 2 && res[0] == IMPROV_CMD_GET_NETWORKS && res[1] == 0, "empty result");
    /* strings that do not fit one packet are refused, never truncated */
    char big[200]; memset(big, 'x', 199); big[199] = 0;
    const char *two[] = { big, big };
    CHECK(improv_result(res, IMPROV_CMD_GET_INFO, two, 2) == 0, "oversized result accepted");
}

static void test_fuzz(void) {
    improv_rx_t rx; improv_rx_reset(&rx);
    uint8_t type, data[256], len; int bad = 0, packets = 0;
    srand(7);
    for (int i = 0; i < 200000; i++) {
        uint8_t b = (uint8_t)(rand() & 0xFF);
        if ((i & 31) == 0) b = 'I';
        packets += feed_all(&rx, &b, 1, &type, data, &len, &bad);
        if (rx.n > sizeof(rx.buf)) break;
    }
    CHECK(rx.n <= sizeof(rx.buf), "rx overflow");
    printf("  fuzz: %d accidental packets, %d bad, no overflow\n", packets, bad);
}

int main(void) {
    printf("improv host test\n");
    test_device_parses_client();
    test_rpc_validation();
    test_client_parses_device();
    test_fuzz();
    printf("%d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
