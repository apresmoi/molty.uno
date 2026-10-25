/*
 * mole_imu.h — QMI8658 IMU driver for IDF v5.x
 *
 * Uses i2c_master.h (NOT the deprecated driver/i2c.h).
 * I2C bus: SDA=GPIO11, SCL=GPIO10, addr=0x6B (QMI8658 with SA0=high).
 *
 * A background FreeRTOS task polls the IMU at 50 Hz internally and
 * caches the latest sample.  The snapshot HTTP endpoint and WebSocket
 * task read from that cache under a mutex — they never block on I2C.
 *
 * Axis mapping (device-frame, matches ws_stream-1.46 wire protocol):
 *   ax = -Accel.y   (chip-Y negated → device-X)
 *   ay = -Accel.x   (chip-X negated → device-Y)
 *   az =  Accel.z
 *   gx = -Gyro.y
 *   gy = -Gyro.x
 *   gz =  Gyro.z
 * The dashboard's naive  dot_dx = +ax * gain, dot_dy = -ay * gain  then
 * maps correctly to physical tilt without any further axis knowledge.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Latest cached IMU sample (device-frame, floating-point). */
typedef struct {
    float ax, ay, az;   /* acceleration in g */
    float gx, gy, gz;   /* angular rate in dps */
    uint32_t ms;        /* esp_timer uptime at capture time */
} mole_imu_sample_t;

/**
 * Initialise the I2C bus, configure the QMI8658, and start the 50 Hz
 * background polling task.  Call once from app_main before any endpoint
 * handlers are registered.
 */
esp_err_t mole_imu_init(void);

/**
 * Return the I2C master bus handle created by mole_imu_init().
 * Valid only after a successful call to mole_imu_init().
 * Used by mole_lcd_init() to add the TCA9554 expander to the same bus.
 */
i2c_master_bus_handle_t mole_imu_get_bus(void);

/**
 * Copy the most-recently-polled sample into *out.
 * Safe to call from any task (protected by internal mutex).
 */
void mole_imu_get(mole_imu_sample_t *out);

#ifdef __cplusplus
}
#endif
