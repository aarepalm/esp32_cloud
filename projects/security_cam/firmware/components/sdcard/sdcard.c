/*
 * sdcard.c — SD card mount/unmount/format
 *
 * ESP32-S3-EYE v2.2 wires the SD card to the SDMMC peripheral in 1-bit mode.
 * (SPI2 on the board is used for the LCD display, not the SD card.)
 *
 * Confirmed pin assignments from factory firmware + schematic:
 *   CLK  → GPIO39
 *   CMD  → GPIO38
 *   D0   → GPIO40
 *   CD   → not connected (no card-detect pin)
 *   WP   → not connected
 */

#include "sdcard.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#include <sys/stat.h>

static const char *TAG = "sdcard";

#define MOUNT_POINT "/sdcard"

/* ESP32-S3-EYE SD card pins (SDMMC 1-bit mode) */
#define SD_PIN_CLK  GPIO_NUM_39
#define SD_PIN_CMD  GPIO_NUM_38
#define SD_PIN_D0   GPIO_NUM_40

static sdmmc_card_t *s_card = NULL;

/* Fill in host and slot structs — used by both mount and format */
static void sdcard_get_hw_config(sdmmc_host_t *host, sdmmc_slot_config_t *slot)
{
    *host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    *slot = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
    slot->clk   = SD_PIN_CLK;
    slot->cmd   = SD_PIN_CMD;
    slot->d0    = SD_PIN_D0;
    slot->width = 1;
    slot->flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
}

static esp_err_t sdcard_mount(bool format_if_needed)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = format_if_needed,
        .max_files              = 8,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    sdcard_get_hw_config(&host, &slot);

    ESP_LOGI(TAG, "Mounting SD card (SDMMC 1-bit: CLK=%d CMD=%d D0=%d)%s",
             SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0,
             format_if_needed ? " [format-on-fail]" : "");

    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot,
                                             &mount_cfg, &s_card);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem — card not FAT32? Use 'format' in boot console.");
        } else {
            ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(err));
        }
        return err;
    }

    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

esp_err_t sdcard_init(void)
{
    if (s_card) {
        ESP_LOGW(TAG, "Already mounted");
        return ESP_OK;
    }

    esp_err_t err = sdcard_mount(false);
    if (err != ESP_OK) {
        return err;
    }

    /* Quick write test */
    FILE *f = fopen(MOUNT_POINT "/cam_test.txt", "w");
    if (f) {
        fprintf(f, "security_cam SD write test OK\n");
        fclose(f);
        ESP_LOGI(TAG, "SD card mounted and write test OK");
    } else {
        ESP_LOGW(TAG, "Mount OK but write test failed — card full or write-protected?");
    }

    return ESP_OK;
}

esp_err_t sdcard_deinit(void)
{
    if (!s_card) {
        return ESP_OK;
    }
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    s_card = NULL;
    ESP_LOGI(TAG, "SD card unmounted");
    return ESP_OK;
}

esp_err_t sdcard_format(void)
{
    /* Must be mounted to hold the card handle for f_mkfs */
    if (!s_card) {
        esp_err_t err = sdcard_mount(false);
        if (err != ESP_OK) return err;
    }

    ESP_LOGI(TAG, "Formatting SD card as FAT32…");
    /* esp_vfs_fat_sdcard_format() calls f_mkfs() directly — works even when
     * the filesystem is already valid (unlike format_if_mount_failed which
     * only triggers on mount failure). */
    esp_err_t err = esp_vfs_fat_sdcard_format(MOUNT_POINT, s_card);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Format complete");
    } else {
        ESP_LOGE(TAG, "Format failed: %s", esp_err_to_name(err));
    }
    return err;
}
