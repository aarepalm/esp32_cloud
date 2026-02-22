/*
 * avi_writer.c — MJPEG-in-AVI (RIFF AVI) writer
 *
 * AVI structure written on disk:
 *
 *   Offset  Size  Field
 *   ------  ----  -----
 *   0       12    RIFF <riff_size> AVI
 *   12      200   LIST <192> hdrl
 *     24    64      avih <56>  avi_main_header_t    ← avih_offset = 32
 *     88    124     LIST <116> strl
 *       100 64        strh <56>  avi_stream_header_t ← strh_offset = 108
 *       164 48        strf <40>  bitmapinfoheader_t
 *   212     12    LIST <movi_cb> movi               ← movi_start_offset = 212
 *   224     ...   [00dc chunks — one per JPEG frame]
 *   ---     ...   idx1 [avi_idx1_entry_t × frame_count]
 *
 * Header total: 224 bytes (fixed). Placeholder sizes patched at close:
 *   offset 4  : RIFF size = file_size - 8
 *   offset 216: movi LIST cb = movi_end - movi_start_offset - 8
 *   avih_offset+12: dwFlags |= AVIF_HASINDEX
 *   avih_offset+16: dwTotalFrames
 *   strh_offset+32: dwLength
 *
 * idx1.dwChunkOffset values are relative to movi_start_offset (the 'LIST'
 * fourcc), per the MSDN AVI spec: "offset from the start of the movi LIST".
 */

#include "avi_writer.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "avi_writer";

/* ---------------------------------------------------------------------------
 * RIFF / AVI structures (little-endian, packed)
 * -------------------------------------------------------------------------*/

#define FOURCC(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b)<<8) | \
                         ((uint32_t)(c)<<16) | ((uint32_t)(d)<<24))

/* AVI main header (avih chunk — 56 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t dwMicroSecPerFrame;
    uint32_t dwMaxBytesPerSec;
    uint32_t dwPaddingGranularity;
    uint32_t dwFlags;
    uint32_t dwTotalFrames;
    uint32_t dwInitialFrames;
    uint32_t dwStreams;
    uint32_t dwSuggestedBufferSize;
    uint32_t dwWidth;
    uint32_t dwHeight;
    uint32_t dwReserved[4];
} avi_main_header_t;

/* AVI stream header (strh chunk — 56 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t fccType;           /* offset  0 */
    uint32_t fccHandler;        /* offset  4 */
    uint32_t dwFlags;           /* offset  8 */
    uint16_t wPriority;         /* offset 12 */
    uint16_t wLanguage;         /* offset 14 */
    uint32_t dwInitialFrames;   /* offset 16 */
    uint32_t dwScale;           /* offset 20 */
    uint32_t dwRate;            /* offset 24 */
    uint32_t dwStart;           /* offset 28 */
    uint32_t dwLength;          /* offset 32 — patched at close */
    uint32_t dwSuggestedBufferSize; /* offset 36 */
    uint32_t dwQuality;         /* offset 40 */
    uint32_t dwSampleSize;      /* offset 44 */
    struct { int16_t left, top, right, bottom; } rcFrame; /* offset 48 */
} avi_stream_header_t;

/* BITMAPINFOHEADER (strf chunk — 40 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} bitmapinfoheader_t;

/* idx1 entry (16 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t ckid;
    uint32_t dwFlags;
    uint32_t dwChunkOffset;
    uint32_t dwChunkLength;
} avi_idx1_entry_t;

#define AVIF_HASINDEX   0x00000010u
#define AVIIF_KEYFRAME  0x00000010u

/* ---------------------------------------------------------------------------
 * Internal state
 * -------------------------------------------------------------------------*/

struct avi_writer_t {
    FILE             *fp;
    uint32_t          width;
    uint32_t          height;
    uint32_t          fps;
    uint32_t          frame_count;
    uint32_t          max_frames;
    uint32_t          movi_start_offset; /* file offset of 'LIST' movi fourcc */
    avi_idx1_entry_t *idx1_buf;          /* pre-allocated in PSRAM */
    long              avih_offset;       /* file position of avih chunk data */
    long              strh_offset;       /* file position of strh chunk data */
};

/* ---------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------*/

static void write_u32(FILE *fp, uint32_t v)    { fwrite(&v, 4, 1, fp); }
static void write_fourcc(FILE *fp, uint32_t v) { fwrite(&v, 4, 1, fp); }

/* ---------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------*/

