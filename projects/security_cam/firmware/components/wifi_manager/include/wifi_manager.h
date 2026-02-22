/*
 * wifi_manager.h — WiFi connection HAL
 *
 * Implementations:
 *   esp32s3/wifi_manager_s3.c  — direct esp_wifi (same as telemetry project)
 *   esp32p4/wifi_manager_p4.c  — esp_hosted SDIO init then esp_wifi (Phase 2)
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Connect to WiFi using CONFIG_WIFI_SSID / CONFIG_WIFI_PASSWORD.
 *         Blocks until connected or reboots after max retries.
 *         Calls esp_netif_init() and esp_event_loop_create_default() internally.
 */
void wifi_manager_connect(void);

#ifdef __cplusplus
}
#endif
