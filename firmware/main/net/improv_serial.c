/*
 * SPDX-License-Identifier: MIT
 *
 * improv_serial.c — Improv Wi-Fi Serial on the console UART (improv_serial.h).
 *
 *   get state     READY (0x02), or PROVISIONED + http://<ip>/ when Wi-Fi is up
 *   wifi settings → net_wifi_join(): saved to NVS "wifi" like POST /api/set_creds,
 *                   then PROVISIONED + http://<ip>/ (the console over Wi-Fi), or
 *                   READY + error "unable to connect"
 *   device info   "molty.UNO", the version, "<board id>/esp32-s3" (ESPHome's board/chip
 *                   form: molty.uno/setup reads the board from it and offers only that
 *                   board's firmware), the board's name
 *   networks      one result per access point (ssid, rssi, YES|NO), then an empty one
 *   network state online/Wi-Fi flags + the URL
 *
 * While a browser talks Improv, the fluid holds "hi" so the user sees which device is on
 * the cable (like the pairing code, link/link_pair.c); a minute of silence goes back
 * to the creature (or the state before it, never the demo reel).
 *
 * Sharing the UART with logs: the UART driver serves both (uart_vfs_dev_use_driver),
 * a packet is written raw (no "\n" → "\r\n") while holding stdout's lock, so a log
 * line is never cut by a packet and a packet never by a log line; each packet sits on
 * its own line, which is all the client needs to find it.
 */
#include "improv_serial.h"
#include "improv_proto.h"
#include "network.h"
#include "molty_board.h"
#include "mole_creature.h"
#include "link_pair.h"
#include "link_protocol.h"
#include "sdkconfig.h"

#if CONFIG_ESP_CONSOLE_UART

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "improv";

#define IMPROV_UART        CONFIG_ESP_CONSOLE_UART_NUM
#define IMPROV_JOIN_MS     15000
#define IMPROV_RADIO_MS    10000     /* wait this long for net_wifi_init() at boot */

static uint8_t s_state = IMPROV_STATE_READY;

/* ── "hi" while a browser is on the cable ─────────────────────────────────── */

#define HELLO_IDLE_US  (60LL * 1000000)   /* no Improv traffic this long: put the fluid back */

static bool    s_hello;
static int64_t s_hello_seen;
static char    s_prev_creature[32];
static char   *s_prev_state;              /* the state JSON to restore (heap)               */

static void hello_show(void)
{
    s_hello_seen = esp_timer_get_time();
    if (s_hello || link_pair_active() || !molty_board_has(MOLTY_CAP_DISPLAY)) return;
    const char *cur = mole_creature_current();
    if (!cur) return;                     /* creatures not up yet (early boot): next packet */
    snprintf(s_prev_creature, sizeof(s_prev_creature), "%s", cur);
    if (mole_creature_state_get(&s_prev_state) != ESP_OK) s_prev_state = NULL;
    if (mole_creature_select("fluid") == ESP_OK &&
        mole_creature_state_set("{\"state\":\"text\",\"text\":\"hi\",\"hold\":true}") == ESP_OK) {
        s_hello = true;
        return;
    }
    free(s_prev_state);
    s_prev_state = NULL;
}

/* as link_pair_cancel: back to the previous creature and state (the creature for text
 * or the demo reel, link_restore_body) */
static void hello_restore(void)
{
    if (!s_hello) return;
    s_hello = false;
    if (!link_pair_active() && mole_creature_select(s_prev_creature) == ESP_OK && s_prev_state) {
        char *body = link_restore_body(s_prev_state);
        if (body) mole_creature_state_set(body);
        free(body);
    }
    free(s_prev_state);
    s_prev_state = NULL;
}

static void send(uint8_t type, const uint8_t *data, uint8_t len)
{
    uint8_t frame[IMPROV_FRAME_MAX];
    size_t n = improv_frame(frame, type, data, len);
    flockfile(stdout);               /* the log writer holds this for a whole line */
    fflush(stdout);
    uart_write_bytes(IMPROV_UART, frame, n);
    funlockfile(stdout);
}

static void send_state(uint8_t state)
{
    s_state = state;
    send(IMPROV_TYPE_CURRENT_STATE, &state, 1);
}

static void send_error(uint8_t err) { send(IMPROV_TYPE_ERROR_STATE, &err, 1); }

static void send_result(uint8_t cmd, const char *const *strings, int count)
{
    uint8_t data[IMPROV_MAX_DATA];
    size_t n = improv_result(data, cmd, strings, count);
    if (!n && count) { send_error(IMPROV_ERR_UNKNOWN); return; }
    send(IMPROV_TYPE_RPC_RESULT, data, (uint8_t)n);
}

static bool device_url(char *url, size_t len)
{
    char ip[20];
    if (!net_wifi_ip(ip, sizeof(ip))) return false;
    snprintf(url, len, "http://%s/", ip);
    return true;
}

