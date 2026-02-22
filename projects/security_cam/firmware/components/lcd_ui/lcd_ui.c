/*
 * lcd_ui.c — ST7789V status screen for the ESP32-S3-EYE.
 *
 * Hardware (confirmed from Zephyr DTS + CircuitPython board files):
 *   SPI2 host, 40 MHz
 *   SCK  = GPIO21   MOSI = GPIO47   CS   = GPIO44
 *   DC   = GPIO43   RST  = GPIO3    BL   = GPIO48
 *
 * Screen is 240×240 RGB565.
 *
 * Layout (y positions are top-left of each text block):
 *   y= 20  STATE LINE  2× font (16×32 per glyph), colour depends on state
 *   y= 80  UPLOAD LINE 1× font (8×16),  cyan
 *   y=120  "Free:    X.X GB"  white
 *   y=145  "Pending: N"       white
 *   y=170  "Done:    N"       white
 */

#include "lcd_ui.h"
#include "font8x16.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <dirent.h>

#include "esp_vfs_fat.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

static const char *TAG = "lcd_ui";

/* ── GPIO / SPI constants ───────────────────────────────────────────────── */
#define LCD_SCK   21
#define LCD_MOSI  47
#define LCD_CS    44
#define LCD_DC    43
#define LCD_RST    3
#define LCD_BL    48

#define LCD_HOST      SPI2_HOST
#define LCD_CLK_HZ    (40 * 1000 * 1000)
#define LCD_WIDTH     240
#define LCD_HEIGHT    240

/* ── Colour palette (RGB565) ────────────────────────────────────────────── */
#define COL_BLACK   0x0000u
#define COL_WHITE   0xFFFFu
#define COL_RED     0xF800u
#define COL_YELLOW  0xFFE0u
#define COL_CYAN    0x07FFu

/* ── State shared with notify functions ─────────────────────────────────── */
typedef struct {
    bool     recording;
    uint32_t elapsed_s;
    bool     uploading;
    char     clip_name[64];
    uint32_t done_count;
} ui_state_t;

static ui_state_t  g_state;
static SemaphoreHandle_t g_mutex;
static volatile bool  g_screen_on   = true;
static volatile bool  g_needs_clear = false;  /* set by set_screen_on, cleared by refresh_task */
static esp_lcd_panel_handle_t g_panel;

/* SD stats — written by sd_stats_task, read by refresh_task.
 * Both are floats/ints; no mutex needed (torn reads are harmless here). */
static volatile float g_sd_free_gb  = -1.0f;
static volatile int   g_sd_pending  = -1;

/* ── Low-level pixel buffer helpers ─────────────────────────────────────── */

/* Fill a rectangular area with a solid colour (RGB565 big-endian on-wire).
 * Draws one row at a time — draw_bitmap expects a buffer sized for exactly
 * the rectangle passed; sending a single row for a tall rect would DMA past
 * the end of the array. */
static void fill_rect(int x, int y, int w, int h, uint16_t colour)
{
    uint16_t row[LCD_WIDTH];
    uint16_t be = (colour >> 8) | (colour << 8);
    for (int i = 0; i < w; i++) row[i] = be;
    for (int ry = y; ry < y + h; ry++) {
        esp_lcd_panel_draw_bitmap(g_panel, x, ry, x + w, ry + 1, row);
    }
}

/*
 * Draw one character glyph at pixel position (px, py).
 * scale=1 → 8×16, scale=2 → 16×32.
 * fg/bg are RGB565.
 */
static void draw_char(int px, int py, char c, int scale,
                      uint16_t fg, uint16_t bg)
{
    uint16_t fg_be = (fg >> 8) | (fg << 8);
    uint16_t bg_be = (bg >> 8) | (bg << 8);

    const uint8_t *glyph = font8x16[(uint8_t)c];

    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        /* Build one (scaled) row of pixels */
        uint16_t line[16]; /* max 8×2 = 16 pixels */
        for (int col = 0; col < 8; col++) {
            uint16_t pix = (bits & (0x80 >> col)) ? fg_be : bg_be;
            for (int s = 0; s < scale; s++) {
                line[col * scale + s] = pix;
            }
        }
        for (int sr = 0; sr < scale; sr++) {
            esp_lcd_panel_draw_bitmap(g_panel,
                                      px, py + row * scale + sr,
                                      px + 8 * scale,
                                      py + row * scale + sr + 1,
                                      line);
        }
    }
}

/* Draw a NUL-terminated string; returns x after last glyph. */
static int draw_string(int px, int py, const char *s, int scale,
                       uint16_t fg, uint16_t bg)
{
    while (*s) {
        draw_char(px, py, *s, scale, fg, bg);
        px += 8 * scale;
        s++;
    }
    return px;
}

/*
 * Draw a string and then blank-pad the rest of the row area up to
 * max_chars (so leftover characters from longer previous strings disappear).
 */
static void draw_string_padded(int px, int py, const char *s, int scale,
                               uint16_t fg, uint16_t bg, int max_chars)
{
    int x = draw_string(px, py, s, scale, fg, bg);
    int glyph_w = 8 * scale;
    int written = (int)(x - px) / glyph_w;
    for (int i = written; i < max_chars; i++) {
        draw_char(px + i * glyph_w, py, ' ', scale, fg, bg);
    }
}

/* ── SD card queries ────────────────────────────────────────────────────── */

static float sd_free_gb(void)
{
    uint64_t total_bytes = 0, free_bytes = 0;
    if (esp_vfs_fat_info("/sdcard", &total_bytes, &free_bytes) != ESP_OK) return -1.0f;
    return (float)free_bytes / (1024.0f * 1024.0f * 1024.0f);
}

