/*
 * SPDX-License-Identifier: MIT
 *
 * app_main.c — Molty firmware entry point (Waveshare ESP32-S3-Touch-LCD-1.46).
 *
 * Boot order:
 *   diag log ring → power latch → NVS → SPIFFS + console (net/console_store)
 *   → USB-C NCM + mDNS + Wi-Fi (net/network) → IMU → LCD + touch
 *   → "fluid" creature (fluid/) → agent link (link/) → diag tasks
 *   → HTTP server (net/http) → OTA image validation → /ws/imu task
 *
 * Plug USB-C into a laptop and browse http://10.13.37.1/ (or http://molty.local/).
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "soc/rtc_cntl_reg.h"   /* RTC_CNTL_OPTION1_REG / FORCE_DOWNLOAD_BOOT */

#include "mole_diag.h"
#include "mole_imu.h"
#include "mole_lcd.h"
#include "mole_touch.h"
#include "mole_creature.h"
#include "mole_fluid.h"
#include "link_client.h"
#include "network.h"
#include "console_store.h"
#include "http.h"

static const char *TAG = "molty";

/* The fluid is the only creature: the boot creature and the registry fallback.
 * Character creatures (.molty files) are states of the fluid, not registry entries. */
static const mole_creature_t s_creature_fluid = {
    .name           = "fluid",
    .init           = mole_fluid_creature_init,
    .deinit         = mole_fluid_creature_deinit,
    .render         = mole_fluid_creature_render,
    .state_get_json = mole_fluid_creature_state_get_json,
    .state_set_json = mole_fluid_creature_state_set_json,
};

void app_main(void)
{
    mole_diag_early_init();   // log ring + ESP_LOG hook first: everything after is captured
    ESP_LOGI(TAG, "molty booting");

    // Clear the ROM download-mode latch in case /api/reboot_bootloader set it and
    // the post-flash reset preserved the RTC domain — prevents a download boot-loop.
    REG_CLR_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);

    // Battery power latch (Waveshare 1.46 soft-power button). On battery, the side
    // button only closes the rail momentarily; the firmware must drive GPIO 7 HIGH
    // within ~100 ms or the chip loses power when the button is released. On USB
    // power this is a no-op. Driving it LOW powers the device off.
    {
        gpio_config_t pwr_cfg = {
            .pin_bit_mask = (1ULL << 7),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&pwr_cfg);
        gpio_set_level(7, 1);
        ESP_LOGI(TAG, "battery power latched (GPIO 7 = HIGH)");
    }

    // NVS — Wi-Fi PHY calibration, credentials, link identity, boot counter
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Count boots before anything that could crash, so a crash loop is visible.
    mole_diag_boot_count_bump();
    ESP_LOGI(TAG, "boot_count=%lu reset_reason=%s",
             (unsigned long)mole_diag_boot_count(), mole_diag_reset_reason());

    // SPIFFS + console page. Must run before USB/Wi-Fi/httpd (see console_store.c).
    console_store_init();

    // Networking: USB-C NCM (10.13.37.1, DHCP server), mDNS, Wi-Fi STA
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(net_usb_init());
    net_mdns_start();
    ESP_ERROR_CHECK(net_wifi_init());
    ESP_LOGI(TAG, "WiFi STA initialized");

    // IMU — creates the I2C bus shared by the TCA9554 expander, LCD and touch
    ret = mole_imu_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "IMU init failed (%s) — IMU endpoints will return stale zeros",
                 esp_err_to_name(ret));
    }

    // LCD + touch + creature. mole_lcd_init() runs ONCE here; creatures must not call it.
    {
        i2c_master_bus_handle_t bus = mole_imu_get_bus();
        mole_creature_set_bus((void *)bus);

        ret = mole_lcd_init(bus);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "LCD init failed (%s) — creature rendering will be dark",
                     esp_err_to_name(ret));
        }
        if (mole_touch_init(bus) != ESP_OK) {
            ESP_LOGW(TAG, "touch init failed — taps/gestures disabled");
        }

        ESP_ERROR_CHECK(mole_creature_register(&s_creature_fluid));
        mole_creature_set_fallback("fluid");
        ret = mole_creature_select("fluid");
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "creature 'fluid' failed to start: %s", esp_err_to_name(ret));
        }
    }

    // Agent link owns its WSS/audio tasks; no device I/O in HTTP callbacks.
    ret = link_client_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "Agent link unavailable: %s", esp_err_to_name(ret));

    // Diagnostics sampler/persist tasks + Wi-Fi hooks (needs SPIFFS + Wi-Fi up).
    mole_diag_start();

    ESP_ERROR_CHECK(http_server_start());

    // OTA rollback validation: a freshly-OTA'd image (PENDING_VERIFY) that got this
    // far (HTTP server up) is marked valid. A crash before here rolls back on reset.
    {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_img_states_t st;
        if (esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "OTA image marked valid (rollback cancelled)");
        }
    }

    http_imu_ws_start();

    ESP_LOGI(TAG, "ready — browse http://" NET_USB_IP "/ (USB-C) or http://molty.local/ (Wi-Fi)");
}
