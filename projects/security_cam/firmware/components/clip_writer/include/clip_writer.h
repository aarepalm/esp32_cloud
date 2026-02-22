/*
 * clip_writer.h — Video clip recording to SD card
 *
 * clip_writer uses cam_caps_t (from camera_hal) at runtime to select
 * the AVI or H.264 path. No target ifdefs needed in the caller.
 *
 * Typical call sequence:
 *   clip_writer_configure(caps)        ← once at startup
 *   clip_writer_begin("clip_name")     ← on motion trigger
 *   clip_writer_write_frame(&frame)    ← for each frame
 *   clip_writer_end()                  ← close and finalise file
 */

#pragma once

#include "esp_err.h"
#include "camera_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Configure the clip writer based on camera capabilities.
 *         Selects AVI path if caps->delivers_jpeg, H.264 path if caps->delivers_h264.
 *         Must be called once before clip_writer_begin().
 */
esp_err_t clip_writer_configure(const cam_caps_t *caps);

/**
 * @brief  Begin a new clip.
 * @param  clip_name  Base name (no extension, no path).
 *                    File written to /sdcard/<clip_name>.avi (or .h264).
 * @return ESP_OK on success.
 */
esp_err_t clip_writer_begin(const char *clip_name);

/**
 * @brief  Write one frame to the current clip.
 * @param  frame  Frame from camera_hal_get_frame(). Must not be NULL.
 *                Caller retains ownership — do not release before this returns.
 * @return ESP_OK on success.
 */
esp_err_t clip_writer_write_frame(const cam_frame_t *frame);

/**
 * @brief  Finalise and close the current clip.
 *         Patches the AVI header (frame count, duration, idx1 table).
 *         Must be called even if zero frames were written.
 */
esp_err_t clip_writer_end(void);

#ifdef __cplusplus
}
#endif
