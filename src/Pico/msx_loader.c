/*
 * msx_loader.c — scan the SD card for MSX ROMs / DSKs and hand them
 *                to the fMSX core via LoadCart() / ChangeDisk().
 */

#include "msx_loader.h"
#include "msx_settings.h"
#include "msx_tape.h"
#include "MSX.h"
#include "FDIDisk.h"
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
    if (ext_ieq(e, "cas")) return MSX_ENTRY_TAPE;
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
static char g_tape_path   [MSX_MAX_PATH_LEN + MSX_MAX_FILENAME_LEN + 2];

/* Mount state tracked by the UI so it can display "currently loaded"
 * and decide whether to show Eject / Save menu entries. Updated by
 * msx_mount_entry() / msx_eject() / msx_create_blank_disk(). */
static bool g_cart_loaded[2] = { false, false };
static bool g_disk_loaded[2] = { false, false };

/* Dirty flags driven by Patch.c / WD1793.c. Raised on any sector
 * write, cleared by flush/eject. */
static volatile bool g_disk_dirty[2] = { false, false };

/* Called from Patch.c:DiskWrite and WD1793.c write path.
 * Hot path: just set a flag. */
void msx_disk_mark_dirty(unsigned char drive) {
    if (drive < 2) g_disk_dirty[drive] = true;
}

int msx_disk_flush_if_dirty(int drv) {
    if (drv < 0 || drv > 1) return -1;
    if (!g_disk_dirty[drv]) return 1;  /* nothing to do */
    if (!g_disk_loaded[drv]) return -2;
    const char *path = g_disk_paths[drv];
    if (!path || !path[0]) return -3;
    printf("disk: flushing drive %c -> %s\n", 'A' + drv, path);
    int r = SaveFDI(&FDD[drv], path, FMT_MSXDSK);
    /* SaveFDI: 0=failed, 1=truncated, 2=padded, 3=ok. */
    if (r >= FDI_SAVE_TRUNCATED) {
        g_disk_dirty[drv] = false;
        return 0;
    }
    return -4;
}

static void strip_to_basename(const char *full, char *out, size_t out_sz) {
    const char *slash = strrchr(full, '/');
    const char *name  = slash ? slash + 1 : full;
    size_t n = strnlen(name, out_sz - 1);
    memcpy(out, name, n);
    out[n] = 0;
}

const char *msx_mounted_name(msx_target_t target) {
    switch (target) {
        case MSX_TARGET_CART_A: return g_cart_loaded[0] ? g_cart_paths[0] : NULL;
        case MSX_TARGET_CART_B: return g_cart_loaded[1] ? g_cart_paths[1] : NULL;
        case MSX_TARGET_DISK_A: return g_disk_loaded[0] ? g_disk_paths[0] : NULL;
        case MSX_TARGET_DISK_B: return g_disk_loaded[1] ? g_disk_paths[1] : NULL;
        case MSX_TARGET_TAPE:   return msx_tape_mounted_name();
        default:                 return NULL;
    }
}

/* If /MSX/CHEATS/<basename>.mcf exists for the just-mounted cart,
 * pull it into CheatCodes[] so the master Cheats(ON/OFF) switch can
 * toggle its patches. Safe to call with no .mcf present. */
static void try_load_cheats_for(const char *cart_path) {
    if (!cart_path || !*cart_path) return;
    const char *slash = strrchr(cart_path, '/');
    const char *base  = slash ? slash + 1 : cart_path;
    char mcf[MSX_MAX_PATH_LEN + 32];
    const char *dot = strrchr(base, '.');
    size_t stem = dot ? (size_t)(dot - base) : strlen(base);
    if (stem >= MSX_MAX_PATH_LEN) return;
    char stembuf[MSX_MAX_PATH_LEN];
    memcpy(stembuf, base, stem);
    stembuf[stem] = 0;
    snprintf(mcf, sizeof(mcf), "/MSX/CHEATS/%s.mcf", stembuf);

    FILINFO fno;
    if (f_stat(mcf, &fno) != FR_OK) return;
    int n = LoadMCF(mcf);
    printf("cheats: %s -> %d MCF entries loaded\n", mcf, n);
}

