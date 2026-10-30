/*
 * SPDX-License-Identifier: MIT
 *
 * app_main.c — Molty firmware entry point. The hardware is whatever board was
 * compiled in (board/molty_board.h, firmware/boards/); nothing here knows a pin.
 *
 * Boot order:
 *   diag log ring → board early init (power latch) → NVS → Improv serial setup
 *   (boards with a serial console) → SPIFFS + console
 *   (net/console_store) → USB-C NCM (boards with usb_net) + mDNS + Wi-Fi
 *   (net/network) → board init (display, touch, IMU, ...) → "fluid" creature
 *   (fluid/) → agent link (link/) → device events (sense/) → diag tasks → HTTP
 *   server (net/http)
 *   → OTA image validation → /ws/imu task
 *
 * Plug USB-C into a laptop and browse http://10.13.37.1/ (or http://molty.local/);
 * boards without native USB on the USB-C port are reached over Wi-Fi only.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "soc/rtc_cntl_reg.h"   /* RTC_CNTL_OPTION1_REG / FORCE_DOWNLOAD_BOOT */

#include "mole_diag.h"
#include "molty_board.h"
#include "mole_creature.h"
#include "mole_fluid.h"
#include "link_client.h"
#include "sense.h"
#include "network.h"
#include "console_store.h"
#include "http.h"
#include "improv_serial.h"

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

    // Board first-thing hook, e.g. the 1.46's battery power latch, which must be
    // driven within ~100 ms of the power button or the board switches off again.
    molty_board_early_init();

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

    // Wi-Fi setup over the USB cable on boards with a serial console (Improv): as early
    // as possible, since a browser that just reset the board gives up after ~1.5 s.
    improv_serial_start();

    // SPIFFS + console page. Must run before USB/Wi-Fi/httpd (see console_store.c).
    console_store_init();

    // Networking: USB-C NCM (10.13.37.1, DHCP server), mDNS, Wi-Fi STA. NCM needs the
    // chip's native USB on the USB-C port; boards with a USB-UART bridge there skip it.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    const bool usb_net = molty_board_declares(MOLTY_CAP_USB_NET);
    if (usb_net) ESP_ERROR_CHECK(net_usb_init());
    net_mdns_start();
    ESP_ERROR_CHECK(net_wifi_init());
    ESP_LOGI(TAG, "WiFi STA initialized");

    // Board: display, touch, IMU, audio, buttons — whatever it has, in its own order.
    // Runs ONCE here; creatures must not touch the hardware init. A capability that
    // fails to start is reported absent (molty_board_has) and the core degrades.
    molty_board_init();

    // Creature
    {
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

    // Device events (shake / tap / button → agent + fluid) and the status mark.
    ret = sense_start();
    if (ret != ESP_OK) ESP_LOGW(TAG, "device events unavailable: %s", esp_err_to_name(ret));

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

    if (usb_net) ESP_LOGI(TAG, "ready — browse http://" NET_USB_IP "/ (USB-C) or http://molty.local/ (Wi-Fi)");
    else ESP_LOGI(TAG, "ready — browse http://molty.local/ (Wi-Fi; this board has no USB-C network)");
}
