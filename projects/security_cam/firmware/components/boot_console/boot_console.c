/*
 * boot_console.c — Interactive boot-time console with auto-continue timeout
 *
 * Root cause of previous failures
 * ────────────────────────────────
 * ESP-IDF's USB Serial/JTAG VFS defaults to NON-BLOCKING reads
 * (usb_serial_jtag_vfs_use_nonblocking — fgetc() returns EOF immediately).
 * To get blocking reads we must:
 *   1. Install the USB Serial/JTAG driver (interrupt-driven ring buffers)
 *   2. Call usb_serial_jtag_vfs_use_driver() to switch VFS to blocking mode
 *
 * Timing fix
 * ──────────
 * After a flash-reset, USB re-enumerates on the host (~1-2 s).  We wait
 * for usb_serial_jtag_is_connected() before starting the countdown, so the
 * user always gets a full 5-second window after the banner appears.
 *
 * Menu commands (type then Enter)
 * ────────────────────────────────
 *   info      — chip, cores, RAM, flash, free heap
 *   ls        — list files on /sdcard
 *   rm <name> — delete /sdcard/<name>
 *   format    — FAT32-format the SD card (type YES)
 *   nvs       — erase NVS (type YES)
 *   boot      — exit console, continue boot
 *   ?/help    — this list
 */

#include "boot_console.h"
#include "sdcard.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

static const char *TAG = "boot_console";

#define LINE_BUF_LEN    128
#define MOUNT_POINT     "/sdcard"

/* Queue of characters from the blocking reader task */
static QueueHandle_t s_char_q;

/* ── USB driver setup ─────────────────────────────────────────────────────── */

static void setup_usb_driver(void)
{
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        cfg.rx_buffer_size = 512;
        cfg.tx_buffer_size = 512;
        ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    }
    /* Switch VFS from non-blocking (default) to interrupt-driven blocking mode */
    usb_serial_jtag_vfs_use_driver();
}

/* ── reader task ──────────────────────────────────────────────────────────── */

static void stdin_reader_task(void *arg)
{
    /* Use usb_serial_jtag_read_bytes() directly — bypasses VFS entirely.
     * This is the most reliable path: directly reads the driver ring buffer. */
    uint8_t ch;
    while (1) {
        int n = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(100));
        if (n > 0) {
            int c = ch;
            xQueueSend(s_char_q, &c, 0);
        }
    }
}

/* ── line input ───────────────────────────────────────────────────────────── */

static int read_char(TickType_t ticks)
{
    int c;
    return (xQueueReceive(s_char_q, &c, ticks) == pdTRUE) ? c : -1;
}

static const char *s_commands[] = {
    "boot", "format", "help", "info", "ls", "nvs", "rm", NULL
};

static void tab_complete(char *buf, size_t *pos, size_t len)
{
    /* Find all commands that start with the current buffer contents */
    const char *matches[8];
    int n = 0;
    for (int i = 0; s_commands[i]; i++) {
        if (strncmp(buf, s_commands[i], *pos) == 0) {
            matches[n++] = s_commands[i];
        }
    }
    if (n == 0) return;   /* no match — do nothing */

    if (n == 1) {
        /* Unique match: complete it */
        size_t cmd_len = strlen(matches[0]);
        /* Print the missing suffix */
        for (size_t i = *pos; i < cmd_len && i < len - 1; i++) {
            buf[i] = matches[0][i];
            putchar(matches[0][i]);
        }
        *pos = cmd_len < len - 1 ? cmd_len : len - 1;
        buf[*pos] = '\0';
        fflush(stdout);
    } else {
        /* Multiple matches: show them, reprint prompt + current input */
        printf("\r\n");
        for (int i = 0; i < n; i++) printf("  %s\r\n", matches[i]);
        printf("cam> ");
        buf[*pos] = '\0';
        printf("%s", buf);
        fflush(stdout);
    }
}

