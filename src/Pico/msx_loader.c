/*
 * msx_loader.c — scan the SD card for MSX ROMs / DSKs and hand them
 *                to the fMSX core via LoadCart() / ChangeDisk().
 */

#include "msx_loader.h"
#include "MSX.h"
#include "ff.h"
#include "psram_allocator.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>

msx_entry_t *msx_entries    = NULL;    /* lazily allocated in PSRAM */
int          msx_entry_count = 0;
char         msx_current_dir[MSX_MAX_PATH_LEN] = "/MSX";

static void ensure_entry_table(void) {
    if (msx_entries) return;
    msx_entries = (msx_entry_t *)psram_malloc(
        sizeof(msx_entry_t) * MSX_MAX_ENTRIES);
}

static int ext_ieq(const char *ext, const char *want) {
    while (*ext && *want) {
        if (tolower((unsigned char)*ext) != tolower((unsigned char)*want)) return 0;
        ++ext; ++want;
    }
    return *ext == 0 && *want == 0;
}

/* Well-known fMSX / system ROM filenames — never show these in the
 * loader, they're BIOS blobs, not user cartridges. Match is full-name
 * case-insensitive. */
static const char *SYSTEM_ROMS[] = {
    "MSX.ROM",
    "MSX2.ROM",       "MSX2EXT.ROM",
    "MSX2P.ROM",      "MSX2PEXT.ROM",
    "DISK.ROM",
    "KANJI.ROM",
    "RS232.ROM",
    "FMPAC.ROM",
    "FMPAC16.ROM",
    "PAINTER.ROM",
    "MSXDOS2.ROM",
    "GMASTER.ROM",    "GMASTER2.ROM",
    "CMOS.ROM",
    "CARTS.SHA",
    "DEFAULT.FNT",
    "DEFAULT.STA",
    NULL,
};

static int name_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

static int is_system_rom(const char *name) {
    for (const char **p = SYSTEM_ROMS; *p; ++p)
        if (name_ieq(name, *p)) return 1;
    return 0;
}

msx_entry_kind_t msx_classify(const char *name) {
    if (is_system_rom(name)) return MSX_ENTRY_UNKNOWN;

    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return MSX_ENTRY_UNKNOWN;
    const char *e = dot + 1;
    if (ext_ieq(e, "rom") || ext_ieq(e, "mx1") || ext_ieq(e, "mx2")
        || ext_ieq(e, "col") || ext_ieq(e, "ri")) return MSX_ENTRY_ROM;
    if (ext_ieq(e, "dsk") || ext_ieq(e, "dsr")) return MSX_ENTRY_DISK;
    return MSX_ENTRY_UNKNOWN;
}

static int cmp_entry(const void *pa, const void *pb) {
    const msx_entry_t *a = (const msx_entry_t *)pa;
    const msx_entry_t *b = (const msx_entry_t *)pb;
    /* dirs first, then files, then alphabetical */
    if (a->kind == MSX_ENTRY_DIR && b->kind != MSX_ENTRY_DIR) return -1;
    if (a->kind != MSX_ENTRY_DIR && b->kind == MSX_ENTRY_DIR) return  1;
    return strcasecmp(a->name, b->name);
}

int msx_rescan(void) {
    DIR dir;
    FILINFO fno;

    ensure_entry_table();
    if (!msx_entries) return 0;

    msx_entry_count = 0;

    FRESULT fr = f_opendir(&dir, msx_current_dir);
    if (fr != FR_OK) {
        /* Fallback: scan root if the configured dir is missing. */
        strcpy(msx_current_dir, "/");
        fr = f_opendir(&dir, msx_current_dir);
        if (fr != FR_OK) return 0;
    }

    while (msx_entry_count < MSX_MAX_ENTRIES) {
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK || fno.fname[0] == 0) break;
        if (fno.fname[0] == '.') continue;   /* hide dotfiles + "."/".." */

        msx_entry_kind_t kind;
        if (fno.fattrib & AM_DIR) {
            kind = MSX_ENTRY_DIR;
        } else {
            kind = msx_classify(fno.fname);
            if (kind == MSX_ENTRY_UNKNOWN) continue;
        }

        size_t n = strlen(fno.fname);
        /* Skip entries that wouldn't fit in our buffer. FatFS hands us
         * the full LFN (up to FF_MAX_LFN = 255); we'd rather hide an
         * oversized file than mount a truncated path and fail. */
        if (n >= MSX_MAX_FILENAME_LEN) {
            printf("msx_loader: skipped oversized name (%u) '%s'\n",
                   (unsigned)n, fno.fname);
            continue;
        }

        msx_entry_t *e = &msx_entries[msx_entry_count++];
        memcpy(e->name, fno.fname, n + 1);
        e->size = (uint32_t)fno.fsize;
        e->kind = kind;
    }
    f_closedir(&dir);

    qsort(msx_entries, (size_t)msx_entry_count, sizeof(msx_entries[0]), cmp_entry);
    return msx_entry_count;
}

