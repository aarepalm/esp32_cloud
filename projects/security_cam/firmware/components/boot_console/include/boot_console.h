/*
 * boot_console.h — Interactive boot-time console with 3-second auto-continue
 *
 * Call boot_console_run() immediately after NVS init and before any hardware
 * init.  It prints a prompt, waits up to 3 seconds for Enter, then either:
 *   • returns immediately (user did nothing → normal boot continues), or
 *   • enters an interactive menu loop until the user types "boot".
 *
 * The SD card does NOT need to be mounted when boot_console_run() is called.
 * Commands that touch the SD card will mount it on demand.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Run the boot console.
 *         Blocks for up to 3 s waiting for a keypress.
 *         If Enter (or any key) is received within the timeout, enters the
 *         interactive menu.  Otherwise returns immediately.
 */
void boot_console_run(void);

#ifdef __cplusplus
}
#endif
