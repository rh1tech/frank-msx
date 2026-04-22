/*
 * msx_loader.h — MSX cartridge + disk image scanner and mount helpers.
 *
 * The loader walks a configurable directory on the FatFS-mounted SD
 * card and builds a list of entries. Directories can be entered with
 * a synthetic ".." entry. Hand a selected entry to one of the mount
 * helpers and the fMSX core does the rest (LoadCart / ChangeDisk).
 */
#ifndef MSX_LOADER_H
#define MSX_LOADER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* FatFS LFN supports up to 255 characters. We keep the entry name
 * buffer generous so real-world game titles aren't truncated. 256
 * entries × 256 bytes = 64 kB in PSRAM — well under budget. */
#define MSX_MAX_ENTRIES       256
#define MSX_MAX_FILENAME_LEN  256
#define MSX_MAX_PATH_LEN      384

typedef enum {
    MSX_ENTRY_UNKNOWN = 0,
    MSX_ENTRY_DIR,
    MSX_ENTRY_ROM,      /* .rom / .mx1 / .mx2 / .col / .ri */
    MSX_ENTRY_DISK,     /* .dsk / .dsr */
} msx_entry_kind_t;

typedef struct {
    char             name[MSX_MAX_FILENAME_LEN];
    uint32_t         size;
    msx_entry_kind_t kind;
} msx_entry_t;

typedef enum {
    MSX_TARGET_CART_A = 0,
    MSX_TARGET_CART_B = 1,
    MSX_TARGET_DISK_A = 2,
    MSX_TARGET_DISK_B = 3,
    MSX_TARGET_COUNT
} msx_target_t;

extern msx_entry_t *msx_entries;                     /* PSRAM-backed */
extern int          msx_entry_count;
extern char         msx_current_dir[MSX_MAX_PATH_LEN]; /* e.g. "/MSX/ROMS" */

/* Identify a file's kind by extension (case-insensitive). */
msx_entry_kind_t msx_classify(const char *name);

/* Rescan msx_current_dir. Returns entry count (0 on empty / error). */
int msx_rescan(void);

/* Enter / leave directories. Updates msx_current_dir and rescans. */
int msx_enter_subdir(const char *name);
int msx_enter_parent(void);

/* Build a full absolute path for entry `idx` into `buf`. */
void msx_entry_path(int idx, char *buf, size_t buf_sz);

/* Mount the entry at `idx` into `target`. For cartridge targets we
 * reset the MSX after the load so the BIOS sees the new cart. For
 * disk targets we just ChangeDisk() — the BIOS "swap disk" routine
 * picks it up without a reset. Returns 0 on success. */
int msx_mount_entry(int idx, msx_target_t target, bool reset_after_cart);

/* Eject whatever is currently mounted into `target`. Cartridge ejects
 * also trigger a ResetMSX so the BIOS notices. Returns 0 on success. */
int msx_eject(msx_target_t target);

/* Current mount state (for the UI). Returns a filename (without path)
 * for display, or NULL if the slot/drive is empty. */
const char *msx_mounted_name(msx_target_t target);

/* Create a new blank 720 kB MSX-format disk image at
 * `/MSX/NEW_<counter>.DSK` and insert it into the given drive. */
int msx_create_blank_disk(msx_target_t target);

/* Save the current contents of a disk drive to a file under /MSX/.
 * `fmt` must be FMT_MSXDSK (raw .DSK) or FMT_FDI.
 * If the drive currently holds a named image, the new file is named
 * after it with the appropriate extension; otherwise an auto-name
 * `SAVE_<counter>.{dsk,fdi}` is used. */
int msx_save_disk(msx_target_t target, int fmt);

#endif /* MSX_LOADER_H */
