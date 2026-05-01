/*
 * frank-msx — fMSX for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-msx
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PSRAM_DLMALLOC_H
#define PSRAM_DLMALLOC_H

#include <stddef.h>

void  psram_heap_init(void *base, size_t size);
void *psram_malloc(size_t size);
void *psram_realloc(void *ptr, size_t size);
void  psram_free(void *ptr);
size_t psram_usable_size(void *ptr);

#endif
