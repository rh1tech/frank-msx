/*
 * frank-msx — fMSX for RP2350
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * msx_flashrom.c — cartridge ROMs in on-board flash when there is no PSRAM.
 *
 * Flash map (4 MB Pico 2 flash):
 *   0x000000 .. firmware (must stay below FLASHROM_BASE)
 *   0x100000 .. 0x1FFFFF  cartridge slot A  (1 MB)
 *   0x200000 .. 0x2FFFFF  cartridge slot B  (1 MB)
 *   0x300000 .. 0x37FFFF  internal slots 2..5 (128 KB each: MSX-DOS2,
 *                         PAINTER, FMPAC, GMASTER)
 *   0x3BF000 .. 0x3FFFFF  pico-launcher (linked at the top of the 16 MB
 *                         XIP window, i.e. the last 260 KB of the flash)
 */
#include "msx_flashrom.h"

#include <string.h>

#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/structs/qmi.h"

#include "psram_allocator.h"

#define FLASHROM_BASE      0x100000u
#define FLASHROM_SLOTS     6

static const uint32_t slot_off[FLASHROM_SLOTS] = {
    0x100000u, 0x200000u, 0x300000u, 0x320000u, 0x340000u, 0x360000u
};
static const uint32_t slot_len[FLASHROM_SLOTS] = {
    0x100000u, 0x100000u, 0x020000u, 0x020000u, 0x020000u, 0x020000u
};
#define FLASHROM_END       0x380000u

extern char __flash_binary_end;

static uint8_t sector_buf[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));

typedef struct {
    uint32_t       off;
    const uint8_t *data;
} flash_job_t;

static void __no_inline_not_in_flash_func(flash_job)(void *param) {
    const flash_job_t *j = (const flash_job_t *)param;
    /* flash_range_erase/program leave XIP through the boot-time XIP setup,
     * which restores the QMI window-0 timing chosen for the boot clock.
     * main.c overclocks clk_sys and retunes that timing (set_flash_timings),
     * so the boot value would run the flash far too fast and every XIP
     * fetch afterwards would return garbage. Put our timing back. */
    uint32_t timing = qmi_hw->m[0].timing;
    uint32_t rfmt   = qmi_hw->m[0].rfmt;
    uint32_t rcmd   = qmi_hw->m[0].rcmd;
    flash_range_erase(j->off, FLASH_SECTOR_SIZE);
    flash_range_program(j->off, j->data, FLASH_SECTOR_SIZE);
    qmi_hw->m[0].timing = timing;
    qmi_hw->m[0].rfmt   = rfmt;
    qmi_hw->m[0].rcmd   = rcmd;
    __asm volatile ("dsb; isb" ::: "memory");
}

/* Erase + program one 4 KB sector, unless it already holds `data`. */
static bool write_sector(uint32_t off, const uint8_t *data) {
    const uint8_t *xip = (const uint8_t *)(XIP_BASE + off);
    if (memcmp(xip, data, FLASH_SECTOR_SIZE) == 0) return true;

    flash_job_t j = { off, data };
    /* flash_safe_execute() parks the other core when it has been set up
     * as a lockout victim; with no second core running it just disables
     * interrupts. Fall back to plain interrupt masking if it refuses. */
    if (flash_safe_execute(flash_job, &j, 1000) != PICO_OK) {
        uint32_t ints = save_and_disable_interrupts();
        flash_job(&j);
        restore_interrupts(ints);
    }
    return memcmp(xip, data, FLASH_SECTOR_SIZE) == 0;
}

bool flashrom_enabled(void) {
    if (psram_present()) return false;
    /* The firmware itself must end below the cartridge windows. */
    return ((uintptr_t)&__flash_binary_end - XIP_BASE) <= FLASHROM_BASE;
}

uint8_t *flashrom_slot_ptr(int slot, int size) {
    if (slot < 0 || slot >= FLASHROM_SLOTS || size <= 0) return NULL;
    if ((uint32_t)size > slot_len[slot]) {
        printf("flashrom: %d kB does not fit slot %d (%lu kB)\n",
               size / 1024, slot, (unsigned long)(slot_len[slot] / 1024));
        return NULL;
    }
    return (uint8_t *)(XIP_BASE + slot_off[slot]);
}

bool flashrom_is_flash(const void *p) {
    uintptr_t a = (uintptr_t)p;
    return a >= XIP_BASE + FLASHROM_BASE && a < XIP_BASE + FLASHROM_END;
}

int flashrom_load(FILE *f, uint8_t *dst, int size) {
    if (!flashrom_is_flash(dst)) return 0;
    uint32_t off = (uintptr_t)dst - XIP_BASE;
    if (off % FLASH_SECTOR_SIZE) return 0;

    int done = 0;
    while (done < size) {
        int want = size - done;
        if (want > FLASH_SECTOR_SIZE) want = FLASH_SECTOR_SIZE;
        int got = (int)fread(sector_buf, 1, (size_t)want, f);
        if (got <= 0) break;
        if (got < FLASH_SECTOR_SIZE)
            memset(sector_buf + got, 0xFF, FLASH_SECTOR_SIZE - got);
        if (!write_sector(off, sector_buf)) {
            printf("flashrom: write failed at 0x%06lx\n", (unsigned long)off);
            break;
        }
        off  += FLASH_SECTOR_SIZE;
        done += got;
        if (got < want) break;
    }
    return done;
}

bool flashrom_copy(uint8_t *dst, const uint8_t *src, int len) {
    if (!flashrom_is_flash(dst) || !flashrom_is_flash(src)) return false;
    uint32_t off = (uintptr_t)dst - XIP_BASE;
    if ((off % FLASH_SECTOR_SIZE) || (len % FLASH_SECTOR_SIZE)) return false;

    for (int done = 0; done < len; done += FLASH_SECTOR_SIZE) {
        memcpy(sector_buf, src + done, FLASH_SECTOR_SIZE);
        if (!write_sector(off + done, sector_buf)) return false;
    }
    return true;
}
