/*
 * Composite-TV video shim for frank-msx.
 *
 * Wraps drivers/tv/ (the software-composite PAL/NTSC driver ported from
 * murmnes) behind the same HDMI.h API the rest of frank-msx uses, so
 * platform.c / main.c / the UI never need to know which physical
 * video path is active.
 *
 * Only compiled when VIDEO_COMPOSITE=ON. In that build the PIO HDMI /
 * VGA / HSTX drivers are all excluded — the TV driver owns PIO0,
 * three DMA channels and a Core-1 alarm pool.
 *
 * Layout:
 *   Core 0 → fMSX emulation.  graphics_set_buffer() copies the current
 *            256×228 MSX front-buffer into a contiguous 256×240 TV
 *            frame (6-line black border top+bottom to centre on NTSC
 *            overscan).
 *   Core 1 → tv_core1_run(): calls tv_graphics_init(), which claims
 *            PIO0 + DMA and registers a 30 kHz repeating timer on
 *            this core; the IRQ fills scanline buffers.  The core
 *            then idles in __wfi().
 *
 * SPDX-License-Identifier: MIT
 */

#include "board_config.h"
#include "HDMI.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/sync.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* The TV driver's exported symbols are renamed to tv_* via tv_rename.h
 * so that they don't collide with the HDMI.h API. We don't include the
 * TV header here (it redefines enum graphics_mode_t with a different
 * value set) — just forward-declare what we call. */
enum tv_graphics_mode_t { TV_TEXTMODE_DEFAULT, TV_GRAPHICSMODE_DEFAULT };

extern void tv_graphics_init(void);
extern void tv_graphics_set_buffer(uint8_t *buffer, uint16_t width, uint16_t height);
extern void tv_graphics_set_mode(int mode);
extern void tv_graphics_set_palette(uint8_t i, uint32_t color888);
extern void tv_graphics_set_offset(int x, int y);

/* ------------------------------------------------------------------ */
/* SELECT_VGA / testPins are normally defined on the PIO-HDMI path by
 * HDMI_vga.c / test_pins.c. Under VIDEO_COMPOSITE those aren't
 * compiled, so provide local no-op definitions that keep HDMI.h's
 * extern declarations satisfied and main.c's probe returning "HDMI"
 * (never VGA). */
bool SELECT_VGA = false;

int testPins(uint32_t pin0, uint32_t pin1) {
    (void)pin0; (void)pin1;
    return 0xFF;   /* non-0 / non-0x1F keeps SELECT_VGA false */
}

/* ------------------------------------------------------------------ */
#define TV_FB_W     256
#define TV_FB_H     240
#define MSX_W       MSX_FB_WIDTH   /* 256 */
#define MSX_H       MSX_FB_HEIGHT  /* 228 */
#define TOP_MARGIN  ((TV_FB_H - MSX_H) / 2)   /* 6 */

/* The TV scanline IRQ reads this framebuffer without locking; a memcpy
 * from Core 0 can tear a single line mid-scan, but on 60 Hz composite
 * that's invisible. */
static uint8_t __attribute__((aligned(4))) tv_frame[TV_FB_W * TV_FB_H];

/* ------------------------------------------------------------------ */
/* Host-visible state we still need to answer HDMI.h queries. */
static int tv_buffer_width  = TV_FB_W;
static int tv_buffer_height = TV_FB_H;
static int tv_shift_x = 0;
static int tv_shift_y = 0;
static volatile bool tv_core1_ready = false;

/* ------------------------------------------------------------------ */
/* Core 1 entry point.
 *
 * Claims PIO0, three DMA channels and a 30 kHz alarm-pool timer on
 * this core. After init it just idles — scanline generation runs
 * entirely inside the timer ISR. */
