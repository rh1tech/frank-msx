#ifndef PSRAM_DLMALLOC_H
#define PSRAM_DLMALLOC_H

#include <stddef.h>

void  psram_heap_init(void *base, size_t size);
void *psram_malloc(size_t size);
void *psram_realloc(void *ptr, size_t size);
void  psram_free(void *ptr);
size_t psram_usable_size(void *ptr);

#endif
