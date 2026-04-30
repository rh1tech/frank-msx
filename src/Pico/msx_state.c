/*
 * msx_state.c — PSRAM-backed save-state slots.
 *
 * fMSX ships SaveSTA/LoadSTA in State.h, but they malloc() the full
 * MAX_STASIZE buffer (≥ 272 KB for MSX2+/512 kB/512 kB) which blows
 * our SRAM heap. We reimplement them using psram_malloc + SaveState /
 * LoadState + FatFS, keeping the on-disk format byte-compatible with
 * upstream (STE\x1a\x03<ram><vram><id_lo><id_hi>\0\0\0\0\0\0\0\0 +
 * raw LoadState/SaveState buffer).
 */

#include "msx_state.h"
#include "MSX.h"
#include "ff.h"
#include "psram_allocator.h"

#include <stdio.h>
#include <string.h>

extern unsigned int SaveState(unsigned char *Buf, unsigned int MaxSize);
extern unsigned int LoadState(unsigned char *Buf, unsigned int MaxSize);
extern word         StateID(void);

#define STATE_DIR "/MSX/STATE"

static void slot_path(int slot, char *buf, size_t n) {
    snprintf(buf, n, STATE_DIR "/SLOT%d.STA", slot);
}

static void ensure_dir(void) {
    /* f_mkdir returns FR_EXIST harmlessly if the directory already
     * exists. Any other error is surfaced at save time. */
    f_mkdir(STATE_DIR);
}

bool msx_state_slot_exists(int slot) {
    if (slot < 0 || slot >= MSX_STATE_SLOTS) return false;
    char path[48];
    slot_path(slot, path, sizeof(path));
    FILINFO fno;
    return f_stat(path, &fno) == FR_OK;
}

int msx_state_save(int slot) {
    if (slot < 0 || slot >= MSX_STATE_SLOTS) return -1;

    size_t sz = 0x8000 + (size_t)RAMPages * 0x4000 + (size_t)VRAMPages * 0x4000;
    unsigned char *buf = (unsigned char *)psram_malloc(sz);
    if (!buf) return -2;

    unsigned int used = SaveState(buf, (unsigned int)sz);
    if (!used) { psram_free(buf); return -3; }

    ensure_dir();

    char path[48];
    slot_path(slot, path, sizeof(path));
    FIL f;
    FRESULT fr = f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) { psram_free(buf); return -4; }

    /* Header matches upstream SaveSTA so a .STA dropped onto a PC
     * fMSX build still loads. */
    unsigned char hdr[16] = "STE\x1a\x03";
    hdr[5] = (unsigned char)(RAMPages  & 0xFF);
    hdr[6] = (unsigned char)(VRAMPages & 0xFF);
    unsigned int id = StateID();
    hdr[7] = (unsigned char)(id & 0xFF);
    hdr[8] = (unsigned char)((id >> 8) & 0xFF);

    UINT bw = 0;
    int ok = 1;
    if (f_write(&f, hdr, 16, &bw) != FR_OK || bw != 16)       ok = 0;
    if (ok && (f_write(&f, buf, used, &bw) != FR_OK || bw != used)) ok = 0;
    f_close(&f);
    psram_free(buf);

    if (!ok) { f_unlink(path); return -5; }
    printf("state: saved slot %d (%u bytes) -> %s\n", slot, used, path);
    return 0;
}

int msx_state_load(int slot) {
    if (slot < 0 || slot >= MSX_STATE_SLOTS) return -1;

    char path[48];
    slot_path(slot, path, sizeof(path));
    FIL f;
    FRESULT fr = f_open(&f, path, FA_READ);
    if (fr != FR_OK) return -2;

    unsigned char hdr[16];
    UINT br = 0;
    if (f_read(&f, hdr, 16, &br) != FR_OK || br != 16) {
        f_close(&f); return -3;
    }
    if (memcmp(hdr, "STE\x1a\x03", 5) != 0)     { f_close(&f); return -4; }
    unsigned int id = StateID();
    if ((hdr[7] | (hdr[8] << 8)) != id)         { f_close(&f); return -5; }
    if (hdr[5] != (RAMPages & 0xFF) ||
        hdr[6] != (VRAMPages & 0xFF))           { f_close(&f); return -6; }

    size_t sz = 0x8000 + (size_t)RAMPages * 0x4000 + (size_t)VRAMPages * 0x4000;
    unsigned char *buf = (unsigned char *)psram_malloc(sz);
    if (!buf) { f_close(&f); return -7; }

    br = 0;
    if (f_read(&f, buf, sz, &br) != FR_OK || br == 0) {
        f_close(&f); psram_free(buf); return -8;
    }
    f_close(&f);

    unsigned int used = LoadState(buf, br);
    psram_free(buf);

    if (!used) {
        printf("state: load slot %d rejected (ResetMSX)\n", slot);
        ResetMSX(Mode, RAMPages, VRAMPages);
        return -9;
    }
    printf("state: loaded slot %d (%u bytes)\n", slot, used);
    return 0;
}

int msx_state_delete(int slot) {
    if (slot < 0 || slot >= MSX_STATE_SLOTS) return -1;

    char path[48];
    slot_path(slot, path, sizeof(path));

    FRESULT fr = f_unlink(path);
    /* FR_NO_FILE / FR_NO_PATH means the slot was already empty — treat
     * as success so the UI doesn't have to special-case it. */
    if (fr == FR_OK || fr == FR_NO_FILE || fr == FR_NO_PATH) {
        printf("state: deleted slot %d (%s)\n", slot,
               fr == FR_OK ? "removed" : "already empty");
        return 0;
    }
    printf("state: delete slot %d failed (FRESULT=%d)\n", slot, fr);
    return -2;
}
