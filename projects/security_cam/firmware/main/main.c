/*
 * security_cam/main/main.c
 *
 * Application state machine — ZERO target ifdefs.
 * All hardware differences are behind the HAL layer in components/.
 *
 * State machine:
 *   INIT → MOTION_WATCH → (motion detected) → RECORDING → MOTION_WATCH
 *                                                    ↓
 *                                             cloud_client (background task)
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "camera_hal.h"
#include "wifi_manager.h"
#include "clip_writer.h"
#include "motion_detect.h"
#include "cloud_client.h"
#include "sdcard.h"
#include "boot_console.h"
#include "lcd_ui.h"
#include "button_adc.h"

static const char *TAG = "main";

/* Queue depth: large enough to absorb all pending clips from upload_all_pending() */
#define UPLOAD_QUEUE_DEPTH  20
#define CLIP_NAME_LEN       64

/* Motion-stop detection during recording — passive JPEG size differencing.
 *
 * When a scene is active, consecutive JPEG frames differ significantly in
 * compressed size (different content → different DCT coefficients → different
 * byte count). When the scene goes static, frame sizes stabilise.
 *
 * We check every written JPEG frame — no RECORD→MOTION→RECORD mode switch.
 * This eliminates the 370ms recording gap (and resulting video skip) that the
 * mode-switch approach caused every 5 seconds.
 *
 * JPEG_SIZE_MOTION_BYTES: consecutive frames must differ by at least this many
 * bytes to count as motion. A moving scene typically varies by 500–3000 bytes
 * per frame; a static scene varies by < 100 bytes. */
#define JPEG_SIZE_MOTION_BYTES  500
#define MOTION_STOP_TIMEOUT_S    8

static QueueHandle_t g_upload_queue;
static QueueHandle_t g_btn_queue;

/* ── upload_all_pending ─────────────────────────────────────────────────── */
/* Scan /sdcard/ for *.avi files and post each basename to the upload queue. */
static void upload_all_pending(void)
{
    DIR *d = opendir("/sdcard");
    if (!d) {
        ESP_LOGW(TAG, "upload_all_pending: cannot open /sdcard");
        return;
    }
    int queued = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < 5) continue;
        if (strcmp(e->d_name + len - 4, ".avi") != 0) continue;

        /* Strip ".avi" extension to get the basename */
        char basename[CLIP_NAME_LEN];
        strlcpy(basename, e->d_name, sizeof(basename));
        basename[len - 4] = '\0';

        if (xQueueSend(g_upload_queue, basename, 0) == pdTRUE) {
            ESP_LOGI(TAG, "Queued for upload: %s", basename);
            queued++;
        } else {
            ESP_LOGW(TAG, "Upload queue full — skipping %s", basename);
        }
    }
    closedir(d);
    ESP_LOGI(TAG, "upload_all_pending: queued %d clip(s)", queued);
}

/* Upload task — owns all WiFi/HTTP work, decoupled from recording loop */
static void upload_task(void *arg)
{
    char clip_name[CLIP_NAME_LEN];
    while (1) {
        /* Block until a clip filename is posted */
        if (xQueueReceive(g_upload_queue, clip_name, portMAX_DELAY) == pdTRUE) {
            ESP_LOGW(TAG, ">>> UPLOAD START  %s", clip_name);
            lcd_ui_notify_uploading(true, clip_name);

            esp_err_t err = cloud_client_upload(clip_name);
            if (err == ESP_OK) {
                ESP_LOGW(TAG, ">>> UPLOAD OK     %s", clip_name);
                /* Delete clip and thumbnail from SD after successful upload */
                char path[CLIP_NAME_LEN + 32];
                snprintf(path, sizeof(path), "/sdcard/%s.avi", clip_name);
                unlink(path);
                snprintf(path, sizeof(path), "/sdcard/%s_thumb.jpg", clip_name);
                unlink(path);
                lcd_ui_inc_uploaded();
            } else {
                ESP_LOGW(TAG, ">>> UPLOAD FAIL   %s  (%s)", clip_name, esp_err_to_name(err));
            }
            lcd_ui_notify_uploading(false, NULL);
        }
    }
}

/* Generate a clip base name from current time and device ID.
 * Format: <device_id>_YYYYMMDD_HHMMSS
 * Returns pointer to static buffer — copy before next call. */
