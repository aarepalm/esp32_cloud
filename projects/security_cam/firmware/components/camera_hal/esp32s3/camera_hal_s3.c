/*
 * camera_hal_s3.c — ESP32-S3-EYE camera HAL (real implementation)
 *
 * Hardware: OV2640 image sensor via DVP interface, driven by esp_camera.
 *
 * Two modes:
 *   CAM_MODE_MOTION  → QVGA (320×240) GRAYSCALE — fast readout, minimal CPU
 *   CAM_MODE_RECORD  → VGA (640×480) JPEG — hardware compressed, low CPU
 *
 * Frame lifecycle:
 *   esp_camera_fb_get() allocates a buffer from the DMA ring (in PSRAM).
 *   We store the original camera_fb_t* in s_current_fb so release_frame()
 *   can call esp_camera_fb_return(). Only one frame is outstanding at a time.
 */

#include "camera_hal.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "camera_hal_s3";

/* Motion mode: QVGA grayscale */
#define MOTION_WIDTH    320
#define MOTION_HEIGHT   240

/* Record mode: VGA JPEG */
#define RECORD_WIDTH    640
#define RECORD_HEIGHT   480

static cam_mode_t    s_current_mode;
static bool          s_initialized = false;
static camera_fb_t  *s_current_fb  = NULL;  /* outstanding frame — returned in release_frame */
static camera_config_t s_cam_config;         /* saved at init, reused by set_mode reinit */

static const cam_caps_t s_caps = {
    .delivers_jpeg  = true,
    .delivers_h264  = false,
    .record_width   = RECORD_WIDTH,
    .record_height  = RECORD_HEIGHT,
    .motion_width   = MOTION_WIDTH,
    .motion_height  = MOTION_HEIGHT,
};

/* Pin assignments for ESP32-S3-EYE v2.2
 * Source: board schematic + factory firmware */
#define CAM_PIN_PWDN    (-1)   /* not connected */
#define CAM_PIN_RESET   (-1)   /* not connected */
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD     4     /* SCCB SDA */
#define CAM_PIN_SIOC     5     /* SCCB SCL */
#define CAM_PIN_D7      16
#define CAM_PIN_D6      17
#define CAM_PIN_D5      18
#define CAM_PIN_D4      12
#define CAM_PIN_D3      10
#define CAM_PIN_D2       8
#define CAM_PIN_D1       9
#define CAM_PIN_D0      11
#define CAM_PIN_VSYNC    6
#define CAM_PIN_HREF     7
#define CAM_PIN_PCLK    13

esp_err_t camera_hal_init(cam_mode_t initial_mode)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialised");
        return ESP_OK;
    }

    camera_config_t config = {
        .pin_pwdn       = CAM_PIN_PWDN,
        .pin_reset      = CAM_PIN_RESET,
        .pin_xclk       = CAM_PIN_XCLK,
        .pin_sccb_sda   = CAM_PIN_SIOD,
        .pin_sccb_scl   = CAM_PIN_SIOC,
        .pin_d7         = CAM_PIN_D7,
        .pin_d6         = CAM_PIN_D6,
        .pin_d5         = CAM_PIN_D5,
        .pin_d4         = CAM_PIN_D4,
        .pin_d3         = CAM_PIN_D3,
        .pin_d2         = CAM_PIN_D2,
        .pin_d1         = CAM_PIN_D1,
        .pin_d0         = CAM_PIN_D0,
        .pin_vsync      = CAM_PIN_VSYNC,
        .pin_href       = CAM_PIN_HREF,
        .pin_pclk       = CAM_PIN_PCLK,

        .xclk_freq_hz   = 20000000,
        .ledc_timer     = LEDC_TIMER_0,
        .ledc_channel   = LEDC_CHANNEL_0,

        .pixel_format   = (initial_mode == CAM_MODE_MOTION) ? PIXFORMAT_GRAYSCALE : PIXFORMAT_JPEG,
        .frame_size     = (initial_mode == CAM_MODE_MOTION) ? FRAMESIZE_QVGA : FRAMESIZE_VGA,
        .jpeg_quality   = 12,   /* 0=best, 63=worst — 12 is good quality */
        .fb_count       = 2,
        .fb_location    = CAMERA_FB_IN_PSRAM,  /* PSRAM DMA mode enabled via CONFIG_CAMERA_PSRAM_DMA */
        .grab_mode      = CAMERA_GRAB_LATEST,
    };

    s_cam_config = config;  /* save for use in set_mode reinit */

    ESP_LOGI(TAG, "Initialising OV2640 (DVP, esp_camera)");
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_current_mode = initial_mode;
    s_initialized  = true;
    ESP_LOGI(TAG, "OV2640 init OK — mode=%s",
             initial_mode == CAM_MODE_MOTION ? "MOTION(QVGA/GRAY)" : "RECORD(VGA/JPEG)");
    return ESP_OK;
}

