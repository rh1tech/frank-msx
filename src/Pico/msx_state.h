/*
 * frank-msx — fMSX for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-msx
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * msx_state.h — save-state slot helpers, PSRAM-backed so we can
 * hold the MAX_STASIZE buffer (~272 KB for MSX2+ / 512 kB RAM+VRAM)
 * that doesn't fit in the RP2350 SRAM heap.
 *
 * Slot files live at /MSX/STATE/SLOT0.STA .. SLOT<N-1>.STA so they
 * roll together with the rest of the MSX dir on the SD card.
 */
#ifndef MSX_STATE_H
#define MSX_STATE_H

#include <stdbool.h>

#define MSX_STATE_SLOTS 4

/* True if slot N has a file on disk. No fMSX validation — we accept
 * whatever SaveSTA wrote, including stale images from other models. */
bool msx_state_slot_exists(int slot);

/* Save/Load a slot. Both return 0 on success, <0 on failure.
 *
 * Load can fail benignly if the slot was saved under a different
 * MSX model / RAM config — fMSX's LoadState refuses, and we leave
 * the emulator running with the pre-save state. */
int  msx_state_save(int slot);
int  msx_state_load(int slot);

/* Delete the slot file. Returns 0 on success (including "already
 * empty"), <0 on failure. */
int  msx_state_delete(int slot);

#endif /* MSX_STATE_H */
