/*
 * mole_touch.c — SPD2010 touch driver (port of Waveshare Touch_SPD2010.c to the
 * IDF i2c_master API + our TCA9554 wrapper). Polled, no INT.
 * Register sequences derived from Waveshare's ESP32-S3-Touch-LCD-1.46 demo code
 * and the SPD2010 datasheet.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mole_touch.h"
#include "link/link_pair.h"
#include "mole_tca9554.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "mole_touch";

#define SPD2010_ADDR 0x53
#define TP_TIMEOUT_MS 50

static i2c_master_dev_handle_t s_dev = NULL;
static i2c_master_bus_handle_t s_bus = NULL;   /* kept for wedged-bus recovery */

/* ── I2C (16-bit register pointer, big-endian) ──────────────────────────────── */
static esp_err_t tp_read(uint16_t reg, uint8_t *data, size_t len) {
    uint8_t a[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    return i2c_master_transmit_receive(s_dev, a, 2, data, len, TP_TIMEOUT_MS);
}
static esp_err_t tp_write(uint16_t reg, const uint8_t *data, size_t len) {
    uint8_t buf[8];
    if (len > sizeof(buf) - 2) return ESP_ERR_INVALID_SIZE;
    buf[0] = (uint8_t)(reg >> 8); buf[1] = (uint8_t)reg;
    memcpy(buf + 2, data, len);
    return i2c_master_transmit(s_dev, buf, len + 2, TP_TIMEOUT_MS);
}

/* ── Command writes (regs/payloads from the Waveshare driver) ───────────────── */
static void w_point_mode(void) { tp_write(0x5000, (uint8_t[]){0x00, 0x00}, 2); esp_rom_delay_us(200); }
static void w_tp_start(void)   { tp_write(0x4600, (uint8_t[]){0x00, 0x00}, 2); esp_rom_delay_us(200); }
static void w_cpu_start(void)  { tp_write(0x0400, (uint8_t[]){0x01, 0x00}, 2); esp_rom_delay_us(200); }
static void w_clear_int(void)  { tp_write(0x0200, (uint8_t[]){0x01, 0x00}, 2); esp_rom_delay_us(200); }

typedef struct {
    uint8_t pt_exist, gesture, aux;
    uint8_t cpu_run, tint_low, tic_in_cpu, tic_in_bios, tic_busy;
    uint16_t read_len;
} tp_status_t;

static esp_err_t read_status(tp_status_t *s) {
    uint8_t d[4] = {0};
    esp_err_t e = tp_read(0x2000, d, 4);
    esp_rom_delay_us(200);
    if (e != ESP_OK) return e;
    s->pt_exist    = d[0] & 0x01;
    s->gesture     = d[0] & 0x02;
    s->aux         = d[0] & 0x08;
    s->tic_busy    = (d[1] & 0x80) >> 7;
    s->tic_in_bios = (d[1] & 0x40) >> 6;
    s->tic_in_cpu  = (d[1] & 0x20) >> 5;
    s->tint_low    = (d[1] & 0x10) >> 4;
    s->cpu_run     = (d[1] & 0x08) >> 3;
    s->read_len    = (uint16_t)(d[3] << 8 | d[2]);
    return ESP_OK;
}

/* Recover a wedged touch controller / I2C bus: reset the bus, pulse the touch
 * RST line (TCA9554 pin 1), clear interrupt. Called after repeated read failures. */
static void tp_recover(void) {
    if (s_bus) i2c_master_bus_reset(s_bus);
    mole_tca9554_set_pin(TCA9554_PIN1, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    mole_tca9554_set_pin(TCA9554_PIN1, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    w_clear_int();
    ESP_LOGW(TAG, "touch recovered (bus/controller reset)");
}

/* Read the touch-point packet. Fills xs/ys, returns count. */
static int read_hdp(const tp_status_t *st, uint16_t *xs, uint16_t *ys, int max) {
    uint8_t d[4 + 10 * 6];
    int len = st->read_len;
    if (len > (int)sizeof(d)) len = sizeof(d);
    if (len < 4) return 0;
    tp_read(0x0003, d, len);
    if (d[4] > 0x0A || !st->pt_exist) return 0;     /* not a point packet */
    int n = (len - 4) / 6;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        int o = i * 6;
        xs[i] = (uint16_t)(((d[7 + o] & 0xF0) << 4) | d[5 + o]);
        ys[i] = (uint16_t)(((d[7 + o] & 0x0F) << 8) | d[6 + o]);
    }
    return n;
}

typedef struct { uint8_t status; uint16_t next_len; } tp_hdp_status_t;
static void read_hdp_status(tp_hdp_status_t *h) {
    uint8_t d[8] = {0};
    tp_read(0xFC02, d, 8);
    h->status = d[5];
    h->next_len = (uint16_t)(d[2] | d[3] << 8);
}
static void read_hdp_remain(const tp_hdp_status_t *h) {
    uint8_t d[32];
    int len = h->next_len; if (len > (int)sizeof(d)) len = sizeof(d);
    if (len > 0) tp_read(0x0003, d, len);
}

/* One full status->HDP exchange. Returns finger count (drives the handshake). */
static int s_tp_fail = 0;
static int tp_read_points(uint16_t *xs, uint16_t *ys, int max) {
    tp_status_t st = {0};
    if (read_status(&st) != ESP_OK) {       /* bus/controller not responding */
        if (++s_tp_fail >= 12) { tp_recover(); s_tp_fail = 0; }
        return 0;
    }
    s_tp_fail = 0;
    int n = 0;
    if (st.tic_in_bios) {
        w_clear_int(); w_cpu_start();
    } else if (st.tic_in_cpu) {
        w_point_mode(); w_tp_start(); w_clear_int();
    } else if (st.cpu_run && st.read_len == 0) {
        w_clear_int();
    } else if (st.pt_exist || st.gesture) {
        n = read_hdp(&st, xs, ys, max);
        for (int guard = 0; guard < 8; guard++) {   /* cap I2C reads so a finger-down poll can't stall the render */
            tp_hdp_status_t h = {0};
            read_hdp_status(&h);
            if (h.status == 0x82) { w_clear_int(); break; }
            else if (h.status == 0x00) { read_hdp_remain(&h); continue; }
            else break;
        }
    } else if (st.cpu_run && st.aux) {
        w_clear_int();
    }
    return n;
}

/* ── Public ─────────────────────────────────────────────────────────────────── */
esp_err_t mole_touch_init(i2c_master_bus_handle_t bus) {
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = SPD2010_ADDR,
        .scl_speed_hz    = 400000,
    };
    s_bus = bus;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add touch dev");

    /* reset via TCA9554 pin 1 (touch RST); LCD RST is pin 2, untouched */
    mole_tca9554_set_pin(TCA9554_PIN1, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    mole_tca9554_set_pin(TCA9554_PIN1, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* prime the firmware handshake (bios -> cpu) with a few exchanges */
    uint16_t xs[MOLE_TOUCH_MAX_POINTS], ys[MOLE_TOUCH_MAX_POINTS];
    for (int i = 0; i < 16; i++) { tp_read_points(xs, ys, MOLE_TOUCH_MAX_POINTS); vTaskDelay(pdMS_TO_TICKS(10)); }
    ESP_LOGI(TAG, "SPD2010 touch init OK (addr 0x53)");
    return ESP_OK;
}

int mole_touch_poll(uint16_t *xs, uint16_t *ys, int max) {
    if (!s_dev) return 0;
    if (max > MOLE_TOUCH_MAX_POINTS) max = MOLE_TOUCH_MAX_POINTS;
    int n = tp_read_points(xs, ys, max);
    link_pair_touch(n);
    return n;
}
