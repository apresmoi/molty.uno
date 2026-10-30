/*
 * SPDX-License-Identifier: MIT
 *
 * improv_proto.h — Improv Wi-Fi Serial framing (https://www.improv-wifi.com/serial/).
 * Plain C, no ESP-IDF: built on the host by test/host/improv_test.c.
 *
 *   packet  "IMPROV" | version 1 | type | length | data[length] | checksum
 *   checksum = low byte of the sum of every byte before it (header included)
 *   RPC command data = command | data length | data      (client → device)
 *   RPC result  data = command | total length | (len | string)*   (device → client)
 *
 * The device's packets share the console UART with log lines: the client only looks
 * for "IMPROV" at the start of a line, so improv_frame() wraps a packet in newlines.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IMPROV_VERSION        1
#define IMPROV_HEADER         "IMPROV"
#define IMPROV_HEADER_LEN     6
#define IMPROV_MAX_DATA       255
/* "\n" + header + version, type, length + data + checksum + "\n" */
#define IMPROV_FRAME_MAX      (1 + IMPROV_HEADER_LEN + 3 + IMPROV_MAX_DATA + 1 + 1)

enum {                                /* packet types */
    IMPROV_TYPE_CURRENT_STATE = 0x01,
    IMPROV_TYPE_ERROR_STATE   = 0x02,
    IMPROV_TYPE_RPC           = 0x03,
    IMPROV_TYPE_RPC_RESULT    = 0x04,
};

enum {                                /* current state */
    IMPROV_STATE_STOPPED      = 0x00,
    IMPROV_STATE_READY        = 0x02, /* authorized: ready for credentials */
    IMPROV_STATE_PROVISIONING = 0x03,
    IMPROV_STATE_PROVISIONED  = 0x04,
};

enum {                                /* error state */
    IMPROV_ERR_NONE           = 0x00,
    IMPROV_ERR_INVALID_RPC    = 0x01,
    IMPROV_ERR_UNKNOWN_RPC    = 0x02,
    IMPROV_ERR_UNABLE_CONNECT = 0x03,
    IMPROV_ERR_UNKNOWN        = 0xFF,
};

enum {                                /* RPC commands */
    IMPROV_CMD_WIFI_SETTINGS  = 0x01,
    IMPROV_CMD_GET_STATE      = 0x02,
    IMPROV_CMD_GET_INFO       = 0x03,
    IMPROV_CMD_GET_NETWORKS   = 0x04,
    IMPROV_CMD_HOSTNAME       = 0x05,
    IMPROV_CMD_DEVICE_NAME    = 0x06,
    IMPROV_CMD_NETWORK_STATE  = 0x07,
};

/* Incremental receiver: feed it every byte read from the UART. */
typedef struct {
    uint8_t buf[IMPROV_HEADER_LEN + 3 + IMPROV_MAX_DATA + 1];
    uint16_t n;
} improv_rx_t;

typedef enum {
    IMPROV_RX_MORE = 0,       /* keep feeding                                        */
    IMPROV_RX_PACKET,         /* *type, *data, *len hold a valid packet (until next feed) */
    IMPROV_RX_BAD,            /* a whole packet arrived with a bad checksum or version */
} improv_rx_result_t;

void improv_rx_reset(improv_rx_t *rx);
improv_rx_result_t improv_rx_feed(improv_rx_t *rx, uint8_t byte,
                                  uint8_t *type, const uint8_t **data, uint8_t *len);

/** A complete packet, newline-wrapped, into out (≥ IMPROV_FRAME_MAX). Returns its size. */
size_t improv_frame(uint8_t *out, uint8_t type, const uint8_t *data, uint8_t len);

/** RPC result data (command, length, strings) into out (≥ IMPROV_MAX_DATA). Returns the
 *  data length, or 0 if the strings do not fit in one packet. */
size_t improv_result(uint8_t *out, uint8_t command, const char *const *strings, int count);

/** Split RPC data into command and payload. False when the inner length disagrees. */
bool improv_rpc(const uint8_t *data, uint8_t len, uint8_t *command,
                const uint8_t **payload, uint8_t *payload_len);

/** Wi-Fi settings payload → NUL-terminated ssid (1..32 bytes) and password (0..64).
 *  False when malformed or too long. */
bool improv_wifi_settings(const uint8_t *payload, uint8_t len,
                          char ssid[33], char pass[65]);

#ifdef __cplusplus
}
#endif
