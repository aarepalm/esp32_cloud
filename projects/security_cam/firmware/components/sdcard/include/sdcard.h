/*
 * sdcard.h — SD card mount/unmount (SDMMC or SPI)
 *
 * Mounts the SD card at /sdcard and verifies write access.
 * Called once at startup before any file I/O.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Mount the SD card at /sdcard.
 *         Uses SDMMC in 1-bit SPI mode (compatible with ESP32-S3-EYE).
 *         Logs available space.
 * @return ESP_OK on success.
 */
esp_err_t sdcard_init(void);

/**
 * @brief  Unmount the SD card safely.
 */
esp_err_t sdcard_deinit(void);

/**
 * @brief  FAT32-format the SD card.
 *         Unmounts first if mounted, then mounts with format_if_mount_failed=true.
 *         Leaves the card mounted on success.
 * @return ESP_OK on success.
 */
esp_err_t sdcard_format(void);

#ifdef __cplusplus
}
#endif
