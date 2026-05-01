/*
 * frank-msx — fMSX for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-msx
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * msx_tape.h — MSX cassette I/O for frank-msx.
 *
 * Two independent input paths:
 *
 *   1. File-backed .CAS: mount a .CAS image from SD. The image feeds
 *      both fMSX's BIOS-trap tape path (Patch.c / CasStream, transparent)
 *      and a real-time waveform generator that injects MSX-standard FSK
 *      bits into the PSG's port-14 "tape input" (bit 7). That second
 *      path is what custom non-BIOS loaders need in order to load.
 *
 *   2. Physical line-in on GPIO TAPE_IN_PIN (22 on M1/M2/Z0, 28 on DV/PC).
 *      When enabled, the GPIO level is sampled every time the MSX reads
 *      the tape bit. Useful for plugging a real cassette / audio-out
 *      of a phone into the Pico.
 *
 * The source is selected at runtime — mounting a .CAS switches to file
 * mode; the user can manually switch to line-in via the tape menu.
 *
 * The read hot path is msx_tape_psg_bit7(): O(1), lock-free, safe to
 * call every PSG port-14 read. Writes are done on one core (Core 0 /
 * UI thread); reads can happen on either core, so single-writer
 * multi-reader semantics are maintained with plain volatile.
 */
#ifndef MSX_TAPE_H
#define MSX_TAPE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    MSX_TAPE_SOURCE_NONE = 0,   /* no input — bit reads as 0 */
    MSX_TAPE_SOURCE_CAS,        /* .CAS file, real-time waveform */
    MSX_TAPE_SOURCE_LINE,       /* physical GPIO line-in */
    MSX_TAPE_SOURCE_COUNT
} msx_tape_source_t;

/* One-time setup. Safe to call from InitMachine(). Claims the TAPE_IN
 * GPIO and leaves it unused until msx_tape_set_source(LINE). */
void msx_tape_init(void);

/* Mount a .CAS file at `path` for reading. `path` must outlive the
 * mount (the loader keeps its own persistent buffer). Opens the file
 * via fMSX's ChangeTape() so the BIOS path keeps working, and stores
 * the image contents in PSRAM for the waveform generator. Returns 0
 * on success, <0 on failure. */
int msx_tape_mount(const char *path);

/* Unmount any currently-loaded .CAS. Frees the PSRAM buffer and closes
 * CasStream. Idempotent. */
void msx_tape_eject(void);

/* Rewind to the start of the current tape. No-op if no tape mounted. */
void msx_tape_rewind(void);

/* Returns the currently mounted .CAS path (without directory prefix
 * trimming), or NULL when no tape is loaded. Same storage contract as
 * msx_loader's g_cart_paths[] — the pointer is stable until the next
 * mount/eject call. */
const char *msx_tape_mounted_name(void);

/* Current source (reflects state after the latest mount / user action). */
msx_tape_source_t msx_tape_get_source(void);

/* Explicit source override. Use LINE to sample the GPIO, CAS after a
 * mount (msx_tape_mount() already sets this), or NONE to silence. */
void msx_tape_set_source(msx_tape_source_t src);

/* Hot path — return the current tape-in bit shifted into position 7
 * so MSX.c can OR it into the PSG[14] return value. */
uint8_t msx_tape_psg_bit7(void);

/* Called by PPIOut() when port-C bit 4 (motor relay) toggles. The
 * real MSX motor gates the tape drive; our waveform generator freezes
 * its bit stream while the motor is off so the first byte of data
 * isn't consumed before BASIC is ready for it. */
void msx_tape_set_motor(bool on);

/* Enable / disable the real-time waveform generator. Off by default —
 * BIOS tape traps (Patch.c TAPION/TAPIN) handle nearly all .CAS files
 * instantly through CasStream, and running the waveform in parallel
 * breaks BIOS's inter-block gap detection. Flip this on from the UI
 * only when loading a game with a custom loader that bypasses BIOS
 * and polls PSG[14] bit 7 directly ("Turbo loader" setting). */
void msx_tape_set_waveform_enabled(bool enabled);
bool msx_tape_get_waveform_enabled(void);

#endif /* MSX_TAPE_H */
