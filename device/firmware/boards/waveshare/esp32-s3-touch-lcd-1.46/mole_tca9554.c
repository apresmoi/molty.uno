/*
 * mole_tca9554.c — TCA9554PWR I/O expander driver, IDF v5.x
 *
 * SPDX-License-Identifier: MIT
 *
 * Register sequences derived from Waveshare's ESP32-S3-Touch-LCD-1.46 demo code
 * (Arduino/examples/LVGL_Arduino/TCA9554PWR.cpp) and the TCA9554 datasheet.
 * Uses IDF i2c_master API (NOT the deprecated driver/i2c.h).
 * The I2C bus is created by board.c; this module adds itself as a device.
 */

#include "mole_tca9554.h"

#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "mole_tca9554";

static i2c_master_dev_handle_t s_dev = NULL;
/* Shadow copy of the OUTPUT register so we can do read-modify-write in set_pin. */
static uint8_t s_output_shadow = 0x00;

/* ── Low-level helpers ──────────────────────────────────────────────────────── */

static esp_err_t tca_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, 2, 50);
}

/* tca_read_reg: retained for future diagnostics; unused in the minimal boot path. */
__attribute__((unused))
static esp_err_t tca_read_reg(uint8_t reg, uint8_t *out)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, 1, 50);
}

/* ── Public API ─────────────────────────────────────────────────────────────── */

esp_err_t mole_tca9554_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = TCA9554_ADDR,
        .scl_speed_hz    = 400000,
        .scl_wait_us     = 0,
        .flags = { .disable_ack_check = false },
    };
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(bus, &dev_cfg, &s_dev),
        TAG, "i2c_master_bus_add_device for TCA9554");

    /* All pins output-LOW by default.
     * CONFIG_REG: 0x00 = all pins are outputs (bit=0 → output).
     * OUTPUT_REG: 0x00 = all outputs LOW. */
    s_output_shadow = 0x00;
    ESP_RETURN_ON_ERROR(tca_write_reg(TCA9554_OUTPUT_REG, s_output_shadow), TAG, "output reg");
    ESP_RETURN_ON_ERROR(tca_write_reg(TCA9554_CONFIG_REG, 0x00), TAG, "config reg");

    ESP_LOGI(TAG, "TCA9554PWR init OK (addr=0x%02x, all pins output-LOW)", TCA9554_ADDR);
    return ESP_OK;
}

esp_err_t mole_tca9554_set_pin(uint8_t pin, uint8_t level)
{
    if (pin > 6) return ESP_ERR_INVALID_ARG;
    if (level) {
        s_output_shadow |=  (uint8_t)(1u << pin);
    } else {
        s_output_shadow &= ~(uint8_t)(1u << pin);
    }
    return tca_write_reg(TCA9554_OUTPUT_REG, s_output_shadow);
}

uint8_t mole_tca9554_read_output(void)
{
    return s_output_shadow;
}