static int sd_pending_count(void)
{
    DIR *d = opendir("/sdcard");
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len > 4 && strcmp(e->d_name + len - 4, ".avi") == 0) n++;
    }
    closedir(d);
    return n;
}

/* ── SD stats task (separate from refresh — f_getfree blocks for seconds) ── */

static void sd_stats_task(void *arg)
{
    /* Stagger start so the initial FAT scan doesn't race with boot activity */
    vTaskDelay(pdMS_TO_TICKS(3000));
    while (1) {
        g_sd_free_gb = sd_free_gb();
        g_sd_pending = sd_pending_count();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ── Refresh task ───────────────────────────────────────────────────────── */

static void refresh_task(void *arg)
{
    /* Initial clear — safe here because no other task touches the LCD yet */
    fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COL_BLACK);

    bool prev_uploading = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(250));

        if (!g_screen_on) continue;

        /* Screen just turned back on — full clear before redrawing */
        if (g_needs_clear) {
            g_needs_clear = false;
            fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COL_BLACK);
        }

        ui_state_t s;
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        s = g_state;
        xSemaphoreGive(g_mutex);

        /* ── State line (2× font, y=20) ─────────────────────────────────── */
        char state_buf[28];
        uint16_t state_col;
        if (s.recording) {
            uint32_t mm = s.elapsed_s / 60;
            uint32_t ss = s.elapsed_s % 60;
            snprintf(state_buf, sizeof(state_buf), "REC %"PRIu32":%02"PRIu32, mm, ss);
            state_col = COL_RED;
        } else {
            snprintf(state_buf, sizeof(state_buf), "WATCHING");
            state_col = COL_YELLOW;
        }
        draw_string_padded(4, 20, state_buf, 2, state_col, COL_BLACK, 12);

        /* ── Upload line (1× font, y=80) ────────────────────────────────── */
        if (s.uploading != prev_uploading || s.uploading) {
            if (s.uploading) {
                draw_string_padded(4, 80, "Uploading...", 1, COL_CYAN, COL_BLACK, 20);
            } else {
                fill_rect(4, 80, LCD_WIDTH - 4, 16, COL_BLACK);
            }
            prev_uploading = s.uploading;
        }

        /* ── SD stats — read from cache updated by sd_stats_task ────────── */
        float  sd_free    = g_sd_free_gb;
        int    sd_pending = g_sd_pending;

        char buf[28];

        if (sd_free >= 0)
            snprintf(buf, sizeof(buf), "Free:    %.1f GB", sd_free);
        else
            snprintf(buf, sizeof(buf), "Free:    ---");
        draw_string_padded(4, 120, buf, 1, COL_WHITE, COL_BLACK, 20);

        if (sd_pending >= 0)
            snprintf(buf, sizeof(buf), "Pending: %d", sd_pending);
        else
            snprintf(buf, sizeof(buf), "Pending: ---");
        draw_string_padded(4, 145, buf, 1, COL_WHITE, COL_BLACK, 20);

        snprintf(buf, sizeof(buf), "Done:    %u", (unsigned)s.done_count);
        draw_string_padded(4, 170, buf, 1, COL_WHITE, COL_BLACK, 20);
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t lcd_ui_init(void)
{
    g_mutex = xSemaphoreCreateMutex();
    if (!g_mutex) return ESP_ERR_NO_MEM;

    memset(&g_state, 0, sizeof(g_state));

    /* Backlight GPIO — active high */
    gpio_config_t bl_cfg = {
        .pin_bit_mask = (1ULL << LCD_BL),
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    gpio_set_level(LCD_BL, 0);  /* GPIO48 is active-low — 0 = backlight ON */

    /* SPI bus */
    spi_bus_config_t buscfg = {
        .mosi_io_num   = LCD_MOSI,
        .miso_io_num   = -1,
        .sclk_io_num   = LCD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* LCD IO handle (SPI panel IO) */
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = LCD_DC,
        .cs_gpio_num       = LCD_CS,
        .pclk_hz           = LCD_CLK_HZ,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                              &io_cfg, &io_handle));

    /* ST7789V panel */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &g_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(g_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(g_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(g_panel, true));  /* ST7789V needs inversion */
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(g_panel, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(g_panel, true));

    xTaskCreate(refresh_task,  "lcd_ui",    4096, NULL, 3, NULL);
    xTaskCreate(sd_stats_task, "lcd_sd",    2048, NULL, 2, NULL);

    ESP_LOGI(TAG, "ST7789V ready");
    return ESP_OK;
}

void lcd_ui_set_screen_on(bool on)
{
    if (on && !g_screen_on) {
        /* Ask refresh_task to do a full clear on next cycle before drawing.
         * Never call fill_rect() here — only refresh_task owns the SPI bus. */
        g_needs_clear = true;
    }
    g_screen_on = on;
    gpio_set_level(LCD_BL, on ? 0 : 1);  /* GPIO48 is active-low */
    ESP_LOGI(TAG, "Screen %s", on ? "ON" : "OFF");
}

bool lcd_ui_get_screen_on(void)
{
    return g_screen_on;
}

void lcd_ui_notify_recording(bool recording, uint32_t elapsed_s)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_state.recording = recording;
    g_state.elapsed_s = elapsed_s;
    xSemaphoreGive(g_mutex);
}

void lcd_ui_notify_uploading(bool uploading, const char *clip_name)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_state.uploading = uploading;
    if (clip_name) {
        strlcpy(g_state.clip_name, clip_name, sizeof(g_state.clip_name));
    } else {
        g_state.clip_name[0] = '\0';
    }
    xSemaphoreGive(g_mutex);
}

void lcd_ui_inc_uploaded(void)
{
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_state.done_count++;
    xSemaphoreGive(g_mutex);
}
