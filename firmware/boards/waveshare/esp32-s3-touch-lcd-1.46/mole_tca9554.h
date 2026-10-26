/*
 * mole_tca9554.h — TCA9554PWR I/O expander driver, IDF v5.x
 *
 * Thin wrapper around the IDF i2c_master API.
 * The TCA9554PWR sits at address 0x20 on I2C bus (SDA=GPIO11, SCL=GPIO10).
 * board.c creates the bus; we share it by accepting a bus handle.
 *
 * Pins used (from Waveshare 1.46 TCA9554PWR.h / Display_SPD2010.h):
 *   EXIO_PIN1  — LCD RST (active-low, set HIGH to bring LCD out of reset)
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TCA9554_ADDR        0x20

#define TCA9554_INPUT_REG   0x00
#define TCA9554_OUTPUT_REG  0x01
#define TCA9554_POLAR_REG   0x02
#define TCA9554_CONFIG_REG  0x03

/* Pin numbers (1-based, matching Waveshare's EXIO_PIN macros) */
#define TCA9554_PIN1        0   /* bit 0 in register */
#define TCA9554_PIN2        1
#define TCA9554_PIN3        2
#define TCA9554_PIN4        3
#define TCA9554_PIN5        4
#define TCA9554_PIN6        5
#define TCA9554_PIN7        6

/**
 * Initialise the TCA9554PWR on the given bus.
 * Sets all pins to output mode and output-LOW (safe default).
 * The bus must already be created (board.c).
 */
esp_err_t mole_tca9554_init(i2c_master_bus_handle_t bus);

/** Set a single output pin high (1) or low (0). */
esp_err_t mole_tca9554_set_pin(uint8_t pin, uint8_t level);

/** Read back the current OUTPUT register (not the input pins). */
uint8_t mole_tca9554_read_output(void);

#ifdef __cplusplus
}
#endif
