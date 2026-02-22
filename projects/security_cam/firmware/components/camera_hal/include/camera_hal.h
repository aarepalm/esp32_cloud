/*
 * camera_hal.h — Hardware Abstraction Layer for camera
 *
 * This is the hardware contract. The same API is implemented by:
 *   esp32s3/camera_hal_s3.c  — OV2640 via DVP (esp_camera)
 *   esp32p4/camera_hal_p4.c  — MIPI-CSI + ISP (Phase 2 stub)
 *
 * main.c and all app code use ONLY this header. Zero target ifdefs outside
 * the HAL implementation files.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pixel format of a delivered frame */
typedef enum {
    CAM_PIXFMT_JPEG,       /* JPEG compressed (OV2640 HW JPEG, or ISP JPEG) */
    CAM_PIXFMT_GRAY8,      /* 8-bit grayscale, 1 byte/pixel */
    CAM_PIXFMT_YUV420,     /* YUV 4:2:0 planar */
    CAM_PIXFMT_H264_NALU,  /* H.264 Network Abstraction Layer Unit (P4 Phase 2) */
} cam_pixfmt_t;

/* Camera operating mode */
typedef enum {
    CAM_MODE_MOTION,   /* Low-resolution grayscale for motion detection */
    CAM_MODE_RECORD,   /* Full-resolution JPEG (or H.264) for recording */
} cam_mode_t;

/* A single captured frame.
 * data points into HAL-managed memory — do not free.
 * Call camera_hal_release_frame() when done. */
typedef struct {
    void    *data;          /* Frame payload */
    size_t   len;           /* Payload length in bytes */
    uint32_t width;         /* Frame width in pixels */
    uint32_t height;        /* Frame height in pixels */
    cam_pixfmt_t fmt;       /* Pixel format */
    uint64_t timestamp_us;  /* esp_timer_get_time() at capture */
} cam_frame_t;

/* Capabilities reported by camera_hal_get_caps().
 * clip_writer uses these at runtime to select AVI or H.264 path — no ifdefs. */
typedef struct {
    bool     delivers_jpeg;       /* True if RECORD mode delivers CAM_PIXFMT_JPEG */
    bool     delivers_h264;       /* True if RECORD mode delivers CAM_PIXFMT_H264_NALU */
    uint32_t record_width;        /* Width in RECORD mode */
    uint32_t record_height;       /* Height in RECORD mode */
    uint32_t motion_width;        /* Width in MOTION mode */
    uint32_t motion_height;       /* Height in MOTION mode */
} cam_caps_t;

/**
 * @brief  Initialise the camera hardware and start delivering frames.
 * @param  initial_mode  Starting mode (usually CAM_MODE_MOTION).
 * @return ESP_OK on success.
 */
esp_err_t camera_hal_init(cam_mode_t initial_mode);

/**
 * @brief  Switch camera mode.
 *         Blocks for up to ~300ms while the sensor stabilises.
 *         Discards frames until the output matches the new mode.
 * @param  mode  New mode.
 * @return ESP_OK on success.
 */
esp_err_t camera_hal_set_mode(cam_mode_t mode);

/**
 * @brief  Get the next available frame.
 * @param  f           Filled on success.
 * @param  timeout_ms  How long to wait for a frame.
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if no frame arrived.
 */
esp_err_t camera_hal_get_frame(cam_frame_t *f, uint32_t timeout_ms);

/**
 * @brief  Release a frame previously obtained via camera_hal_get_frame().
 *         Must be called before getting the next frame.
 */
esp_err_t camera_hal_release_frame(cam_frame_t *f);

/**
 * @brief  De-initialise the camera hardware.
 */
esp_err_t camera_hal_deinit(void);

/**
 * @brief  Return the static capabilities struct for this hardware.
 *         Valid after camera_hal_init(). Never NULL.
 */
const cam_caps_t *camera_hal_get_caps(void);

#ifdef __cplusplus
}
#endif