int msx_mount_entry_with_mapper(int idx, msx_target_t target, int mapper,
                                bool reset_after_cart) {
    if (idx < 0 || idx >= msx_entry_count) return -1;
    msx_entry_t *e = &msx_entries[idx];

    switch (target) {
        case MSX_TARGET_CART_A:
        case MSX_TARGET_CART_B: {
            if (e->kind != MSX_ENTRY_ROM) return -2;
            int slot = (target == MSX_TARGET_CART_A) ? 0 : 1;

            /* Free the old ROMData explicitly — fMSX's LoadCart
             * allocates the new buffer BEFORE freeing the old one,
             * so we plug that half of the leak here. The eject code
             * inside LoadCart() does free, but also calls ResetMSX,
             * which we want to control ourselves from the UI layer. */
            extern byte *ROMData[];
            extern byte ROMMask[];
            if (g_cart_loaded[slot] && ROMData[slot]) {
                psram_free(ROMData[slot]);
                ROMData[slot] = NULL;
                ROMMask[slot] = 0;
                g_cart_loaded[slot] = false;
            }

            msx_entry_path(idx, g_cart_paths[slot], sizeof(g_cart_paths[slot]));
            ROMName[slot] = g_cart_paths[slot];
            printf("mount: LoadCart(\"%s\", slot=%d, mapper=%d)\n",
                   g_cart_paths[slot], slot, mapper);

            int pages = LoadCart(g_cart_paths[slot], slot, mapper);
            if (!pages) {
                printf("  LoadCart returned 0 (fMSX rejected the ROM)\n");
                g_cart_loaded[slot] = false;
                return -3;
            }
            g_cart_loaded[slot] = true;
            printf("  mounted %d 8k-pages\n", pages);
            try_load_cheats_for(g_cart_paths[slot]);
            if (reset_after_cart) ResetMSX(Mode, RAMPages, VRAMPages);
            return 0;
        }
        case MSX_TARGET_DISK_A:
        case MSX_TARGET_DISK_B: {
            if (e->kind != MSX_ENTRY_DISK) return -2;
            int drv = (target == MSX_TARGET_DISK_A) ? 0 : 1;
            /* If the currently-mounted disk is dirty, save it before we
             * overwrite the FDIDisk buffer. Mount-replace is a common
             * "swap disks" action and losing writes here would be the
             * worst-case data-loss path. */
            if (g_disk_loaded[drv]) {
                int fr = msx_disk_flush_if_dirty(drv);
                if (fr < 0 && fr != -3)
                    printf("  warning: pre-swap flush returned %d\n", fr);
            }
            /* Disks auto-eject inside ChangeDisk() when a drive is
             * already loaded (EjectFDI is called before re-Load). */
            msx_entry_path(idx, g_disk_paths[drv], sizeof(g_disk_paths[drv]));
            DSKName[drv] = g_disk_paths[drv];
            if (!ChangeDisk((byte)drv, g_disk_paths[drv])) return -3;
            g_disk_loaded[drv] = true;
            g_disk_dirty[drv]  = false;
            return 0;
        }
        case MSX_TARGET_TAPE: {
            if (e->kind != MSX_ENTRY_TAPE) return -2;
            msx_entry_path(idx, g_tape_path, sizeof(g_tape_path));
            int rc = msx_tape_mount(g_tape_path);
            if (rc != 0) { g_tape_path[0] = 0; return rc < 0 ? -3 : rc; }
            return 0;
        }
        default:
            return -1;
    }
}

int msx_mount_entry(int idx, msx_target_t target, bool reset_after_cart) {
    return msx_mount_entry_with_mapper(idx, target, MAP_GUESS, reset_after_cart);
}