static int read_line(char *buf, size_t len)
{
    size_t pos = 0;
    while (1) {
        int c = read_char(portMAX_DELAY);
        if (c < 0) continue;
        if (c == '\n' || c == '\r') {
            printf("\r\n"); fflush(stdout);
            break;
        }
        if ((c == 8 || c == 127) && pos > 0) {
            pos--;
            printf("\b \b"); fflush(stdout);
            continue;
        }
        if (c == '\t') {
            buf[pos] = '\0';
            tab_complete(buf, &pos, len);
            continue;
        }
        if (c >= 0x20 && pos < len - 1) {
            buf[pos++] = (char)c;
            putchar(c); fflush(stdout);
        }
    }
    buf[pos] = '\0';
    return (int)pos;
}

/* ── commands ─────────────────────────────────────────────────────────────── */

static void cmd_help(void)
{
    printf("\n"
           "  info          chip model, cores, RAM, flash, free heap\n"
           "  ls            list files on SD card\n"
           "  rm <name>     delete /sdcard/<name>\n"
           "  format        FAT32-format the SD card\n"
           "  nvs           erase NVS partition\n"
           "  boot          exit console, continue normal boot\n"
           "  ?/help        this help\n"
           "\n");
}

static void cmd_info(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const char *model = "unknown";
    switch (chip.model) {
        case CHIP_ESP32S3: model = "ESP32-S3"; break;
        case CHIP_ESP32:   model = "ESP32";    break;
        case CHIP_ESP32S2: model = "ESP32-S2"; break;
        case CHIP_ESP32C3: model = "ESP32-C3"; break;
        default: break;
    }
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    printf("\n"
           "  Chip:          %s rev %d, %d core(s)\n"
           "  Flash:         %lu MB\n"
           "  Free internal: %u KB\n"
           "  Free PSRAM:    %u KB\n"
           "\n",
           model, chip.revision, chip.cores,
           (unsigned long)(flash_size >> 20),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)   >> 10));
}

static bool ensure_sd_mounted(void)
{
    struct stat st;
    if (stat(MOUNT_POINT, &st) == 0) return true;
    printf("  Mounting SD card… "); fflush(stdout);
    esp_err_t err = sdcard_init();
    if (err == ESP_OK) { printf("OK\n"); return true; }
    printf("FAILED (%s)\n", esp_err_to_name(err));
    return false;
}

static void cmd_ls(void)
{
    if (!ensure_sd_mounted()) return;
    DIR *d = opendir(MOUNT_POINT);
    if (!d) { printf("  opendir failed: %s\n", strerror(errno)); return; }
    printf("\n  %-40s  %10s\n", "Name", "Size (B)");
    printf("  %-40s  %10s\n", "----------------------------------------", "----------");
    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(d)) != NULL) {
        char path[272];
        snprintf(path, sizeof(path), MOUNT_POINT "/%s", ent->d_name);
        struct stat st;
        long size = 0;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) size = (long)st.st_size;
        printf("  %-40s  %10ld\n", ent->d_name, size);
        count++;
    }
    closedir(d);
    if (count == 0) printf("  (empty)\n");
    printf("\n");
}

static void cmd_rm(const char *filename)
{
    if (!filename || !*filename) { printf("  Usage: rm <filename>\n"); return; }
    if (!ensure_sd_mounted()) return;
    char path[272];
    snprintf(path, sizeof(path), MOUNT_POINT "/%s", filename);
    struct stat st;
    if (stat(path, &st) != 0) { printf("  File not found: %s\n", path); return; }
    printf("  Delete %s (%ld bytes)? [y/N] ", path, (long)st.st_size); fflush(stdout);
    char confirm[8]; read_line(confirm, sizeof(confirm));
    if (confirm[0] != 'y' && confirm[0] != 'Y') { printf("  Cancelled.\n"); return; }
    printf("%s\n", unlink(path) == 0 ? "  Deleted." : "  Failed.");
}

static void cmd_format(void)
{
    printf("\n  WARNING: This will erase ALL data on the SD card!\n"
           "  Type YES (uppercase) to confirm: "); fflush(stdout);
    char confirm[8]; read_line(confirm, sizeof(confirm));
    if (strcmp(confirm, "YES") != 0) { printf("  Cancelled.\n"); return; }
    printf("  Formatting… "); fflush(stdout);
    esp_err_t err = sdcard_format();
    if (err == ESP_OK) {
        printf("done.\n");
        ESP_LOGI(TAG, "SD card formatted successfully.");
    } else {
        printf("FAILED (%s)\n", esp_err_to_name(err));
    }
}

