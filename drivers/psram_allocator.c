/*
 * psram_allocator.c — PSRAM heap backed by dlmalloc's mspace API.
 *
 * Ported from frank-blood (which uses dlmalloc compiled with
 * ONLY_MSPACES=1). We don't rely on a memmap_psram.ld linker script;
 * instead we carve the 8 MB PSRAM window as:
 *
 *   [PSRAM_BASE .. PSRAM_BASE + SCRATCH_SIZE)   scratch buffers
 *   [PSRAM_BASE + SCRATCH_SIZE .. PSRAM_END)    dlmalloc mspace
 *
 * The three scratch sub-regions (psram_get_scratch_1/2/file_buffer)
 * remain so any legacy call sites still work. All other allocations
 * go through psram_malloc / psram_free / psram_realloc, which now
 * truly free memory when the caller asks.
 */

#include "psram_allocator.h"
#include "psram_dlmalloc.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define PSRAM_BASE       0x11000000u
#define PSRAM_SIZE       (8u * 1024u * 1024u)
#define PSRAM_END        (PSRAM_BASE + PSRAM_SIZE)
#define PSRAM_NOCACHE    0x15000000u

/* 512 kB of scratch at the base of PSRAM:
 *   [  0 ..  128kB)   scratch 1  (e.g. decompression)
 *   [128 ..  256kB)   scratch 2
 *   [256 ..  512kB)   file-load buffer
 * The dlmalloc heap starts right after this region. */
#define SCRATCH_SIZE     (512u * 1024u)

static uint8_t *psram_start = (uint8_t *)(uintptr_t)PSRAM_BASE;

/* ---- dlmalloc mspace glue ------------------------------------ */

typedef void* mspace;
extern mspace create_mspace_with_base(void *base, size_t capacity, int locked);
extern void  *mspace_malloc  (mspace msp, size_t bytes);
extern void  *mspace_realloc (mspace msp, void *ptr, size_t bytes);
extern void   mspace_free    (mspace msp, void *ptr);
extern size_t mspace_usable_size(const void *ptr);

static mspace g_msp = NULL;

static int is_psram(const void *p) {
    uintptr_t a = (uintptr_t)p;
    return (a >= PSRAM_BASE    && a < PSRAM_END) ||
           (a >= PSRAM_NOCACHE && a < PSRAM_NOCACHE + PSRAM_SIZE);
}

static void ensure_init(void) {
    if (g_msp) return;
    void  *base = psram_start + SCRATCH_SIZE;
    size_t size = PSRAM_SIZE  - SCRATCH_SIZE;
    g_msp = create_mspace_with_base(base, size, 0);
    if (g_msp) {
        printf("PSRAM heap: %u kB at %p (dlmalloc)\n",
               (unsigned)(size / 1024), base);
    } else {
        printf("PSRAM heap: FAILED to init mspace\n");
    }
}

/* ---- public API ---------------------------------------------- */

void *psram_malloc(size_t size) {
    ensure_init();
    if (!g_msp) return NULL;
    return mspace_malloc(g_msp, size);
}

void *psram_realloc(void *ptr, size_t size) {
    ensure_init();
    if (!g_msp) return NULL;
    if (!ptr)       return mspace_malloc(g_msp, size);
    if (size == 0)  { psram_free(ptr); return NULL; }
    if (is_psram(ptr)) return mspace_realloc(g_msp, ptr, size);
    /* Pointer came from the C library heap — fall back. */
    return realloc(ptr, size);
}

void psram_free(void *ptr) {
    if (!ptr) return;
    if (is_psram(ptr)) {
        if (g_msp) mspace_free(g_msp, ptr);
        return;
    }
    free(ptr);
}

size_t psram_usable_size(void *ptr) {
    return ptr ? mspace_usable_size(ptr) : 0;
}

void psram_reset(void) {
    /* Throw the whole mspace away and start over. Used on deep reset
     * paths — any live dlmalloc blocks are leaked. */
    g_msp = NULL;
    ensure_init();
}

/* ---- compat shims kept so existing code keeps building ------- */

void  psram_set_temp_mode(int enable) { (void)enable; }
void  psram_set_sram_mode(int enable) { (void)enable; }
void  psram_reset_temp(void)          { }
size_t psram_get_temp_offset(void)    { return 0; }
void  psram_set_temp_offset(size_t o) { (void)o; }

void psram_mark_session(void)    { }
void psram_restore_session(void) { }
void psram_print_stats(void)     { }

void *psram_get_scratch_1(size_t size) {
    if (size > 128u * 1024u) return NULL;
    return psram_start;
}
void *psram_get_scratch_2(size_t size) {
    if (size > 128u * 1024u) return NULL;
    return psram_start + 128u * 1024u;
}
void *psram_get_file_buffer(size_t size) {
    if (size > 256u * 1024u) return NULL;
    return psram_start + 256u * 1024u;
}
