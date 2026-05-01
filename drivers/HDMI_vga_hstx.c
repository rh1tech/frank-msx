/*
 * frank-msx — fMSX for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-msx
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * HDMI_vga_hstx.c — VGA HSTX adapter.
 *
 * Exposes the same `graphics_*` API as the PIO HDMI / HDMI_hstx drivers but
 * targets the DispHSTX library's 320x240 RGB332 framebuffer (scaled 2x to
 * 640x480 VGA by DispHSTX). Ported in spirit from murmnes VGA HSTX path
 * with an MSX-centric palette (graphics_set_palette takes RGB888, not
 * QuickNES's int16 palette indices).
 *
 * MSX frame (256x228 8-bit indexed) is centred inside the 320x240
 * framebuffer with 32-pixel horizontal and 6-pixel vertical black borders.
 * DispHSTX reads the framebuffer directly from its Core 1 VGA ISR; writes
 * racing with scanout only tear at pixel granularity which is imperceptible
 * at 60 Hz. Same race trade-off murmnes ships with.
 *
 * M2-only. CMake enforces the platform guard. HDMI audio is unavailable on
 * VGA — audio falls back to PWM (or I2S on the M2's audio HAT) via the
 * dispatcher in main.c.
 */

#include "HDMI.h"

#include "pico/stdlib.h"

#include "disphstx.h"
#include "disphstx_vmode_simple.h"
#include "disphstx_vmode.h"

#include <string.h>

/* ---- DispHSTX framebuffer ------------------------------------------- */
/* 320x240 RGB332, scaled 2x to 640x480 by DispHSTX. */
#define FB_W 320
#define FB_H 240

/* MSX native frame dimensions (see src/board_config.h). */
#define MSX_W 256
#define MSX_H 228

/* Centred letterbox offsets. */
#define BORDER_L ((FB_W - MSX_W) / 2)          /* 32 */
#define BORDER_T ((FB_H - MSX_H) / 2)          /* 6  */

static uint8_t __attribute__((aligned(4))) vga_framebuffer[FB_W * FB_H];

/* ---- Same externally-visible globals the frank-msx code pokes. ------- */
int graphics_buffer_width   = MSX_W;
int graphics_buffer_height  = MSX_H;
int graphics_buffer_shift_x = 0;
int graphics_buffer_shift_y = 0;
enum graphics_mode_t hdmi_graphics_mode = GRAPHICSMODE_DEFAULT;

/* ---- Palette (RGB332) and raw RGB888 cache for greyscale toggle ----- */
static uint8_t  pal_rgb332[256];
static uint32_t pal_raw_rgb888[256];

static bool crt_active = false;
static bool greyscale_active = false;

/* ---- Current / pending MSX source buffer (owned by main.c) ---------- */
static const uint8_t *graphics_buffer = NULL;
static const uint8_t *pending_buffer  = NULL;

/* ---- Helpers -------------------------------------------------------- */
static inline uint8_t rgb888_to_rgb332(uint32_t c888) {
    uint8_t r = (c888 >> 16) & 0xff;
    uint8_t g = (c888 >>  8) & 0xff;
    uint8_t b =  c888        & 0xff;
    return (uint8_t)((r & 0xe0) | ((g & 0xe0) >> 3) | ((b & 0xc0) >> 6));
}

static inline uint32_t rgb888_to_grey888(uint32_t c888) {
    uint8_t r = (c888 >> 16) & 0xff;
    uint8_t g = (c888 >>  8) & 0xff;
    uint8_t b =  c888        & 0xff;
    uint8_t Y = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
    return ((uint32_t)Y << 16) | ((uint32_t)Y << 8) | Y;
}

/* Blit one MSX frame into the DispHSTX framebuffer through the palette. */
static void __not_in_flash("vga_blit") blit_msx_frame(const uint8_t *src) {
    if (!src) return;
    const uint8_t *pal = pal_rgb332;
    int fb_w = graphics_buffer_width;
    int fb_h = graphics_buffer_height;
    if (fb_w <= 0 || fb_h <= 0) return;

    /* Clamp to the letterbox window. Source dimensions match MSX native
     * by default, but the settings UI can shrink them. */
    if (fb_w > MSX_W) fb_w = MSX_W;
    if (fb_h > MSX_H) fb_h = MSX_H;
    int x_off = BORDER_L + (MSX_W - fb_w) / 2;
    int y_off = BORDER_T + (MSX_H - fb_h) / 2;

    for (int y = 0; y < fb_h; y++) {
        const uint8_t *srow = src + y * fb_w;
        uint8_t *drow = vga_framebuffer + (y_off + y) * FB_W + x_off;
        if (crt_active && (y & 1)) {
            /* Scanline effect — leave every other row black. */
            memset(drow, 0, fb_w);
            continue;
        }
        int x = 0;
        /* 4-at-a-time with aligned 32-bit source reads. */
        int fast = fb_w & ~3;
        for (; x < fast; x += 4) {
            uint32_t p = *(const uint32_t *)(srow + x);
            drow[x + 0] = pal[ p        & 0xff];
            drow[x + 1] = pal[(p >> 8)  & 0xff];
            drow[x + 2] = pal[(p >> 16) & 0xff];
            drow[x + 3] = pal[(p >> 24)       ];
        }
        for (; x < fb_w; x++) drow[x] = pal[srow[x]];
    }
}

