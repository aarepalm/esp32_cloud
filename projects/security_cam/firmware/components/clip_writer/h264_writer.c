/*
 * h264_writer.c — H.264 elementary stream writer (Phase 2 stub)
 *
 * Writes raw H.264 NALUs to a .h264 file.
 * This stub compiles cleanly but does no real work.
 * Real implementation added in Phase 2 Step 12.
 */

#include "h264_writer.h"
#include <stdlib.h>
#include "esp_log.h"

static const char *TAG = "h264_writer";

struct h264_writer_t {
    FILE    *fp;
    uint32_t nalu_count;
};

h264_writer_t *h264_writer_open(const char *path)
{
    h264_writer_t *w = calloc(1, sizeof(*w));
    if (!w) {
        ESP_LOGE(TAG, "Out of heap");
        return NULL;
    }
    w->fp = fopen(path, "wb");
    if (!w->fp) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        free(w);
        return NULL;
    }
    ESP_LOGI(TAG, "h264_writer_open: %s — STUB (Phase 2)", path);
    return w;
}

esp_err_t h264_writer_write_nalu(h264_writer_t *w, const void *nalu, size_t len)
{
    if (!w || !w->fp) {
        return ESP_ERR_INVALID_STATE;
    }
    /* TODO Phase 2: fwrite(nalu, 1, len, w->fp) with start code prefix if needed */
    (void)nalu;
    (void)len;
    w->nalu_count++;
    return ESP_OK;
}

esp_err_t h264_writer_close(h264_writer_t *w)
{
    if (!w) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "h264_writer_close: %"PRIu32" NALUs written (stub)", w->nalu_count);
    if (w->fp) {
        fclose(w->fp);
    }
    free(w);
    return ESP_OK;
}