avi_writer_t *avi_writer_open(const char *path, uint32_t width, uint32_t height,
                              uint32_t fps, uint32_t max_frames)
{
    avi_writer_t *w = calloc(1, sizeof(*w));
    if (!w) {
        ESP_LOGE(TAG, "Out of heap for writer struct");
        return NULL;
    }

    w->fp = fopen(path, "wb");
    if (!w->fp) {
        ESP_LOGE(TAG, "Cannot open %s for writing", path);
        free(w);
        return NULL;
    }

    w->width       = width;
    w->height      = height;
    w->fps         = (fps > 0) ? fps : 10;
    w->max_frames  = max_frames;
    w->frame_count = 0;

    /* Pre-allocate idx1 in PSRAM (60s × 10fps × 16 bytes ≈ 9.6 KB) */
    w->idx1_buf = heap_caps_malloc(max_frames * sizeof(avi_idx1_entry_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!w->idx1_buf) {
        ESP_LOGW(TAG, "PSRAM idx1 alloc failed, trying DRAM");
        w->idx1_buf = malloc(max_frames * sizeof(avi_idx1_entry_t));
    }
    if (!w->idx1_buf) {
        ESP_LOGE(TAG, "Cannot allocate idx1 buffer (%"PRIu32" entries)", max_frames);
        fclose(w->fp);
        free(w);
        return NULL;
    }

    uint32_t usec_per_frame = 1000000u / w->fps;

    /* -----------------------------------------------------------------------
     * Write RIFF AVI header (224 bytes, fixed layout — see file header comment)
     * -----------------------------------------------------------------------*/

    /* RIFF AVI */
    write_fourcc(w->fp, FOURCC('R','I','F','F'));
    write_u32   (w->fp, 0);                 /* riff_size — patched at close */
    write_fourcc(w->fp, FOURCC('A','V','I',' '));

    /* LIST hdrl (cb = 192 — fixed) */
    write_fourcc(w->fp, FOURCC('L','I','S','T'));
    write_u32   (w->fp, 192);
    write_fourcc(w->fp, FOURCC('h','d','r','l'));

    /* avih chunk */
    write_fourcc(w->fp, FOURCC('a','v','i','h'));
    write_u32   (w->fp, 56);
    w->avih_offset = ftell(w->fp);          /* position of avih data = 32 */
    {
        avi_main_header_t avih = {
            .dwMicroSecPerFrame    = usec_per_frame,
            .dwMaxBytesPerSec      = 0,     /* patched at close */
            .dwPaddingGranularity  = 0,
            .dwFlags               = 0,     /* AVIF_HASINDEX set at close */
            .dwTotalFrames         = 0,     /* patched at close */
            .dwInitialFrames       = 0,
            .dwStreams             = 1,
            .dwSuggestedBufferSize = width * height * 3 / 2,
            .dwWidth               = width,
            .dwHeight              = height,
            .dwReserved            = {0, 0, 0, 0},
        };
        fwrite(&avih, sizeof(avih), 1, w->fp);
    }

    /* LIST strl (cb = 116 — fixed) */
    write_fourcc(w->fp, FOURCC('L','I','S','T'));
    write_u32   (w->fp, 116);
    write_fourcc(w->fp, FOURCC('s','t','r','l'));

    /* strh chunk */
    write_fourcc(w->fp, FOURCC('s','t','r','h'));
    write_u32   (w->fp, 56);
    w->strh_offset = ftell(w->fp);          /* position of strh data = 108 */
    {
        avi_stream_header_t strh = {
            .fccType             = FOURCC('v','i','d','s'),
            .fccHandler          = FOURCC('M','J','P','G'),
            .dwFlags             = 0,
            .wPriority           = 0,
            .wLanguage           = 0,
            .dwInitialFrames     = 0,
            .dwScale             = 1,
            .dwRate              = w->fps,
            .dwStart             = 0,
            .dwLength            = 0,       /* patched at close */
            .dwSuggestedBufferSize = width * height * 3 / 2,
            .dwQuality           = 0xFFFFFFFFu,
            .dwSampleSize        = 0,
            .rcFrame             = {0, 0, (int16_t)width, (int16_t)height},
        };
        fwrite(&strh, sizeof(strh), 1, w->fp);
    }

    /* strf chunk (BITMAPINFOHEADER) */
    write_fourcc(w->fp, FOURCC('s','t','r','f'));
    write_u32   (w->fp, 40);
    {
        bitmapinfoheader_t strf = {
            .biSize          = 40,
            .biWidth         = (int32_t)width,
            .biHeight        = (int32_t)height,
            .biPlanes        = 1,
            .biBitCount      = 24,
            .biCompression   = FOURCC('M','J','P','G'),
            .biSizeImage     = width * height * 3,
            .biXPelsPerMeter = 0,
            .biYPelsPerMeter = 0,
            .biClrUsed       = 0,
            .biClrImportant  = 0,
        };
        fwrite(&strf, sizeof(strf), 1, w->fp);
    }

    /* LIST movi — size is a placeholder, patched at close */
    w->movi_start_offset = (uint32_t)ftell(w->fp); /* = 212, position of 'LIST' */
    write_fourcc(w->fp, FOURCC('L','I','S','T'));
    write_u32   (w->fp, 0);                 /* movi cb — patched at close */
    write_fourcc(w->fp, FOURCC('m','o','v','i'));
    /* Frame data starts here at offset 224 */

    ESP_LOGI(TAG, "avi_writer_open: %s (%"PRIu32"x%"PRIu32" @ %"PRIu32" fps)",
             path, width, height, fps);
    return w;
}

esp_err_t avi_writer_write_frame(avi_writer_t *w, const void *jpeg, size_t len)
{
    if (!w || !w->fp) {
        return ESP_ERR_INVALID_STATE;
    }
    if (w->frame_count >= w->max_frames) {
        ESP_LOGW(TAG, "Frame count exceeds pre-allocated idx1 capacity (%"PRIu32")",
                 w->max_frames);
        return ESP_ERR_NO_MEM;
    }

    /* Offset of this '00dc' chunk from the 'LIST' movi start (MSDN spec) */
    uint32_t chunk_offset = (uint32_t)ftell(w->fp) - w->movi_start_offset;

    /* Write '00dc' chunk header + JPEG data (padded to even length) */
    write_fourcc(w->fp, FOURCC('0','0','d','c'));
    write_u32   (w->fp, (uint32_t)len);
    if (fwrite(jpeg, 1, len, w->fp) != len) {
        ESP_LOGE(TAG, "Frame write failed at frame %"PRIu32, w->frame_count);
        return ESP_FAIL;
    }
    if (len & 1u) {
        uint8_t pad = 0;
        fwrite(&pad, 1, 1, w->fp);
    }

    /* Record idx1 entry */
    w->idx1_buf[w->frame_count].ckid          = FOURCC('0','0','d','c');
    w->idx1_buf[w->frame_count].dwFlags       = AVIIF_KEYFRAME;
    w->idx1_buf[w->frame_count].dwChunkOffset = chunk_offset;
    w->idx1_buf[w->frame_count].dwChunkLength = (uint32_t)len;

    w->frame_count++;
    return ESP_OK;
}

esp_err_t avi_writer_close(avi_writer_t *w)
{
    if (!w) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t frame_count = w->frame_count;

    /* End of movi data — record position before writing idx1 */
    long movi_end = ftell(w->fp);

    /* Append idx1 chunk */
    write_fourcc(w->fp, FOURCC('i','d','x','1'));
    write_u32   (w->fp, frame_count * (uint32_t)sizeof(avi_idx1_entry_t));
    fwrite(w->idx1_buf, sizeof(avi_idx1_entry_t), frame_count, w->fp);

    long file_end = ftell(w->fp);

    /* Patch RIFF size at offset 4 */
    fseek(w->fp, 4, SEEK_SET);
    write_u32(w->fp, (uint32_t)(file_end - 8));

    /* Patch movi LIST cb at movi_start_offset + 4
     * cb = bytes from 'movi' fourcc to end of movi data
     *    = movi_end - (movi_start_offset + 8)               */
    uint32_t movi_cb = (uint32_t)(movi_end - w->movi_start_offset) - 8;
    fseek(w->fp, (long)w->movi_start_offset + 4, SEEK_SET);
    write_u32(w->fp, movi_cb);

    /* Patch avih: read back, update dwFlags + dwTotalFrames + dwMaxBytesPerSec */
    fseek(w->fp, w->avih_offset, SEEK_SET);
    avi_main_header_t avih;
    fread(&avih, sizeof(avih), 1, w->fp);
    avih.dwFlags      |= AVIF_HASINDEX;
    avih.dwTotalFrames = frame_count;
    if (frame_count > 0 && w->fps > 0) {
        uint32_t video_bytes = (uint32_t)(movi_end - w->movi_start_offset - 12);
        uint32_t dur_ms      = frame_count * 1000u / w->fps;
        avih.dwMaxBytesPerSec = (dur_ms > 0) ? (video_bytes * 1000u / dur_ms) : 0;
    }
    fseek(w->fp, w->avih_offset, SEEK_SET);
    fwrite(&avih, sizeof(avih), 1, w->fp);

    /* Patch strh.dwLength (at strh_offset + 32) */
    fseek(w->fp, w->strh_offset + 32, SEEK_SET);
    write_u32(w->fp, frame_count);

    fclose(w->fp);
    free(w->idx1_buf);
    free(w);

    ESP_LOGI(TAG, "avi_writer_close: %"PRIu32" frames, AVI complete", frame_count);
    return ESP_OK;
}