static bool radio_ready(void)
{
    for (int waited = 0; !net_wifi_ready(); waited += 100) {
        if (waited >= IMPROV_RADIO_MS) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return true;
}

static void on_rpc(const uint8_t *data, uint8_t len)
{
    uint8_t cmd, plen;
    const uint8_t *payload;
    if (!improv_rpc(data, len, &cmd, &payload, &plen)) { send_error(IMPROV_ERR_INVALID_RPC); return; }
    send_error(IMPROV_ERR_NONE);      /* the spec: clear the error on every command */
    hello_show();
    char url[40];

    switch (cmd) {
    case IMPROV_CMD_GET_STATE:
        if (device_url(url, sizeof(url))) {
            send_state(IMPROV_STATE_PROVISIONED);
            const char *r[] = { url };
            send_result(cmd, r, 1);
        } else {
            send_state(s_state == IMPROV_STATE_PROVISIONING ? IMPROV_STATE_PROVISIONING : IMPROV_STATE_READY);
        }
        break;

    case IMPROV_CMD_GET_INFO: {
        char hw[80];
        snprintf(hw, sizeof(hw), "%s/esp32-s3", molty_board_id());
        const char *r[] = { "molty.UNO", esp_app_get_description()->version, hw, molty_board_name() };
        send_result(cmd, r, 4);
        break;
    }

    case IMPROV_CMD_WIFI_SETTINGS: {
        char ssid[33], pass[65];
        if (!improv_wifi_settings(payload, plen, ssid, pass)) { send_error(IMPROV_ERR_INVALID_RPC); break; }
        if (!radio_ready()) { send_error(IMPROV_ERR_UNKNOWN); break; }
        send_state(IMPROV_STATE_PROVISIONING);
        ESP_LOGI(TAG, "joining \"%s\" (credentials from the serial setup)", ssid);
        esp_err_t e = net_wifi_join(ssid, pass, IMPROV_JOIN_MS);
        memset(pass, 0, sizeof(pass));
        if (e == ESP_OK && device_url(url, sizeof(url))) {
            send_state(IMPROV_STATE_PROVISIONED);
            const char *r[] = { url };
            send_result(cmd, r, 1);
        } else {
            ESP_LOGW(TAG, "join failed: %s", esp_err_to_name(e));
            send_state(IMPROV_STATE_READY);
            send_error(IMPROV_ERR_UNABLE_CONNECT);
        }
        break;
    }

    case IMPROV_CMD_GET_NETWORKS: {
        net_ap_t aps[12];
        int n = radio_ready() ? net_wifi_scan(aps, 12) : -1;
        if (n < 0) { send_error(IMPROV_ERR_UNKNOWN); break; }
        for (int i = 0; i < n; i++) {
            if (!aps[i].ssid[0]) continue;                      /* hidden network */
            char rssi[8];
            snprintf(rssi, sizeof(rssi), "%d", aps[i].rssi);
            const char *r[] = { aps[i].ssid, rssi, aps[i].open ? "NO" : "YES" };
            send_result(cmd, r, 3);
        }
        send_result(cmd, NULL, 0);                              /* end of the list */
        break;
    }

    case IMPROV_CMD_NETWORK_STATE: {
        bool online = device_url(url, sizeof(url));
        const char *r[] = { online ? "3" : "2", url };          /* bit 0 online, bit 1 Wi-Fi */
        send_result(cmd, r, online ? 2 : 1);
        break;
    }

    default:
        send_error(IMPROV_ERR_UNKNOWN_RPC);
        break;
    }
}

static void improv_task(void *arg)
{
    (void)arg;
    improv_rx_t rx;
    improv_rx_reset(&rx);
    uint8_t buf[64];
    for (;;) {
        int n = uart_read_bytes(IMPROV_UART, buf, sizeof(buf), pdMS_TO_TICKS(1000));
        if (s_hello && esp_timer_get_time() - s_hello_seen > HELLO_IDLE_US) hello_restore();
        for (int i = 0; i < n; i++) {
            uint8_t type, len;
            const uint8_t *data;
            improv_rx_result_t r = improv_rx_feed(&rx, buf[i], &type, &data, &len);
            if (r == IMPROV_RX_PACKET && type == IMPROV_TYPE_RPC) on_rpc(data, len);
            else if (r == IMPROV_RX_BAD) send_error(IMPROV_ERR_INVALID_RPC);
        }
    }
}

void improv_serial_start(void)
{
    if (!molty_board_declares(MOLTY_CAP_SERIAL)) return;
    /* RX ring only: TX stays blocking (no TX ring), so a packet and a log line reach
     * the FIFO in the order their writers took stdout's lock */
    esp_err_t e = uart_driver_install(IMPROV_UART, 1024, 0, 0, NULL, 0);
    if (e != ESP_OK) { ESP_LOGW(TAG, "uart driver: %s — serial setup disabled", esp_err_to_name(e)); return; }
    uart_vfs_dev_use_driver(IMPROV_UART);
    if (xTaskCreatePinnedToCore(improv_task, "improv", 4096, NULL, 3, NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "no task — serial setup disabled");
        return;
    }
    ESP_LOGI(TAG, "Improv Wi-Fi Serial on UART%d (molty.uno/setup)", IMPROV_UART);
}

#else   /* no UART console in this build (e.g. the 1.46: CONFIG_ESP_CONSOLE_NONE) */

void improv_serial_start(void) {}

#endif
