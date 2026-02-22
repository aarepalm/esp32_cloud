/*
 * lcd_ui.h — Status screen driver for ESP32-S3-EYE ST7789V LCD.
 *
 * Call lcd_ui_init() once after sdcard_init().
 * All other calls are thread-safe (protected by a mutex).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise SPI bus, ST7789V panel, backlight GPIO, and start the
 *        refresh task. Must be called after the SD card is mounted.
 */
esp_err_t lcd_ui_init(void);

/**
 * @brief Turn the screen (backlight) on or off.
 */
void lcd_ui_set_screen_on(bool on);

/**
 * @brief Query current backlight state.
 */
bool lcd_ui_get_screen_on(void);

/**
 * @brief Notify UI of recording state change.
 * @param recording  true = camera is recording a clip
 * @param elapsed_s  seconds elapsed in current clip (ignored when recording=false)
 */
void lcd_ui_notify_recording(bool recording, uint32_t elapsed_s);

/**
 * @brief Notify UI of upload state.
 * @param uploading  true = upload in progress
 * @param clip_name  base name being uploaded (may be NULL when uploading=false)
 */
void lcd_ui_notify_uploading(bool uploading, const char *clip_name);

/**
 * @brief Increment the session "Done" counter (called after each successful upload).
 */
void lcd_ui_inc_uploaded(void);

#ifdef __cplusplus
}
#endif
