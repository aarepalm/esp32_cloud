/*
 * camera_hal_p4.c — ESP32-P4 camera HAL stub (Phase 2)
 *
 * Hardware: MIPI-CSI camera + hardware ISP + hardware H.264 encoder.
 * Driver: esp_cam_ctlr_csi + esp_driver_isp (IDF native, no legacy component).
 *
 * This file is a compile-clean stub. Real implementation added in Phase 2.
 * The stub allows `idf.py set-target esp32p4 && idf.py build` to succeed
 * from day 1, confirming the build system structure is correct.
 *
 * Reference: $IDF_PATH/examples/peripherals/camera/camera_dsi/
 */

#include "camera_hal.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "camera_hal_p4";

/* P4 record capabilities — H.264 at 1080p */
#define MOTION_WIDTH    320
#define MOTION_HEIGHT   240
#define RECORD_WIDTH    1920
#define RECORD_HEIGHT   1080

static bool s_initialized = false;
static cam_mode_t s_current_mode;

static const cam_caps_t s_caps = {
    .delivers_jpeg  = false,   /* P4 delivers H.264 in record mode */
    .delivers_h264  = true,
    .record_width   = RECORD_WIDTH,
    .record_height  = RECORD_HEIGHT,
    .motion_width   = MOTION_WIDTH,
    .motion_height  = MOTION_HEIGHT,
};

esp_err_t camera_hal_init(cam_mode_t initial_mode)
{
    ESP_LOGI(TAG, "camera_hal_p4 init — STUB (Phase 2 not implemented)");
    /*
     * TODO Phase 2:
     *   - Configure MIPI-CSI interface
     *   - Initialise ISP pipeline
     *   - Start H.264 encoder (record mode) or grayscale path (motion mode)
     *   Reference: esp_cam_ctlr_csi_config_t, esp_cam_new_csi_ctlr()
     */
    s_current_mode = initial_mode;
    s_initialized = true;
    return ESP_OK;
}

esp_err_t camera_hal_set_mode(cam_mode_t mode)
{
    ESP_LOGI(TAG, "camera_hal_p4 set_mode %d — STUB", mode);
    /*
     * TODO Phase 2:
     *   Switch ISP/encoder configuration between motion and record paths.
     */
    s_current_mode = mode;
    vTaskDelay(pdMS_TO_TICKS(300));
    return ESP_OK;
}

esp_err_t camera_hal_get_frame(cam_frame_t *f, uint32_t timeout_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Stub: return a fake 1-byte frame */
    static uint8_t dummy = 0;
    f->data         = &dummy;
    f->len          = 1;
    f->width        = (s_current_mode == CAM_MODE_MOTION) ? MOTION_WIDTH  : RECORD_WIDTH;
    f->height       = (s_current_mode == CAM_MODE_MOTION) ? MOTION_HEIGHT : RECORD_HEIGHT;
    f->fmt          = (s_current_mode == CAM_MODE_MOTION) ? CAM_PIXFMT_GRAY8 : CAM_PIXFMT_H264_NALU;
    f->timestamp_us = 0;
    vTaskDelay(pdMS_TO_TICKS(timeout_ms > 100 ? 100 : timeout_ms));
    return ESP_OK;
}

esp_err_t camera_hal_release_frame(cam_frame_t *f)
{
    (void)f;
    return ESP_OK;
}

esp_err_t camera_hal_deinit(void)
{
    s_initialized = false;
    return ESP_OK;
}

const cam_caps_t *camera_hal_get_caps(void)
{
    return &s_caps;
}