static void cmd_nvs_erase(void)
{
    printf("\n  WARNING: This will erase all NVS data (WiFi creds etc.)!\n"
           "  Type YES (uppercase) to confirm: "); fflush(stdout);
    char confirm[8]; read_line(confirm, sizeof(confirm));
    if (strcmp(confirm, "YES") != 0) { printf("  Cancelled.\n"); return; }
    esp_err_t err = nvs_flash_erase();
    if (err == ESP_OK) { printf("  NVS erased.\n"); ESP_LOGI(TAG, "NVS erased by user."); }
    else               { printf("  Failed: %s\n", esp_err_to_name(err)); }
}

/* ── main entry point ─────────────────────────────────────────────────────── */

void boot_console_run(void)
{
    /* Step 1: Install USB JTAG driver and switch VFS to blocking mode */
    setup_usb_driver();

    /* Step 2: Spin up the reader task (uses usb_serial_jtag_read_bytes directly) */
    s_char_q = xQueueCreate(32, sizeof(int));
    xTaskCreate(stdin_reader_task, "con_rd", 2048, NULL, 3, NULL);

    /* Step 3: Wait for a keypress for up to 30 seconds.
     * The banner is reprinted every 5 seconds so a user who opens the
     * terminal mid-countdown still sees it clearly.
     * usb_serial_jtag_is_connected() returning true ≠ terminal is open,
     * so we don't gate on that — we just keep the window open long enough. */
#define TOTAL_WAIT_S     5
#define BANNER_EVERY_S   5

    static const char BANNER[] =
        "\n"
        "=============================================\n"
        "       security_cam boot console\n"
        "=============================================\n"
        "  Press Enter for interactive console\n"
        "  (auto-boot in %d s if no key pressed)\n\n";

    bool key_pressed = false;
    int seconds_waited = 0;

    while (seconds_waited < TOTAL_WAIT_S) {
        /* Print banner at t=0 and every BANNER_EVERY_S seconds */
        if (seconds_waited % BANNER_EVERY_S == 0) {
            printf(BANNER, TOTAL_WAIT_S - seconds_waited);
            fflush(stdout);
        }

        int c = read_char(pdMS_TO_TICKS(1000));
        if (c >= 0) { key_pressed = true; break; }
        seconds_waited++;
    }

    if (!key_pressed) {
        printf("\r  Timeout — continuing boot.\n\n");
        return;
    }

    printf("\r  Console active. Type 'help' for commands, 'boot' to continue.\n\n");

    /* Step 6: Interactive loop */
    char line[LINE_BUF_LEN];
    while (1) {
        printf("cam> "); fflush(stdout);
        if (read_line(line, sizeof(line)) == 0) {
            printf("  (type 'help' for commands, 'boot' to continue)\n");
            continue;
        }
        char *cmd = line, *args = NULL;
        char *sp = strchr(line, ' ');
        if (sp) { *sp = '\0'; args = sp + 1; while (*args == ' ') args++; }

        if (!strcmp(cmd,"boot")||!strcmp(cmd,"q")||!strcmp(cmd,"quit")||!strcmp(cmd,"exit")) {
            printf("  Continuing boot…\n\n"); break;
        } else if (!strcmp(cmd,"help") || !strcmp(cmd,"?")) { cmd_help();
        } else if (!strcmp(cmd,"info"))                     { cmd_info();
        } else if (!strcmp(cmd,"ls") || !strcmp(cmd,"dir")) { cmd_ls();
        } else if (!strcmp(cmd,"rm") || !strcmp(cmd,"del")) { cmd_rm(args);
        } else if (!strcmp(cmd,"format"))                   { cmd_format();
        } else if (!strcmp(cmd,"nvs"))                      { cmd_nvs_erase();
        } else { printf("  Unknown command '%s'. Type 'help'.\n", cmd); }
    }
}
