/*
 * avi_writer.h — MJPEG-in-AVI (RIFF AVI) writer
 *
 * Internal to clip_writer component.
 * clip_writer.c uses avi_writer when cam_caps_t.delivers_jpeg == true.
 *
 * AVI structure:
 *   RIFF AVI  {
 *     LIST hdrl {
 *       avih  (main AVI header — frame count, fps, dimensions)
 *       LIST strl {
 *         strh  (stream header)
 *         strf  (BITMAPINFOHEADER for MJPEG)
 *       }
 *     }
 *     LIST movi {
 *       00dc  (frame 0)
 *       00dc  (frame 1)
 *       ...
 *     }
 *     idx1    (index of all frames — patched at close)
 *   }
 *
 * The AVI header is written with placeholder values at open.
 * At close, the file is seeked back to patch frame count and size fields.
 * idx1 is pre-allocated in PSRAM and written in one fwrite() at close.
 */

#pragma once

#include <stdio.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct avi_writer_t avi_writer_t;

/**
 * @brief  Open an AVI file for writing.
 * @param  path     Full path including .avi extension.
 * @param  width    Frame width (must match all frames written).
 * @param  height   Frame height.
 * @param  fps      Target frame rate (used in stream header).
 * @param  max_frames  Pre-allocate idx1 for this many frames (e.g. 60s * fps).
 * @return Handle on success, NULL on error.
 */
avi_writer_t *avi_writer_open(const char *path, uint32_t width, uint32_t height,
                              uint32_t fps, uint32_t max_frames);

/**
 * @brief  Append one JPEG frame to the AVI file.
 * @param  w       Writer handle.
 * @param  jpeg    JPEG data.
 * @param  len     JPEG data length in bytes.
 * @return ESP_OK on success.
 */
esp_err_t avi_writer_write_frame(avi_writer_t *w, const void *jpeg, size_t len);

/**
 * @brief  Finalise and close the AVI file.
 *         Seeks back to patch the AVI header and appends the idx1 chunk.
 *         Frees w.
 * @return ESP_OK on success.
 */
esp_err_t avi_writer_close(avi_writer_t *w);

#ifdef __cplusplus
}
#endif
