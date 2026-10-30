/*
 * qmi8658.c — QMI8658 IMU driver (IDF v5.5), shared by the Waveshare boards.
 *
 * SPDX-License-Identifier: MIT
 *
 * Register sequences derived from Waveshare's ESP32-S3-Touch-LCD-1.46 demo code
 * (Arduino/examples/LVGL_Arduino/Gyro_QMI8658.cpp + .h) and the QMI8658 datasheet.
 * Key differences from the Arduino version:
 *   - Uses i2c_master.h (IDF v5.x new API), not the deprecated driver/i2c.h.
 *   - No Arduino Wire / I2C_Driver wrapper — raw IDF calls only.
 *   - Init sequence follows the Waveshare cpp exactly (setState running → setScale
 *     → setODR → setLPF) so the chip ends up in the same configuration.
 *   - Accel ±8 g, Gyro ±512 dps, 8000 Hz internal ODR (polled at 50 Hz).
 *
 * Device-frame axis mapping: per board (qmi8658_config_t.axes, see qmi8658.h). The
 * 1.46's { -2, -1, +3 } matches the ws_stream-1.46 wire protocol:
 *   ax = -chip_y,  ay = -chip_x,  az = +chip_z
 *   gx = -chip_gy, gy = -chip_gx, gz = +chip_gz
 */

#include "qmi8658.h"

#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"      /* imu_poll TWDT subscribe/feed — see sdkconfig.defaults'
                                * "make silent hangs confess" note; this task is the one
                                * whose stack-overflow the first-ever coredump caught. */

// ── Bus speed (the bus and the address come from the board) ──────────────────────
#define IMU_I2C_FREQ_HZ     400000

// ── QMI8658 register map (from Gyro_QMI8658.h) ────────────────────────────────
#define QMI_REVISION_ID     0x01
#define QMI_CTRL1           0x02
#define QMI_CTRL2           0x03   // accel settings
#define QMI_CTRL3           0x04   // gyro settings
#define QMI_CTRL5           0x06   // LPF settings
#define QMI_CTRL6           0x07   // AttitudeEngine (disabled)
#define QMI_CTRL7           0x08   // sensor enable
#define QMI_CTRL9           0x0A   // host command register
#define QMI_STATUSINT       0x2D
#define QMI_AX_L            0x35   // 12 bytes: ax_l,ax_h, ay_l,ay_h, az_l,az_h,
                                   //           gx_l,gx_h, gy_l,gy_h, gz_l,gz_h

// ── Scale / ODR enum values (mirror Waveshare enums) ──────────────────────────
// Accel scale bits sit at CTRL2[6:4]
#define QMI_ASCALE_OFFSET   4
#define QMI_ASCALE_8G       (2 << QMI_ASCALE_OFFSET)   // ACC_RANGE_8G = 0x2
#define QMI_AODR_8000       0x00                         // acc_odr_norm_8000

// Gyro scale bits sit at CTRL3[6:4]
#define QMI_GSCALE_OFFSET   4
#define QMI_GSCALE_512DPS   (5 << QMI_GSCALE_OFFSET)   // GYR_RANGE_512DPS = 0x5
#define QMI_GODR_8000       0x00                         // gyro_odr_norm_8000

// LPF mode 0 = 2.66% of ODR (applied to both accel and gyro)
#define QMI_ALPF_OFFSET     1
#define QMI_GLPF_OFFSET     5
#define QMI_LPF_MODE_0      0x00

// Accel scale factor for ±8 g
static const float ACCEL_SCALE = 8.0f / 32768.0f;
// Gyro scale factor for ±512 dps
static const float GYRO_SCALE  = 512.0f / 32768.0f;

static const char *TAG = "qmi8658";

// ── Module globals ─────────────────────────────────────────────────────────────
static i2c_master_dev_handle_t  s_imu_dev    = NULL;
static SemaphoreHandle_t        s_imu_mutex  = NULL;
static molty_imu_sample_t       s_sample     = {};
static int8_t                   s_axes[3]    = { 1, 2, 3 };

/* device axis from the chip's three: sign(m) × chip[|m| - 1] */
static inline float axis(const float chip[3], int8_t m)
{
    return m < 0 ? -chip[-m - 1] : chip[m - 1];
}

