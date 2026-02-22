/*
 * h264_writer.h — H.264 elementary stream writer (Phase 2 stub)
 *
 * Used when cam_caps_t.delivers_h264 == true (ESP32-P4 path).
 * Writes raw H.264 NALUs to a .h264 file.
 * MP4 muxing can be added later if needed.
 */

#pragma once

#include <stdio.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h264_writer_t h264_writer_t;

/**
 * @brief  Open an H.264 file for writing.
 * @param  path  Full path including .h264 extension.
 * @return Handle on success, NULL on error.
 */
h264_writer_t *h264_writer_open(const char *path);

/**
 * @brief  Append one H.264 NALU to the file.
 * @param  w     Writer handle.
 * @param  nalu  NALU data (including start code if present).
 * @param  len   NALU length in bytes.
 * @return ESP_OK on success.
 */
esp_err_t h264_writer_write_nalu(h264_writer_t *w, const void *nalu, size_t len);

/**
 * @brief  Finalise and close the H.264 file. Frees w.
 */
esp_err_t h264_writer_close(h264_writer_t *w);

#ifdef __cplusplus
}
#endif
