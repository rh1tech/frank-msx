/*
 * frank-msx — fMSX for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-msx
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PSRAM_INIT_H
#define PSRAM_INIT_H

#include "pico/stdlib.h"

void psram_init(uint cs_pin);

/* Write/read-back test of the PSRAM behind CS1, run after psram_init().
 * Returns false when no PSRAM chip answers; in that case CS1 is released
 * (the pin goes back to plain GPIO) and XIP writes to M1 are disabled. */
bool psram_detect(uint cs_pin);

#endif