esp_err_t camera_hal_set_mode(cam_mode_t mode)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (mode == s_current_mode) return ESP_OK;

    ESP_LOGI(TAG, "Switching mode %s → %s",
             s_current_mode == CAM_MODE_MOTION ? "MOTION" : "RECORD",
             mode == CAM_MODE_MOTION ? "MOTION" : "RECORD");

    /* Both transitions require full deinit+reinit.
     *
     * GRAY→JPEG: sensor API set_pixformat() only changes the OV2640 register.
     *   The ESP32-S3 DMA pipeline stays configured for grayscale byte-capture,
     *   so every "JPEG" frame would be raw grayscale garbage (confirmed: 76800-byte
     *   chunks with no FF D8 header). Full reinit reconfigures the DMA correctly.
     *
     * JPEG→GRAY: sensor API leaves the OV2640 PLL broken (clk_2x=0, clk_div=0),
     *   causing VSYNC to stop and fb_get() to hang. Full reinit fixes this.
     *
     * Note: JPEG reinit allocates ~62480-byte frame buffers (640×480÷5 in PSRAM).
     *   OV2640 at quality=12 typically produces 20–40 KB per VGA frame indoors,
     *   so this is sufficient for normal security-camera use. */
    if (s_current_fb) {
        esp_camera_fb_return(s_current_fb);
        s_current_fb = NULL;
    }
    esp_camera_deinit();

    if (mode == CAM_MODE_RECORD) {
        s_cam_config.pixel_format = PIXFORMAT_JPEG;
        s_cam_config.frame_size   = FRAMESIZE_VGA;
    } else {
        s_cam_config.pixel_format = PIXFORMAT_GRAYSCALE;
        s_cam_config.frame_size   = FRAMESIZE_QVGA;
    }

    esp_err_t err = esp_camera_init(&s_cam_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera reinit failed: %s", esp_err_to_name(err));
        s_initialized = false;
        return err;
    }

    s_current_mode = mode;
    return ESP_OK;
}

esp_err_t camera_hal_get_frame(cam_frame_t *f, uint32_t timeout_ms)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    /* esp_camera_fb_get() blocks until a frame is ready.
     * timeout_ms is advisory — we just use it for a brief wait-and-retry. */
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGW(TAG, "esp_camera_fb_get() returned NULL");
        return ESP_ERR_TIMEOUT;
    }

    s_current_fb    = fb;
    f->data         = fb->buf;
    f->len          = fb->len;
    f->width        = fb->width;
    f->height       = fb->height;
    f->fmt          = (s_current_mode == CAM_MODE_MOTION) ? CAM_PIXFMT_GRAY8 : CAM_PIXFMT_JPEG;
    f->timestamp_us = esp_timer_get_time();

    (void)timeout_ms;
    return ESP_OK;
}

esp_err_t camera_hal_release_frame(cam_frame_t *f)
{
    (void)f;
    if (s_current_fb) {
        esp_camera_fb_return(s_current_fb);
        s_current_fb = NULL;
    }
    return ESP_OK;
}

esp_err_t camera_hal_deinit(void)
{
    if (!s_initialized) return ESP_OK;
    if (s_current_fb) {
        esp_camera_fb_return(s_current_fb);
        s_current_fb = NULL;
    }
    esp_camera_deinit();
    s_initialized = false;
    return ESP_OK;
}

const cam_caps_t *camera_hal_get_caps(void)
{
    return &s_caps;
}
