/*
 * frank-msx — fMSX for RP2350
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * msx_flashrom.h — cartridge ROMs in on-board flash when there is no PSRAM.
 *
 * Without PSRAM the SRAM heap holds only the MSX1 machine (RAM, VRAM,
 * BIOS). Cartridge images are written into a reserved part of the flash
 * instead and the emulator reads them straight from XIP. Each cartridge
 * slot has its own fixed window; a sector is erased and programmed only
 * when its contents differ, so loading the same cartridge again does not
 * wear the flash.
 */
#ifndef MSX_FLASHROM_H
#define MSX_FLASHROM_H

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when cartridges must go to flash (no PSRAM, flash window usable). */
bool flashrom_enabled(void);

/* XIP address of the flash window for cartridge slot `slot`, or NULL when
 * the slot has no window or `size` does not fit. */
uint8_t *flashrom_slot_ptr(int slot, int size);

/* True when `p` points into the cartridge flash windows. */
bool flashrom_is_flash(const void *p);

/* Copy `size` bytes from the open file into flash at `dst`.
 * Returns the number of bytes stored (== size on success). */
int flashrom_load(FILE *f, uint8_t *dst, int size);

/* Copy `len` bytes inside the flash windows (cartridge mirroring). */
bool flashrom_copy(uint8_t *dst, const uint8_t *src, int len);

#ifdef __cplusplus
}
#endif

#endif /* MSX_FLASHROM_H */