int msx_enter_subdir(const char *name) {
    size_t cur = strlen(msx_current_dir);
    size_t add = strlen(name);
    if (cur + 1 + add + 1 >= sizeof(msx_current_dir)) return -1;
    if (strcmp(msx_current_dir, "/") != 0) {
        msx_current_dir[cur] = '/';
        memcpy(msx_current_dir + cur + 1, name, add + 1);
    } else {
        memcpy(msx_current_dir + 1, name, add + 1);
    }
    return msx_rescan();
}

int msx_enter_parent(void) {
    if (strcmp(msx_current_dir, "/") == 0) return msx_entry_count;
    char *slash = strrchr(msx_current_dir, '/');
    if (!slash) return msx_entry_count;
    if (slash == msx_current_dir) msx_current_dir[1] = 0; /* -> "/" */
    else                          *slash = 0;
    return msx_rescan();
}

void msx_entry_path(int idx, char *buf, size_t buf_sz) {
    if (idx < 0 || idx >= msx_entry_count) { if (buf_sz) buf[0] = 0; return; }
    const char *name = msx_entries[idx].name;
    if (strcmp(msx_current_dir, "/") == 0) snprintf(buf, buf_sz, "/%s", name);
    else                                   snprintf(buf, buf_sz, "%s/%s", msx_current_dir, name);
}

/* Persistent copies of the currently mounted paths. fMSX stashes the
 * pointer we pass to LoadCart / ChangeDisk into ROMName[] / DSKName[]
 * and reads it back later (save-states, hot-swap), so the memory must
 * outlive the mount call. */
static char g_cart_paths[2][MSX_MAX_PATH_LEN + MSX_MAX_FILENAME_LEN + 2];
static char g_disk_paths[2][MSX_MAX_PATH_LEN + MSX_MAX_FILENAME_LEN + 2];

int msx_mount_entry(int idx, msx_target_t target, bool reset_after_cart) {
    if (idx < 0 || idx >= msx_entry_count) return -1;
    msx_entry_t *e = &msx_entries[idx];

    switch (target) {
        case MSX_TARGET_CART_A:
        case MSX_TARGET_CART_B: {
            if (e->kind != MSX_ENTRY_ROM) return -2;
            int slot = (target == MSX_TARGET_CART_A) ? 0 : 1;
            msx_entry_path(idx, g_cart_paths[slot], sizeof(g_cart_paths[slot]));
            ROMName[slot] = g_cart_paths[slot];
            printf("mount: LoadCart(\"%s\", slot=%d)\n", g_cart_paths[slot], slot);

            /* Quick path sanity: open via FatFS directly so we can tell
             * whether fMSX's fopen path is wrong or the cart itself is
             * the problem. */
            {
                FIL probe;
                FRESULT pr = f_open(&probe, g_cart_paths[slot], FA_READ);
                if (pr != FR_OK) {
                    printf("  f_open failed: %d\n", pr);
                    return -4;
                }
                printf("  file size: %lu\n", (unsigned long)f_size(&probe));
                f_close(&probe);
            }

            int pages = LoadCart(g_cart_paths[slot], slot, MAP_GUESS);
            if (!pages) {
                printf("  LoadCart returned 0 (fMSX rejected the ROM)\n");
                return -3;
            }
            printf("  mounted %d 8k-pages\n", pages);
            if (reset_after_cart) ResetMSX(Mode, RAMPages, VRAMPages);
            return 0;
        }
        case MSX_TARGET_DISK_A:
        case MSX_TARGET_DISK_B: {
            if (e->kind != MSX_ENTRY_DISK) return -2;
            int drv = (target == MSX_TARGET_DISK_A) ? 0 : 1;
            msx_entry_path(idx, g_disk_paths[drv], sizeof(g_disk_paths[drv]));
            DSKName[drv] = g_disk_paths[drv];
            if (!ChangeDisk((byte)drv, g_disk_paths[drv])) return -3;
            return 0;
        }
        default:
            return -1;
    }
}
