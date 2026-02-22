/*
 * cloud_client.h — Upload clips and thumbnails to S3 via presigned PUT URLs
 *
 * Flow:
 *   1. GET CONFIG_LAMBDA_PRESIGN_URL?clip=<name>.avi&thumb=<name>_thumb.jpg
 *      → JSON: { "clip_url": "...", "thumb_url": "..." }
 *   2. PUT /sdcard/<name>.avi  → clip_url
 *   3. PUT /sdcard/<name>_thumb.jpg → thumb_url
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Upload a clip and its thumbnail to S3.
 * @param  clip_name  Base name (no extension, no path).
 *                    Expects /sdcard/<clip_name>.avi and /sdcard/<clip_name>_thumb.jpg
 *                    to exist on the SD card.
 * @return ESP_OK on success.
 */
esp_err_t cloud_client_upload(const char *clip_name);

#ifdef __cplusplus
}
#endif