/* ---- Public API mirror ---------------------------------------------- */
void graphics_set_buffer(uint8_t *buffer) {
    pending_buffer = buffer;
    /* PutImage() calls this once per MSX frame from Core 0. Blit
     * immediately — DispHSTX scanout on Core 1 will pick up whatever is
     * in vga_framebuffer at the next VSYNC. */
    if (pending_buffer && pending_buffer != graphics_buffer) {
        graphics_buffer = pending_buffer;
    }
    if (graphics_buffer) {
        blit_msx_frame(graphics_buffer);
    }
}

uint8_t *graphics_get_buffer(void) {
    return (uint8_t *)(graphics_buffer ? graphics_buffer : pending_buffer);
}

uint32_t graphics_get_width(void)  { return (uint32_t)graphics_buffer_width;  }
uint32_t graphics_get_height(void) { return (uint32_t)graphics_buffer_height; }

void graphics_set_res(int w, int h) {
    graphics_buffer_width  = w;
    graphics_buffer_height = h;
}

void graphics_set_shift(int x, int y) {
    graphics_buffer_shift_x = x;
    graphics_buffer_shift_y = y;
}

void graphics_set_palette(uint8_t i, uint32_t color888) {
    pal_raw_rgb888[i] = color888 & 0x00ffffff;
    uint32_t c = greyscale_active ? rgb888_to_grey888(color888) : color888;
    pal_rgb332[i] = rgb888_to_rgb332(c);
}

uint32_t graphics_get_palette(uint8_t i) {
    return pal_raw_rgb888[i];
}

void graphics_set_mode(enum graphics_mode_t mode) {
    hdmi_graphics_mode = mode;
}

void graphics_set_bgcolor(uint32_t color888) {
    /* Reuse slot 0 as background colour, matching the HSTX HDMI path. */
    graphics_set_palette(0, color888);
}

void graphics_restore_sync_colors(void) {
    /* DispHSTX generates sync in hardware — nothing to restore. */
}

void graphics_set_crt_active(bool active) { crt_active       = active; }
bool graphics_get_crt_active(void)        { return crt_active;         }

void graphics_set_greyscale(bool active) {
    if (greyscale_active == active) return;
    greyscale_active = active;
    for (int i = 0; i < 256; i++) {
        uint32_t c = greyscale_active ? rgb888_to_grey888(pal_raw_rgb888[i])
                                      : pal_raw_rgb888[i];
        pal_rgb332[i] = rgb888_to_rgb332(c);
    }
}
bool graphics_get_greyscale(void) { return greyscale_active; }

struct video_mode_t graphics_get_video_mode(int mode) {
    (void)mode;
    /* Standard VGA 640x480@60: 800 h-total, 640 active, pixel clock 25.175 MHz
     * (DispHSTX runs a 25.2 MHz approximation at clk_hstx / 5 = 126/5). */
    struct video_mode_t vm = {
        .h_total  = 800,
        .h_width  = 640,
        .freq     = 60,
        .vgaPxClk = 25200000,
    };
    return vm;
}

/* ====================================================================
 * graphics_init — bring up DispHSTX on Core 0. DispHSTX claims Core 1
 * itself for the VGA scanout ISR. No scanline callback is needed — the
 * VGA hardware reads directly from vga_framebuffer.
 * ==================================================================== */
void graphics_init(g_out out) {
    (void)out;  /* VGA-only; mode ignored. */

    /* Seed palette + clear framebuffer so the first scanout is black
     * rather than undefined BSS (already zero-init, but be explicit). */
    for (int i = 0; i < 256; i++) {
        pal_raw_rgb888[i] = 0;
        pal_rgb332[i]     = 0;
    }
    memset(vga_framebuffer, 0, sizeof(vga_framebuffer));

    /* DispHSTX configures HSTX + sys_clock and launches the VGA ISR on
     * Core 1 internally. The `_Fast` variant runs sys_clock at 252 MHz
     * (same as the HDMI_HSTX path) instead of 126 MHz — fMSX needs the
     * extra headroom to keep up at 60 Hz in MSX2+ mode. */
    (void)DispVMode320x240x8_Fast(DISPHSTX_DISPMODE_VGA, vga_framebuffer);

    /* If graphics_set_buffer() was called before init, pick it up. */
    if (pending_buffer && !graphics_buffer) {
        graphics_buffer = pending_buffer;
        blit_msx_frame(graphics_buffer);
    }
}