// ── Low-level I2C helpers ──────────────────────────────────────────────────────

static esp_err_t imu_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_imu_dev, buf, 2, 50 /* ms */);
}

static esp_err_t imu_read_regs(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(s_imu_dev, &reg, 1, out, len, 50 /* ms */);
}

// ── QMI8658 init (mirrors Waveshare's QMI8658_Init + setState(running)) ───────

static esp_err_t imu_chip_init(void)
{
    uint8_t rev = 0;
    ESP_RETURN_ON_ERROR(imu_read_regs(QMI_REVISION_ID, &rev, 1), TAG, "read revision");
    ESP_LOGI(TAG, "QMI8658 revision id: 0x%02x", rev);

    // setState(sensor_running):
    //   CTRL1: enable 2 MHz oscillator (bit0=0), enable auto address increment (bit6=1)
    uint8_t ctrl1;
    imu_read_regs(QMI_CTRL1, &ctrl1, 1);
    ctrl1 &= 0xFE;   // enable 2 MHz oscillator
    ctrl1 |= 0x40;   // auto address increment for fast block reads
    imu_write_reg(QMI_CTRL1, ctrl1);

    // CTRL7: enable high-speed clock, accel + gyro full mode, no syncSample
    imu_write_reg(QMI_CTRL7, 0x43);

    // CTRL6: disable AttitudeEngine
    imu_write_reg(QMI_CTRL6, 0x00);

    // setAccScale(ACC_RANGE_8G) + setAccODR(norm_8000)
    //   CTRL2[6:4] = scale bits, CTRL2[3:0] = ODR bits
    uint8_t ctrl2;
    imu_read_regs(QMI_CTRL2, &ctrl2, 1);
    ctrl2 &= ~0x70;   // clear scale bits
    ctrl2 |= QMI_ASCALE_8G;
    ctrl2 &= ~0x0F;   // clear ODR bits
    ctrl2 |= QMI_AODR_8000;
    imu_write_reg(QMI_CTRL2, ctrl2);

    // setAccLPF(LPF_MODE_0)
    uint8_t ctrl5;
    imu_read_regs(QMI_CTRL5, &ctrl5, 1);
    ctrl5 &= ~0x06;                      // clear ALPF bits [2:1]
    ctrl5 |= (QMI_LPF_MODE_0 << QMI_ALPF_OFFSET);
    ctrl5 |= 0x01;                       // enable accel LPF
    imu_write_reg(QMI_CTRL5, ctrl5);

    // setGyroScale(GYR_RANGE_512DPS) + setGyroODR(norm_8000)
    uint8_t ctrl3;
    imu_read_regs(QMI_CTRL3, &ctrl3, 1);
    ctrl3 &= ~0x70;
    ctrl3 |= QMI_GSCALE_512DPS;
    ctrl3 &= ~0x0F;
    ctrl3 |= QMI_GODR_8000;
    imu_write_reg(QMI_CTRL3, ctrl3);

    // setGyroLPF(LPF_MODE_3)  — Waveshare uses LPF_MODE_3 for gyro
    //   LPF_MODE_3 = 0x06, sits at CTRL5[6:5]
    imu_read_regs(QMI_CTRL5, &ctrl5, 1);
    ctrl5 &= ~0x60;
    ctrl5 |= (0x3 << QMI_GLPF_OFFSET);  // LPF_MODE_3 = 0x6 >> 1 = 0x3 shifted
    ctrl5 |= 0x10;                       // enable gyro LPF
    imu_write_reg(QMI_CTRL5, ctrl5);

    ESP_LOGI(TAG, "QMI8658 configured: accel ±8g @8kHz, gyro ±512dps @8kHz");
    return ESP_OK;
}

// ── 50 Hz polling task ─────────────────────────────────────────────────────────

