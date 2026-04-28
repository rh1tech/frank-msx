/*
 * SRAM-resident memset/memcpy shims for the TV scanline ISR.
 *
 * The composite-TV driver's ISR calls memset() 15+ times per scanline
 * at 30 kHz.  On RP2350 the Pico SDK's default memset/memcpy live in
 * flash (unlike RP2040, where pico_mem_ops provides SRAM versions).
 * Leaving the ISR calling flash-resident memset blows the XIP cache on
 * every scan line and starves Core 0, dropping fMSX to ~1 fps.
 *
 * We override memset/memcpy in the TV TUs only, via `-include` and
 * __builtin wrappers that expand to small SRAM-resident loops.  This
 * doesn't touch the rest of frank-msx's memset/memcpy usage.
 */

#ifndef TV_MEM_H_
#define TV_MEM_H_

/* Pull the real string.h prototypes in first so subsequent translation
 * units don't see our function-style macros clobber their declarations
 * — we only want to rewrite CALL sites, not prototypes. */
#include <string.h>
#include <stddef.h>
#include <stdint.h>

/* Placed in SRAM via the `.time_critical` section.  The ISR is already
 * __time_critical_func; putting memset here keeps everything on the
 * same side of the flash/XIP boundary. */
static inline __attribute__((always_inline)) void *tv_fast_memset(void *dst, int val, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    uint8_t v = (uint8_t)val;
    /* Align to 4 bytes */
    while (n && ((uintptr_t)d & 3)) { *d++ = v; --n; }
    uint32_t v32 = ((uint32_t)v) * 0x01010101u;
    uint32_t *d32 = (uint32_t *)d;
    size_t w = n >> 2;
    while (w >= 4) { d32[0] = v32; d32[1] = v32; d32[2] = v32; d32[3] = v32; d32 += 4; w -= 4; }
    while (w--) *d32++ = v32;
    d = (uint8_t *)d32;
    n &= 3;
    while (n--) *d++ = v;
    return dst;
}

static inline __attribute__((always_inline)) void *tv_fast_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if ((((uintptr_t)d | (uintptr_t)s) & 3) == 0) {
        uint32_t *d32 = (uint32_t *)d;
        const uint32_t *s32 = (const uint32_t *)s;
        size_t w = n >> 2;
        while (w >= 4) { d32[0]=s32[0]; d32[1]=s32[1]; d32[2]=s32[2]; d32[3]=s32[3]; d32+=4; s32+=4; w-=4; }
        while (w--) *d32++ = *s32++;
        d = (uint8_t *)d32;
        s = (const uint8_t *)s32;
        n &= 3;
    }
    while (n--) *d++ = *s++;
    return dst;
}

#define memset(d, v, n) tv_fast_memset((d), (v), (n))
#define memcpy(d, s, n) tv_fast_memcpy((d), (s), (n))

#endif /* TV_MEM_H_ */
