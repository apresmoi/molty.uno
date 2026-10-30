/*
 * SPDX-License-Identifier: MIT
 *
 * improv_proto.c — Improv Wi-Fi Serial framing. See improv_proto.h.
 */
#include "improv_proto.h"

#include <string.h>

void improv_rx_reset(improv_rx_t *rx) { rx->n = 0; }

improv_rx_result_t improv_rx_feed(improv_rx_t *rx, uint8_t byte,
                                  uint8_t *type, const uint8_t **data, uint8_t *len)
{
    if (rx->n < IMPROV_HEADER_LEN) {
        /* match the header; on a mismatch this byte may still start a new one */
        if (byte == (uint8_t)IMPROV_HEADER[rx->n]) rx->n++;
        else rx->n = byte == (uint8_t)IMPROV_HEADER[0] ? 1 : 0;
        if (rx->n > 0) rx->buf[rx->n - 1] = byte;
        return IMPROV_RX_MORE;
    }
    rx->buf[rx->n++] = byte;
    if (rx->n < IMPROV_HEADER_LEN + 3) return IMPROV_RX_MORE;           /* version, type, length */
    const uint8_t plen = rx->buf[IMPROV_HEADER_LEN + 2];
    const uint16_t total = IMPROV_HEADER_LEN + 3 + plen + 1;
    if (rx->n < total) return IMPROV_RX_MORE;
    rx->n = 0;                                                           /* next byte: new packet */
    uint8_t sum = 0;
    for (uint16_t i = 0; i < total - 1; i++) sum = (uint8_t)(sum + rx->buf[i]);
    if (sum != rx->buf[total - 1] || rx->buf[IMPROV_HEADER_LEN] != IMPROV_VERSION) return IMPROV_RX_BAD;
    *type = rx->buf[IMPROV_HEADER_LEN + 1];
    *len = plen;
    *data = rx->buf + IMPROV_HEADER_LEN + 3;
    return IMPROV_RX_PACKET;
}

size_t improv_frame(uint8_t *out, uint8_t type, const uint8_t *data, uint8_t len)
{
    size_t n = 0;
    out[n++] = '\n';                    /* the client only matches "IMPROV" at a line start */
    const size_t start = n;
    memcpy(out + n, IMPROV_HEADER, IMPROV_HEADER_LEN);
    n += IMPROV_HEADER_LEN;
    out[n++] = IMPROV_VERSION;
    out[n++] = type;
    out[n++] = len;
    if (len) memcpy(out + n, data, len);
    n += len;
    uint8_t sum = 0;
    for (size_t i = start; i < n; i++) sum = (uint8_t)(sum + out[i]);
    out[n++] = sum;
    out[n++] = '\n';
    return n;
}

size_t improv_result(uint8_t *out, uint8_t command, const char *const *strings, int count)
{
    size_t n = 2;
    for (int i = 0; i < count; i++) {
        size_t l = strings[i] ? strlen(strings[i]) : 0;
        if (l > 255 || n + 1 + l > IMPROV_MAX_DATA) return 0;
        out[n++] = (uint8_t)l;
        if (l) memcpy(out + n, strings[i], l);
        n += l;
    }
    out[0] = command;
    out[1] = (uint8_t)(n - 2);
    return n;
}

bool improv_rpc(const uint8_t *data, uint8_t len, uint8_t *command,
                const uint8_t **payload, uint8_t *payload_len)
{
    if (len < 2 || data[1] != len - 2) return false;
    *command = data[0];
    *payload = data + 2;
    *payload_len = data[1];
    return true;
}

bool improv_wifi_settings(const uint8_t *payload, uint8_t len, char ssid[33], char pass[65])
{
    if (len < 2) return false;
    uint8_t sl = payload[0];
    if (sl < 1 || sl > 32 || 1 + sl + 1 > len) return false;
    uint8_t pl = payload[1 + sl];
    if (pl > 64 || 1 + sl + 1 + pl != len) return false;
    memcpy(ssid, payload + 1, sl);
    ssid[sl] = '\0';
    memcpy(pass, payload + 2 + sl, pl);
    pass[pl] = '\0';
    if (memchr(ssid, '\0', sl) || memchr(pass, '\0', pl)) return false;   /* embedded NUL */
    return true;
}
