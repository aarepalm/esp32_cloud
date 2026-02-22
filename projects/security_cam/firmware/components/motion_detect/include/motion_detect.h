/*
 * motion_detect.h — Frame-differencing motion detector
 *
 * Pure algorithm — no hardware knowledge. Works on any cam_frame_t
 * with CAM_PIXFMT_GRAY8 format.
 *
 * Algorithm: compare consecutive grayscale frames pixel-by-pixel,
 * count pixels where |new - old| > pixel_threshold,
 * return the count as the motion score.
 */

#pragma once

#include "esp_err.h"
#include "camera_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t width;           /* Frame width (must match CAM_MODE_MOTION output) */
    uint32_t height;          /* Frame height */
    int      threshold;       /* Changed-pixel count to trigger recording */
    /* Optional tuning */
    uint8_t  pixel_threshold; /* Per-pixel change to count as "changed" (default 30) */
} motion_detect_config_t;

/**
 * @brief  Initialise the motion detector.
 *         Allocates a reference frame buffer in PSRAM.
 * @param  cfg  Configuration (width, height, threshold).
 * @return ESP_OK on success.
 */
esp_err_t motion_detect_init(const motion_detect_config_t *cfg);

/**
 * @brief  Compute a motion score for the given frame.
 *         The first call after init returns 0 (no reference frame yet).
 * @param  frame  GRAY8 frame from camera_hal.
 * @return Number of changed pixels. Compare to cfg.threshold.
 */
int motion_detect_score(const cam_frame_t *frame);

/**
 * @brief  Reset the reference frame (force next score to return 0).
 *         Call after switching back to motion mode post-recording.
 *         Uses the full WARMUP_FRAMES warmup for AE settling.
 */
void motion_detect_reset(void);

/**
 * @brief  Quick reset for periodic motion checks during recording.
 *         Uses a 1-frame warmup: first frame re-establishes the reference,
 *         second frame returns a valid score. No AE settling needed since
 *         the camera was recently in motion mode.
 */
void motion_detect_quick_reset(void);

/**
 * @brief  Free resources.
 */
void motion_detect_deinit(void);

#ifdef __cplusplus
}
#endif
