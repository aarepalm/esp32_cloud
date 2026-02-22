/*
 * cloud_client.c — Upload clips and thumbnails to S3 via presigned PUT URLs
 *
 * Phase 1 Step 7 target. Real implementation replaces stubs below.
 *
 * Pattern identical to telemetry.c but uses PUT instead of POST,
 * and needs a two-step flow: GET presigned URL → PUT file.
 *
 * NOTE: esp_tls is NOT a standalone component in IDF v5.4.
 * Use esp_crt_bundle_attach from mbedtls for TLS trust. (Telemetry lesson learned.)
 */

#include "cloud_client.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "cloud_client";

/* Maximum presigned URL length — AWS SigV4 presigned URLs can reach ~1500 chars */
#define PRESIGN_URL_LEN 2048

/* HTTP response buffer for the presign Lambda response.
 * Two STS-session presigned URLs with large X-Amz-Security-Token can reach
 * ~1900 bytes of JSON. 4096 gives comfortable headroom. */
#define RESP_BUF_LEN    4096

static char s_clip_url [PRESIGN_URL_LEN];
static char s_thumb_url[PRESIGN_URL_LEN];
static char s_resp_buf [RESP_BUF_LEN];
static int  s_resp_len  = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int remaining = RESP_BUF_LEN - s_resp_len - 1;
        if (remaining > 0) {
            int copy = evt->data_len < remaining ? evt->data_len : remaining;
            memcpy(s_resp_buf + s_resp_len, evt->data, copy);
            s_resp_len += copy;
            s_resp_buf[s_resp_len] = '\0';
        }
    }
    return ESP_OK;
}

/* GET presigned URLs from Lambda Function URL */
static esp_err_t get_presigned_urls(const char *clip_name)
{
    char url[512];
    snprintf(url, sizeof(url), "%s?clip=%s.avi&thumb=%s_thumb.jpg",
             CONFIG_LAMBDA_PRESIGN_URL, clip_name, clip_name);

    s_resp_len = 0;
    memset(s_resp_buf, 0, sizeof(s_resp_buf));

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = http_event_handler,
        .timeout_ms        = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed — bad URL? (%s)", CONFIG_LAMBDA_PRESIGN_URL);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Presign GET failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Presign Lambda returned HTTP %d: %s", status, s_resp_buf);
        return ESP_FAIL;
    }

    /* Parse JSON: { "clip_url": "...", "thumb_url": "..." } */
    cJSON *root = cJSON_Parse(s_resp_buf);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed: %s", s_resp_buf);
        return ESP_FAIL;
    }

    const cJSON *clip_url  = cJSON_GetObjectItem(root, "clip_url");
    const cJSON *thumb_url = cJSON_GetObjectItem(root, "thumb_url");

    if (!cJSON_IsString(clip_url) || !cJSON_IsString(thumb_url)) {
        ESP_LOGE(TAG, "Missing clip_url or thumb_url in JSON");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    strlcpy(s_clip_url,  clip_url->valuestring,  sizeof(s_clip_url));
    strlcpy(s_thumb_url, thumb_url->valuestring, sizeof(s_thumb_url));
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Presigned URLs received OK");
    return ESP_OK;
}

/* PUT a file from SD card to a presigned S3 URL */
static esp_err_t put_file_to_s3(const char *sd_path, const char *presigned_url,
                                 const char *content_type)
{
    struct stat st;
    if (stat(sd_path, &st) != 0) {
        ESP_LOGE(TAG, "File not found: %s", sd_path);
        return ESP_ERR_NOT_FOUND;
    }

    FILE *f = fopen(sd_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", sd_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Uploading %s (%ld bytes) → S3", sd_path, (long)st.st_size);

    esp_http_client_config_t cfg = {
        .url               = presigned_url,
        .method            = HTTP_METHOD_PUT,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 120000,   /* 2 min — large AVI over home WiFi */
        .buffer_size_tx    = 32768,    /* larger TCP send buffer → fewer segments */
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed for PUT");
        fclose(f);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", content_type);

    esp_err_t err = esp_http_client_open(client, (int)st.st_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PUT open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        fclose(f);
        return err;
    }

    /* Stream file in 32 KB chunks from PSRAM.
     * 4 KB on the stack gave 2200+ iterations for a 9 MB clip.
     * 32 KB reduces that to ~280 iterations (8× fewer SD + TCP calls). */
    #define UPLOAD_CHUNK_SIZE  (32 * 1024)
    uint8_t *buf = heap_caps_malloc(UPLOAD_CHUNK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(UPLOAD_CHUNK_SIZE);   /* fallback to DRAM */
    }
    if (!buf) {
        ESP_LOGE(TAG, "Cannot allocate upload buffer");
        esp_http_client_cleanup(client);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    int64_t t_start = esp_timer_get_time();
    size_t remaining = (size_t)st.st_size;
    while (remaining > 0) {
        size_t to_read = remaining < UPLOAD_CHUNK_SIZE ? remaining : UPLOAD_CHUNK_SIZE;
        size_t n = fread(buf, 1, to_read, f);
        if (n == 0) {
            ESP_LOGE(TAG, "File read error at byte %zu", (size_t)st.st_size - remaining);
            break;
        }
        int written = esp_http_client_write(client, (char *)buf, (int)n);
        if (written < 0) {
            ESP_LOGE(TAG, "PUT write error");
            break;
        }
        remaining -= n;
    }
    free(buf);
    fclose(f);

    int64_t elapsed_ms = (esp_timer_get_time() - t_start) / 1000;
    if (elapsed_ms > 0) {
        ESP_LOGI(TAG, "PUT stream: %ld bytes in %lld ms → %lld KB/s",
                 (long)st.st_size, elapsed_ms,
                 (int64_t)st.st_size / elapsed_ms);
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "S3 PUT returned HTTP %d for %s", status, sd_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Upload complete: %s (HTTP %d)", sd_path, status);
    return ESP_OK;
}

esp_err_t cloud_client_upload(const char *clip_name)
{
    /* Step 1: get presigned PUT URLs */
    esp_err_t err = get_presigned_urls(clip_name);
    if (err != ESP_OK) {
        return err;
    }

    /* Step 2: upload clip */
    char clip_path[128];
    snprintf(clip_path, sizeof(clip_path), "/sdcard/%s.avi", clip_name);
    err = put_file_to_s3(clip_path, s_clip_url, "video/avi");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Clip upload failed: %s", esp_err_to_name(err));
        /* Continue to try thumbnail upload */
    }

    /* Step 3: upload thumbnail */
    char thumb_path[128];
    snprintf(thumb_path, sizeof(thumb_path), "/sdcard/%s_thumb.jpg", clip_name);
    esp_err_t thumb_err = put_file_to_s3(thumb_path, s_thumb_url, "image/jpeg");
    if (thumb_err != ESP_OK) {
        ESP_LOGW(TAG, "Thumbnail upload failed: %s", esp_err_to_name(thumb_err));
    }

    /* Clip upload status is the primary result.
     * Missing thumbnail is logged as a warning but doesn't fail the upload —
     * the S3 event trigger fires on the clip and the SES email still goes out. */
    return err;
}