static const char *make_clip_name(void)
{
    static char name[CLIP_NAME_LEN];
    time_t now = time(NULL);
    struct tm t;
    gmtime_r(&now, &t);
    snprintf(name, sizeof(name), "%s_%04d%02d%02d_%02d%02d%02d",
             CONFIG_DEVICE_ID,
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    return name;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Security camera starting");

    /* NVS — required by WiFi stack */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Boot console — 3-second window to enter interactive mode.
     * Offers: format SD card, list files, system info, erase NVS, etc. */
    boot_console_run();

    /* Step 1: Mount SD card — retry until a card is inserted */
    ESP_LOGI(TAG, "Mounting SD card...");
    {
        esp_err_t sd_err;
        while ((sd_err = sdcard_init()) != ESP_OK) {
            ESP_LOGW(TAG, "SD card not ready (%s) — insert card, retrying in 2s...",
                     esp_err_to_name(sd_err));
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    /* Step 2: Connect WiFi */
    ESP_LOGI(TAG, "Connecting WiFi...");
    wifi_manager_connect();

    /* Step 3: Initialise camera in motion-detection mode */
    ESP_LOGI(TAG, "Initialising camera...");
    ESP_ERROR_CHECK(camera_hal_init(CAM_MODE_MOTION));

    /* Step 4: Query capabilities — clip_writer uses these at runtime */
    const cam_caps_t *caps = camera_hal_get_caps();
    ESP_LOGI(TAG, "Camera caps: jpeg=%d h264=%d record=%"PRIu32"x%"PRIu32" motion=%"PRIu32"x%"PRIu32,
             caps->delivers_jpeg, caps->delivers_h264,
             caps->record_width, caps->record_height,
             caps->motion_width, caps->motion_height);

    /* Step 5: Configure clip writer for this hardware */
    ESP_ERROR_CHECK(clip_writer_configure(caps));

    /* Step 6: Initialise motion detector */
    motion_detect_config_t md_cfg = {
        .width     = caps->motion_width,
        .height    = caps->motion_height,
        .threshold = CONFIG_MOTION_THRESHOLD,
    };
    ESP_ERROR_CHECK(motion_detect_init(&md_cfg));

    /* Step 7: Start background upload task */
    g_upload_queue = xQueueCreate(UPLOAD_QUEUE_DEPTH, CLIP_NAME_LEN);
    ESP_ERROR_CHECK(g_upload_queue ? ESP_OK : ESP_ERR_NO_MEM);

    xTaskCreate(upload_task, "upload", 8192, NULL, 5, NULL);

    /* Step 8: Initialise LCD UI and button ADC */
    ESP_ERROR_CHECK(lcd_ui_init());
    ESP_ERROR_CHECK(button_adc_init());
    g_btn_queue = button_adc_get_queue();

    /* Main loop: motion watch → record → upload */
    ESP_LOGI(TAG, "Entering motion watch loop");

    bool recording = false;
    char current_clip[CLIP_NAME_LEN] = {0};
    int64_t record_start_us = 0;
    int64_t motion_last_seen_us = 0;
    int64_t next_frame_us = 0;     /* FPS limiter: earliest time to capture next record frame */
    int frame_count = 0;
    bool thumb_saved = false;
    size_t frame_prev_len = 0;     /* previous JPEG frame size for passive motion detection */

#define RECORD_FRAME_INTERVAL_US  (1000000LL / CONFIG_RECORD_FPS)

    while (1) {
        /* ── Button event drain (non-blocking) ─────────────────────────── */
        button_event_t btn;
        while (xQueueReceive(g_btn_queue, &btn, 0) == pdTRUE) {
            if (btn.id == BTN_MENU && btn.type == BTN_EVT_SHORT_PRESS) {
                lcd_ui_set_screen_on(!lcd_ui_get_screen_on());
            }
            if (btn.id == BTN_PLAY && btn.type == BTN_EVT_LONG_PRESS) {
                ESP_LOGI(TAG, "PLAY long press — queuing all pending clips");
                upload_all_pending();
            }
        }

        cam_frame_t frame;
        esp_err_t err = camera_hal_get_frame(&frame, 100 /* ms */);
        if (err != ESP_OK) {
            /* Timeout or transient error — keep looping */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (!recording) {
            /* --- MOTION WATCH --- */
            int score = motion_detect_score(&frame);
            camera_hal_release_frame(&frame);

            if (score >= CONFIG_MOTION_THRESHOLD) {
                ESP_LOGW(TAG, ">>> RECORD START  score=%d", score);

                /* Switch camera to record mode */
                ESP_ERROR_CHECK(camera_hal_set_mode(CAM_MODE_RECORD));

                /* Discard first 3 frames — OV2640 AE resets on reinit and needs
                 * ~3 frames to stabilise exposure to steady state. */
                { cam_frame_t f0; if (camera_hal_get_frame(&f0, 200) == ESP_OK) camera_hal_release_frame(&f0); }
                { cam_frame_t f0; if (camera_hal_get_frame(&f0, 200) == ESP_OK) camera_hal_release_frame(&f0); }
                { cam_frame_t f0; if (camera_hal_get_frame(&f0, 200) == ESP_OK) camera_hal_release_frame(&f0); }

                /* Open clip file */
                const char *base = make_clip_name();
                strlcpy(current_clip, base, sizeof(current_clip));
                ESP_ERROR_CHECK(clip_writer_begin(current_clip));

                recording = true;
                record_start_us    = esp_timer_get_time();
                motion_last_seen_us = record_start_us;
                next_frame_us       = record_start_us;
                frame_count = 0;
                thumb_saved = false;
                frame_prev_len = 0;

                lcd_ui_notify_recording(true, 0);
            }

        } else {
            /* --- RECORDING --- */

            /* Update elapsed time on the screen */
            int64_t now_us_rec = esp_timer_get_time();
            uint32_t elapsed_s = (uint32_t)((now_us_rec - record_start_us) / 1000000LL);
            lcd_ui_notify_recording(true, elapsed_s);

            /* Save first valid JPEG frame as thumbnail.
             * Check magic bytes (0xFF 0xD8) to reject any stale grayscale
             * frame that GRAB_LATEST may return right after the mode switch. */
            if (!thumb_saved && frame.fmt == CAM_PIXFMT_JPEG &&
                frame.len > 2 &&
                ((const uint8_t *)frame.data)[0] == 0xFF &&
                ((const uint8_t *)frame.data)[1] == 0xD8) {
                char thumb_path[CLIP_NAME_LEN + 32];
                snprintf(thumb_path, sizeof(thumb_path), "/sdcard/%s_thumb.jpg", current_clip);
                FILE *tf = fopen(thumb_path, "wb");
                if (tf) {
                    fwrite(frame.data, 1, frame.len, tf);
                    fclose(tf);
                    thumb_saved = true;
                    ESP_LOGI(TAG, "Thumbnail saved: %s (%u bytes)",
                             thumb_path, (unsigned)frame.len);
                }
            }

            /* Write frame to AVI — enforce CONFIG_RECORD_FPS rate.
             * The OV2640 at VGA JPEG outputs ~25fps natively; without this
             * gate the idx1 buffer (sized for CONFIG_RECORD_FPS) overflows
             * long before the 60s wall-clock limit is reached. */
            int64_t now_frame_us = esp_timer_get_time();
            if (now_frame_us >= next_frame_us) {
                clip_writer_write_frame(&frame);
                frame_count++;
                next_frame_us += RECORD_FRAME_INTERVAL_US;
                /* If we fall badly behind (e.g. after a long mode switch),
                 * reset rather than burst-writing to catch up. */
                if (next_frame_us < now_frame_us) {
                    next_frame_us = now_frame_us + RECORD_FRAME_INTERVAL_US;
                }
            }
            /* Passive motion detection: compare this JPEG frame size to the
             * previous one. A changing scene (motion) produces significant
             * frame-to-frame size variation; a static scene is stable.
             * No mode switch — no recording gap, no video skip. */
            if (frame_prev_len > 0) {
                int64_t delta = (int64_t)frame.len - (int64_t)frame_prev_len;
                if (delta < 0) delta = -delta;
                if (delta > JPEG_SIZE_MOTION_BYTES) {
                    motion_last_seen_us = now_frame_us;
                }
            }
            frame_prev_len = frame.len;

            camera_hal_release_frame(&frame);

            int64_t now_us = now_frame_us;

            /* Decide whether to stop recording */
            int64_t elapsed_us    = now_us - record_start_us;
            int64_t max_us        = (int64_t)CONFIG_MAX_CLIP_SECONDS * 1000000LL;
            int64_t no_motion_us  = now_us - motion_last_seen_us;
            int64_t stop_timeout  = (int64_t)MOTION_STOP_TIMEOUT_S * 1000000LL;

            bool stop_max      = (elapsed_us >= max_us);
            bool stop_no_motion = (no_motion_us >= stop_timeout) &&
                                  (frame_count > 5); /* ensure baseline established */

            if (stop_max || stop_no_motion) {
                if (stop_max) {
                    ESP_LOGW(TAG, ">>> RECORD STOP   max duration (%ds, %d frames)",
                             CONFIG_MAX_CLIP_SECONDS, frame_count);
                } else {
                    ESP_LOGW(TAG, ">>> RECORD STOP   motion gone (%ds idle, %d frames)",
                             MOTION_STOP_TIMEOUT_S, frame_count);
                }

                clip_writer_end();
                lcd_ui_notify_recording(false, 0);

                /* Signal background upload task */
                char upload_name[CLIP_NAME_LEN];
                strlcpy(upload_name, current_clip, sizeof(upload_name));
                if (xQueueSend(g_upload_queue, upload_name, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "Upload queue full — dropping clip %s", upload_name);
                }

                if (stop_max) {
                    /* Motion still happening — start new clip immediately */
                    const char *base = make_clip_name();
                    strlcpy(current_clip, base, sizeof(current_clip));
                    ESP_ERROR_CHECK(clip_writer_begin(current_clip));
                    record_start_us     = esp_timer_get_time();
                    motion_last_seen_us = record_start_us;
                    next_frame_us       = record_start_us;
                    frame_count = 0;
                    thumb_saved = false;
                    frame_prev_len = 0;
                    lcd_ui_notify_recording(true, 0);
                    ESP_LOGW(TAG, ">>> RECORD START  (continued after max duration)");
                } else {
                    /* Motion stopped — return to motion watch */
                    camera_hal_set_mode(CAM_MODE_MOTION);
                    motion_detect_reset();   /* full warmup for AE re-settling */
                    recording = false;
                    ESP_LOGI(TAG, "Returning to motion watch");
                }
            }
        }
    }
}
