/*
 * motion_detect.c — Frame-differencing motion detector
 *
 * Algorithm:
 *   1. Receive GRAY8 frame (width × height bytes)
 *   2. Compare each pixel against the previous frame
 *   3. Count pixels where |new - old| > pixel_threshold
 *   4. Update the reference frame
 *   5. Return the changed-pixel count
 *
 * Phase 1 Step 5 target: implement the real algorithm.
 * Current status: STUB — counts nothing, always returns 0.
 */

#include "motion_detect.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "motion_detect";

/* Frames to discard on init/reset while AE/AWB settles */
#define WARMUP_FRAMES  30

static motion_detect_config_t s_cfg;
static uint8_t               *s_ref_frame   = NULL;
static bool                   s_has_ref     = false;
static size_t                 s_frame_bytes = 0;
static int                    s_warmup_left = WARMUP_FRAMES;

esp_err_t motion_detect_init(const motion_detect_config_t *cfg)
{
    if (!cfg || cfg->width == 0 || cfg->height == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg        = *cfg;
    s_frame_bytes = (size_t)cfg->width * cfg->height;

    /* Default pixel sensitivity if not set.
     * 40/255 ≈ 16% per-pixel change required — filters global brightness
     * shifts (clouds, lamp flicker) that affect the whole frame uniformly. */
    if (s_cfg.pixel_threshold == 0) {
        s_cfg.pixel_threshold = 40;
    }

    /* Allocate reference frame buffer in PSRAM if available */
    s_ref_frame = heap_caps_malloc(s_frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ref_frame) {
        ESP_LOGW(TAG, "PSRAM unavailable, allocating ref frame in DRAM");
        s_ref_frame = malloc(s_frame_bytes);
    }
    if (!s_ref_frame) {
        ESP_LOGE(TAG, "Cannot allocate reference frame buffer (%u bytes)",
                 (unsigned)s_frame_bytes);
        return ESP_ERR_NO_MEM;
    }

    s_has_ref     = false;
    s_warmup_left = WARMUP_FRAMES;
    ESP_LOGI(TAG, "Init: %"PRIu32"x%"PRIu32" px_thresh=%u trigger=%d",
             cfg->width, cfg->height, s_cfg.pixel_threshold, cfg->threshold);
    return ESP_OK;
}

int motion_detect_score(const cam_frame_t *frame)
{
    if (!frame || !frame->data || !s_ref_frame) {
        return 0;
    }

    /* Expect GRAY8 */
    if (frame->fmt != CAM_PIXFMT_GRAY8) {
        ESP_LOGW(TAG, "motion_detect_score called with non-GRAY8 frame (fmt=%d)", frame->fmt);
        return 0;
    }

    /* Warm-up: discard first WARMUP_FRAMES frames while AE/AWB settles.
     * Just update reference each time so the first real comparison is stable. */
    if (s_warmup_left > 0) {
        size_t copy_len = frame->len < s_frame_bytes ? frame->len : s_frame_bytes;
        memcpy(s_ref_frame, frame->data, copy_len);
        s_has_ref = true;
        s_warmup_left--;
        return 0;
    }

    const uint8_t *cur = (const uint8_t *)frame->data;
    const uint8_t *ref = s_ref_frame;
    int changed = 0;
    size_t n = frame->len < s_frame_bytes ? frame->len : s_frame_bytes;
    for (size_t i = 0; i < n; i++) {
        int diff = (int)cur[i] - (int)ref[i];
        if (diff < 0) diff = -diff;
        if (diff > (int)s_cfg.pixel_threshold) changed++;
    }
    memcpy(s_ref_frame, cur, n);
    return changed;
}

void motion_detect_reset(void)
{
    s_has_ref     = false;
    s_warmup_left = WARMUP_FRAMES;
}

void motion_detect_quick_reset(void)
{
    /* 1-frame warmup: first scored frame re-establishes the reference,
     * second frame returns a valid score. s_has_ref is preserved so that
     * the first warmup frame is treated as a reference update, not skipped. */
    s_warmup_left = 1;
}

void motion_detect_deinit(void)
{
    if (s_ref_frame) {
        free(s_ref_frame);
        s_ref_frame = NULL;
    }
    s_has_ref     = false;
    s_frame_bytes = 0;
}
