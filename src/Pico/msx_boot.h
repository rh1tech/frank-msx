/*
 * msx_boot.h — boot-time splash screen and fatal-error screens.
 *
 * All of these run BEFORE InitMachine() is called, so they can't rely
 * on fMSX's palette or refresh timing. They draw directly into the
 * HDMI framebuffer (SCREEN[0/1]) and pace themselves off time_us_64().
 *
 * Each screen either returns when the user presses a key / waits long
 * enough (welcome) or runs forever (fatal error — the user reboots).
 */
#ifndef MSX_BOOT_H
#define MSX_BOOT_H

#include <stdbool.h>
#include <stdint.h>

/* Show the "FRANK MSX" starfield welcome. Returns when:
 *   - user presses any PS/2 / USB key or any gamepad button, OR
 *   - approximately `timeout_ms` milliseconds have elapsed.
 */
void msx_boot_welcome(uint32_t timeout_ms);

/* Reasons the emulator can't come up. All of them paint a full-screen
 * notice and loop forever — user has to power-cycle after fixing
 * whatever's missing. */
typedef enum {
    MSX_BOOT_ERR_NO_SD,        /* f_mount failed */
    MSX_BOOT_ERR_NO_DIR,       /* /MSX dir missing */
    MSX_BOOT_ERR_NO_BIOS,      /* required BIOS ROM(s) missing */
} msx_boot_err_t;

/* Never returns — paints the given error and spins. The `detail` line
 * is an optional second body line (e.g. the filename that's missing);
 * may be NULL. */
_Noreturn void msx_boot_fatal(msx_boot_err_t reason, const char *detail);

/* Check whether the SD card is mounted and /MSX + the BIOS required
 * for the selected model are present. On failure this calls
 * msx_boot_fatal() with the right reason. On success returns normally.
 *
 * `sd_mounted` = FatFS f_mount already succeeded.
 * `model`      = 1 (MSX1) / 2 (MSX2) / 3 (MSX2+).
 */
void msx_boot_require_bios(bool sd_mounted, int model);

#endif /* MSX_BOOT_H */
