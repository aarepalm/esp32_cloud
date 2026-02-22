/*
 * wifi_manager_p4.c — WiFi for ESP32-P4-FunctionalEVBoard (Phase 2 stub)
 *
 * The P4 does not have built-in WiFi. It uses a companion ESP32-C6 chip
 * connected over SDIO. The esp_hosted component initialises the SDIO bridge
 * and exposes the standard esp_wifi API.
 *
 * STUB: This file does not include esp_wifi.h because WIFI_INIT_CONFIG_DEFAULT()
 * references Kconfig symbols that only exist when esp_hosted is configured.
 * Real implementation and esp_wifi calls will be added in Phase 2.
 *
 * Do NOT add esp_wifi to this file until esp_hosted is properly wired in.
 */

#include "wifi_manager.h"
#include "esp_log.h"

static const char *TAG = "wifi_p4";

void wifi_manager_connect(void)
{
    ESP_LOGE(TAG, "wifi_manager_p4 NOT IMPLEMENTED — Phase 2 stub");
    ESP_LOGE(TAG, "To implement: add esp_hosted SDIO init then standard esp_wifi calls");
    /*
     * TODO Phase 2:
     *
     * 1. Add esp_hosted managed component:
     *    idf.py add-dependency "espressif/esp_hosted"
     *
     * 2. Include and initialise esp_hosted before esp_wifi:
     *    #include "esp_hosted.h"
     *    esp_hosted_config_t hosted_cfg = ESP_HOSTED_DEFAULT_CONFIG();
     *    ESP_ERROR_CHECK(esp_hosted_init(&hosted_cfg));
     *
     * 3. After esp_hosted_init() the esp_wifi API behaves identically
     *    to the S3 path. Copy wifi_manager_s3.c connect logic here.
     *
     * Reference board SDIO pins: see ESP32-P4-FunctionalEVBoard schematic.
     */
}
