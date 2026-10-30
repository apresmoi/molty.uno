/*
 * SPDX-License-Identifier: MIT
 *
 * qmi8658.h — QMI8658 6-axis IMU (shared by the Waveshare 1.46 and 1.28 boards).
 *
 * Uses the IDF v5 i2c_master API on a bus the board created. A background task
 * polls the chip at 50 Hz (core 0) and caches the latest sample; readers take it
 * under a mutex and never block on I2C. Accel ±8 g, gyro ±512 dps.
 *
 * The board maps chip axes to the device frame (molty_board.h): device axis i =
 * sign(axes[i]) × chip axis |axes[i]| (1 = x, 2 = y, 3 = z). The same map applies to
 * the gyro. The 1.46: { -2, -1, +3 } — ax = -chip_y, ay = -chip_x, az = +chip_z.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "molty_board.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QMI8658_ADDR_SA0_HIGH 0x6B
#define QMI8658_ADDR_SA0_LOW  0x6A

typedef struct {
    i2c_master_bus_handle_t bus;
    uint8_t addr;
    int8_t  axes[3];
} qmi8658_config_t;

/** Add the chip on cfg->bus, configure it and start the 50 Hz polling task. Once. */
esp_err_t qmi8658_init(const qmi8658_config_t *cfg);

/** Copy the most recent sample (zeros before the first poll). Any task. */
void qmi8658_get(molty_imu_sample_t *out);

#ifdef __cplusplus
}
#endif
