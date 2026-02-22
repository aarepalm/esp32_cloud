/*
 * clip_writer.c — Dispatch layer between clip_writer.h API and AVI/H.264 backends
 *
 * Uses cam_caps_t at runtime to route to avi_writer or h264_writer.
 * No target ifdefs here.
 */

#include "clip_writer.h"
#include "avi_writer.h"
#include "h264_writer.h"
#include "esp_log.h"
#include "sdcard.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "clip_writer";

/* Runtime-selected backend */
typedef enum { BACKEND_AVI, BACKEND_H264 } backend_t;

static backend_t        s_backend;
static const cam_caps_t *s_caps;
static avi_writer_t     *s_avi;
static h264_writer_t    *s_h264;

esp_err_t clip_writer_configure(const cam_caps_t *caps)
{
    if (!caps) {
        return ESP_ERR_INVALID_ARG;
    }
    s_caps = caps;

    if (caps->delivers_jpeg) {
        s_backend = BACKEND_AVI;
        ESP_LOGI(TAG, "Backend: AVI (MJPEG) — %"PRIu32"x%"PRIu32,
                 caps->record_width, caps->record_height);
    } else if (caps->delivers_h264) {
        s_backend = BACKEND_H264;
        ESP_LOGI(TAG, "Backend: H.264 — %"PRIu32"x%"PRIu32,
                 caps->record_width, caps->record_height);
    } else {
        ESP_LOGE(TAG, "Camera delivers neither JPEG nor H.264 — cannot configure clip_writer");
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

esp_err_t clip_writer_begin(const char *clip_name)
{
    char path[128];

    if (s_backend == BACKEND_AVI) {
        snprintf(path, sizeof(path), "/sdcard/%s.avi", clip_name);
        uint32_t max_frames = (uint32_t)(CONFIG_MAX_CLIP_SECONDS * CONFIG_RECORD_FPS);
        s_avi = avi_writer_open(path, s_caps->record_width, s_caps->record_height,
                                CONFIG_RECORD_FPS, max_frames);
        if (!s_avi) {
            ESP_LOGE(TAG, "avi_writer_open failed: %s", path);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Recording to %s", path);

    } else {
        snprintf(path, sizeof(path), "/sdcard/%s.h264", clip_name);
        s_h264 = h264_writer_open(path);
        if (!s_h264) {
            ESP_LOGE(TAG, "h264_writer_open failed: %s", path);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Recording to %s", path);
    }
    return ESP_OK;
}

esp_err_t clip_writer_write_frame(const cam_frame_t *frame)
{
    if (!frame) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_backend == BACKEND_AVI) {
        if (!s_avi) {
            return ESP_ERR_INVALID_STATE;
        }
        return avi_writer_write_frame(s_avi, frame->data, frame->len);
    } else {
        if (!s_h264) {
            return ESP_ERR_INVALID_STATE;
        }
        return h264_writer_write_nalu(s_h264, frame->data, frame->len);
    }
}

esp_err_t clip_writer_end(void)
{
    if (s_backend == BACKEND_AVI) {
        if (!s_avi) {
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t err = avi_writer_close(s_avi);
        s_avi = NULL;
        return err;
    } else {
        if (!s_h264) {
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t err = h264_writer_close(s_h264);
        s_h264 = NULL;
        return err;
    }
}