int msx_eject(msx_target_t target) {
    switch (target) {
        case MSX_TARGET_CART_A:
        case MSX_TARGET_CART_B: {
            int slot = (target == MSX_TARGET_CART_A) ? 0 : 1;
            if (!g_cart_loaded[slot]) return -1;
            /* LoadCart(NULL, slot, 0) frees the old ROMData and then
             * calls ResetMSX(). That's the leak-free path. */
            LoadCart(NULL, slot, 0);
            g_cart_loaded[slot] = false;
            g_cart_paths[slot][0] = 0;
            return 0;
        }
        case MSX_TARGET_DISK_A:
        case MSX_TARGET_DISK_B: {
            int drv = (target == MSX_TARGET_DISK_A) ? 0 : 1;
            if (!g_disk_loaded[drv]) return -1;
            /* Flush any pending writes to the SD card before we drop
             * the in-memory FDIDisk buffer. */
            int fr = msx_disk_flush_if_dirty(drv);
            if (fr < 0 && fr != -3)
                printf("eject: dirty flush returned %d\n", fr);
            /* ChangeDisk(drv, NULL) frees the FDIDisk buffer via
             * EjectFDI. No ResetMSX; disk swap is hot. */
            ChangeDisk((byte)drv, NULL);
            g_disk_loaded[drv] = false;
            g_disk_dirty[drv]  = false;
            g_disk_paths[drv][0] = 0;
            return 0;
        }
        case MSX_TARGET_TAPE: {
            if (!msx_tape_mounted_name()) return -1;
            msx_tape_eject();
            g_tape_path[0] = 0;
            return 0;
        }
        default:
            return -1;
    }
}

static int next_sequence_id(const char *prefix, const char *ext) {
    /* Pick a filename /MSX/<prefix><NN>.<ext> that doesn't exist yet. */
    FILINFO fno;
    char trial[MSX_MAX_PATH_LEN];
    for (int i = 1; i < 1000; ++i) {
        snprintf(trial, sizeof(trial), "/MSX/%s%03d.%s", prefix, i, ext);
        if (f_stat(trial, &fno) != FR_OK) return i;
    }
    return 0;
}

int msx_create_blank_disk(msx_target_t target) {
    if (target != MSX_TARGET_DISK_A && target != MSX_TARGET_DISK_B) return -1;
    int drv = (target == MSX_TARGET_DISK_A) ? 0 : 1;

    int seq = next_sequence_id("NEW", "DSK");
    if (seq == 0) return -2;

    snprintf(g_disk_paths[drv], sizeof(g_disk_paths[drv]),
             "/MSX/NEW%03d.DSK", seq);
    printf("disk: creating blank image at %s\n", g_disk_paths[drv]);

    /* fMSX's ChangeDisk() with an empty string ("") creates a new
     * 720 kB MSX-formatted disk image in memory. We follow up with
     * SaveFDI(FMT_MSXDSK) so the empty image lives on the SD card. */
    DSKName[drv] = g_disk_paths[drv];
    if (!ChangeDisk((byte)drv, "")) return -3;
    if (!SaveFDI(&FDD[drv], g_disk_paths[drv], FMT_MSXDSK)) return -4;
    g_disk_loaded[drv] = true;
    g_disk_dirty[drv]  = false;
    return 0;
}

int msx_save_disk(msx_target_t target, int fmt) {
    if (target != MSX_TARGET_DISK_A && target != MSX_TARGET_DISK_B) return -1;
    int drv = (target == MSX_TARGET_DISK_A) ? 0 : 1;
    if (!g_disk_loaded[drv]) return -2;

    const char *ext = (fmt == FMT_FDI) ? "FDI" : "DSK";
    int seq = next_sequence_id("SAVE", ext);
    if (seq == 0) return -3;

    char dest[MSX_MAX_PATH_LEN];
    snprintf(dest, sizeof(dest), "/MSX/SAVE%03d.%s", seq, ext);
    printf("disk: saving drive %c to %s (fmt=%d)\n", 'A' + drv, dest, fmt);

    int r = SaveFDI(&FDD[drv], dest, fmt);
    /* SaveFDI result: 0 failed, 1 truncated, 2 padded, 3 ok. */
    return (r >= FDI_SAVE_TRUNCATED) ? 0 : -4;
}