static void tv_core1_run(void) {
    tv_graphics_init();
    tv_graphics_set_buffer(tv_frame, TV_FB_W, TV_FB_H);
    tv_graphics_set_mode(TV_GRAPHICSMODE_DEFAULT);
    tv_graphics_set_offset(0, 0);
    /* Black border — palette entry 0 is the MSX background colour. */
    tv_graphics_set_palette(0, 0);
    __dmb();
    tv_core1_ready = true;
    __dmb();
    while (true) __wfi();
}

/* ------------------------------------------------------------------ */
/* HDMI.h API surface */

void graphics_init(g_out g) {
    (void)g;
    if (tv_core1_ready) return;
    memset(tv_frame, 0, sizeof(tv_frame));
    multicore_launch_core1(tv_core1_run);
    /* Wait for Core 1 to finish claiming PIO/DMA before Core 0 starts
     * pushing palette writes / mode changes. */
    while (!tv_core1_ready) tight_loop_contents();
}

void graphics_set_buffer(uint8_t *buffer) {
    if (!buffer) return;
    /* 256×228 MSX frame → 256×240 TV frame with 6 black lines top and
     * bottom. Copy under an aligned-word loop; alignment is guaranteed
     * since both buffers are 4-byte aligned and MSX_W is a multiple of
     * 4. A single memcpy suffices. */
    memcpy(&tv_frame[TOP_MARGIN * TV_FB_W], buffer, (size_t)MSX_W * MSX_H);
}

uint8_t *graphics_get_buffer(void) { return tv_frame; }

uint32_t graphics_get_width(void)  { return (uint32_t)tv_buffer_width;  }
uint32_t graphics_get_height(void) { return (uint32_t)tv_buffer_height; }

void graphics_set_res(int w, int h) {
    /* Informational on the TV path — the TV driver rescales internally
     * to the composite line format.  Record so graphics_get_width()
     * returns something sensible. */
    tv_buffer_width  = w;
    tv_buffer_height = h;
}

void graphics_set_shift(int x, int y) {
    /* TV driver does its own horizontal centring, so we only store
     * values for any code that queries them back.  fMSX's msx_ui
     * currently doesn't. */
    tv_shift_x = x;
    tv_shift_y = y;
}

void graphics_set_palette(uint8_t i, uint32_t color888) {
    if (!tv_core1_ready) return;
    tv_graphics_set_palette(i, color888);
}

void graphics_set_bgcolor(uint32_t color888) {
    /* TV driver's own bgcolor entry point is a stub; map background to
     * palette slot 0, which is what the MSX core uses for the border. */
    graphics_set_palette(0, color888);
}

uint32_t graphics_get_palette(uint8_t i) {
    (void)i;
    /* TV driver doesn't expose the cached RGB triple. The UI keeps its
     * own copy, so returning 0 here is harmless. */
    return 0;
}

void graphics_set_mode(enum graphics_mode_t mode) {
    if (!tv_core1_ready) return;
    /* frank-msx only ever passes GRAPHICSMODE_DEFAULT (index 1).  Map
     * the HDMI-side enum onto the TV-side enum directly. */
    tv_graphics_set_mode((int)mode == TEXTMODE_DEFAULT
                         ? TV_TEXTMODE_DEFAULT
                         : TV_GRAPHICSMODE_DEFAULT);
}

/* ------------------------------------------------------------------ */
/* Stubs: HDMI-only features that the TV path doesn't support.  These
 * must exist so the linker can resolve the same symbols platform.c /
 * msx_settings.c reference on the HDMI build. */
void graphics_set_crt_active(bool active)   { (void)active; }
bool graphics_get_crt_active(void)          { return false; }
void graphics_set_greyscale(bool active)    { (void)active; }
bool graphics_get_greyscale(void)           { return false; }
void graphics_restore_sync_colors(void)     { /* nothing to restore */ }

void graphics_init_hdmi(void)                         { }
void graphics_set_palette_hdmi(uint8_t i, uint32_t c) { (void)i; (void)c; }
void graphics_set_bgcolor_hdmi(uint32_t c)            { (void)c; }

void startVIDEO(uint8_t vol) { (void)vol; }
void set_palette(uint8_t n)  { (void)n;  }