static void imu_poll_task(void *arg)
{
    /* TWDT subscribe (a silent hang panics and leaves a coredump).
     * This task is created exactly once at boot (qmi8658_init) and never
     * deleted, so a single subscribe is enough — no matching delete needed. */
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK) {
        ESP_LOGW(TAG, "imu_poll: esp_task_wdt_add failed (%s) — no watchdog coverage",
                 esp_err_to_name(wdt_err));
    }

    const TickType_t period = pdMS_TO_TICKS(20);  // 50 Hz
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&last_wake, period);
        esp_task_wdt_reset();   /* feed once per iteration, before any early `continue` below */

        /* Read 12 bytes: AX_L … GZ_H starting at QMI_AX_L (auto-increment) */
        uint8_t buf[12];
        esp_err_t ret = imu_read_regs(QMI_AX_L, buf, 12);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "IMU read failed: %s", esp_err_to_name(ret));
            continue;
        }

        /* Parse raw int16 values */
        float chip_ax = (float)(int16_t)((buf[1]  << 8) | buf[0])  * ACCEL_SCALE;
        float chip_ay = (float)(int16_t)((buf[3]  << 8) | buf[2])  * ACCEL_SCALE;
        float chip_az = (float)(int16_t)((buf[5]  << 8) | buf[4])  * ACCEL_SCALE;
        float chip_gx = (float)(int16_t)((buf[7]  << 8) | buf[6])  * GYRO_SCALE;
        float chip_gy = (float)(int16_t)((buf[9]  << 8) | buf[8])  * GYRO_SCALE;
        float chip_gz = (float)(int16_t)((buf[11] << 8) | buf[10]) * GYRO_SCALE;

        /*
         * Device-frame axis map from the board (1.46: ax = -chip_ay, ay = -chip_ax,
         * az = +chip_az, same for the gyro). Dashboard naive mapping
         * dot_dx = +ax * gain, dot_dy = -ay * gain then follows physical tilt.
         */
        const float acc[3] = { chip_ax, chip_ay, chip_az }, gyr[3] = { chip_gx, chip_gy, chip_gz };
        molty_imu_sample_t s;
        s.ax = axis(acc, s_axes[0]);
        s.ay = axis(acc, s_axes[1]);
        s.az = axis(acc, s_axes[2]);
        s.gx = axis(gyr, s_axes[0]);
        s.gy = axis(gyr, s_axes[1]);
        s.gz = axis(gyr, s_axes[2]);
        s.ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

        xSemaphoreTake(s_imu_mutex, portMAX_DELAY);
        s_sample = s;
        xSemaphoreGive(s_imu_mutex);
    }
}

// ── Public API ─────────────────────────────────────────────────────────────────

esp_err_t qmi8658_init(const qmi8658_config_t *cfg)
{
    if (!cfg || !cfg->bus) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < 3; i++) {
        if (cfg->axes[i] == 0 || cfg->axes[i] < -3 || cfg->axes[i] > 3) return ESP_ERR_INVALID_ARG;
        s_axes[i] = cfg->axes[i];
    }
    s_imu_mutex = xSemaphoreCreateMutex();
    if (!s_imu_mutex) return ESP_ERR_NO_MEM;

    /* Add QMI8658 device */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = cfg->addr,
        .scl_speed_hz    = IMU_I2C_FREQ_HZ,
        .scl_wait_us     = 0,
        .flags = { .disable_ack_check = false },
    };
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(cfg->bus, &dev_cfg, &s_imu_dev),
        TAG, "i2c_master_bus_add_device");

    /* Configure the chip */
    ESP_RETURN_ON_ERROR(imu_chip_init(), TAG, "imu_chip_init");

    /* Spin up background polling task — pinned to core 0 to avoid
     * contention with the HTTP server (which defaults to core 1 on esp32s3). */
    BaseType_t xret = xTaskCreatePinnedToCore(
        imu_poll_task, "imu_poll",
        /* 4096: was 2048, which imu_poll overflowed under deploy-time
         * interrupt pressure — proven by the first flash coredump ever
         * pulled off this board (2026-07-13): "A stack overflow in task
         * imu_poll has been detected", after three deploy-path fixes
         * chased the wrong task. Cheap insurance on internal RAM. */
        4096, NULL, 5, NULL, 0);
    if (xret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed for imu_poll");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "IMU init complete — polling at 50 Hz on core 0");
    return ESP_OK;
}

void qmi8658_get(molty_imu_sample_t *out)
{
    xSemaphoreTake(s_imu_mutex, portMAX_DELAY);
    *out = s_sample;
    xSemaphoreGive(s_imu_mutex);
}
