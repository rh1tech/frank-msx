/*
 * VGA output driver for frank-msx
 * Adapted from murmnes/drivers/hdmi_pio/vga.c by Mikhail Matveev.
 *
 * When SELECT_VGA is true, VGA PIO output is used. When false, all
 * dispatcher calls defer to the existing HDMI (TMDS) driver in HDMI.c.
 * Only one can be active at a time (shared PIO0 and DMA IRQ).
 *
 * The M2 board shares the HDMI connector's 8 GPIOs (12..19) with a VGA
 * signal produced by an HDMI-to-VGA ribbon/DAC. Layout:
 *
 *   GPIO 12 -> B0  (bit 0)
 *   GPIO 13 -> B1  (bit 1)
 *   GPIO 14 -> G0  (bit 2)
 *   GPIO 15 -> G1  (bit 3)
 *   GPIO 16 -> R0  (bit 4)
 *   GPIO 17 -> R1  (bit 5)
 *   GPIO 18 -> HS  (bit 6)
 *   GPIO 19 -> VS  (bit 7)
 *
 * Autodetection: main.c probes GPIO12/13 via testPins() before calling
 * graphics_init() and sets SELECT_VGA accordingly. If a real HDMI cable
 * is plugged in, the two pins float independently and SELECT_VGA stays
 * false — the HDMI TMDS driver takes over.
 *
 * SPDX-License-Identifier: MIT
 */

#include "board_config.h"   /* must come before HDMI.h so HDMI_BASE_PIN picks
                              up the M2 board's value (12) rather than the
                              HDMI.h default (6) — GPIO 6 is SD SPI0_SCK! */
#include "HDMI.h"
#include "hardware/clocks.h"
#include "stdbool.h"
#include "hardware/structs/pll.h"
#include "hardware/structs/systick.h"

#include "hardware/dma.h"
#include "hardware/irq.h"
#include <string.h>
#include <stdio.h>
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "stdlib.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* VGA uses the same GPIO range as the HDMI ribbon (12..19 on M2). */
#ifndef VGA_BASE_PIN
#define VGA_BASE_PIN (HDMI_BASE_PIN)
#endif

/* PIO shared with HDMI (only one is active at a time). */
#ifndef PIO_VGA
#define PIO_VGA pio0
#endif

/* Frank-msx's HDMI PIO driver uses DMA_IRQ_1 (audio DMA owns IRQ_0),
 * so the VGA driver must use the same IRQ slot to stay compatible. */
#ifndef VGA_DMA_IRQ
#define VGA_DMA_IRQ (DMA_IRQ_1)
#endif

/* conv_color[] is defined in HDMI.c — VGA reuses its 4 KB buffer as
 * line-pattern storage when SELECT_VGA is active. */
extern uint32_t conv_color[1224];

/* SELECT_VGA: set by main.c before graphics_init() based on testPins(). */
bool SELECT_VGA = false;

/* HDMI dispatcher targets (defined in HDMI.c). */
extern void graphics_init_hdmi(void);
extern void graphics_set_palette_hdmi(uint8_t i, uint32_t color888);
extern void graphics_set_bgcolor_hdmi(uint32_t color888);

/* Host framebuffer globals — defined in HDMI.c, written by main.c. These
 * are ordinary .data ints so they live in SRAM; reading them from the
 * ISR does not hit flash. */
extern int graphics_buffer_width;
extern int graphics_buffer_height;
extern int graphics_buffer_shift_x;
extern int graphics_buffer_shift_y;

/* Per-line source fetch helper (defined in HDMI.c). The HDMI ISR is in
 * __scratch_y so it's safe to call from there; we shadow the framebuffer
 * pointer into SRAM below so the VGA ISR doesn't have to branch through
 * flash to reach it. */
extern uint8_t *get_line_buffer(int line);

/* Framebuffer pointer mirror.
 * HDMI.c's graphics_buffer is a file-local static in __scratch_y. Host
 * code updates it via graphics_set_buffer() (defined in HDMI.c), which
 * also writes this shadow so the VGA ISR can read the framebuffer from
 * SRAM without calling across the translation unit boundary. Exposed
 * (non-static) so HDMI.c can write to it. */
uint8_t * __scratch_y("vga_fb") vga_fb = NULL;

/* Video mode table (defined in HDMI.c): shares the 640x480@60Hz entry. */
extern struct video_mode_t graphics_get_video_mode(int mode);
static int get_video_mode(void) { return 0; }

/* Hot-path mode constants cached in SRAM. The ISR runs at ~31 kHz and
 * reading the video-mode struct from flash on every call thrashes the
 * XIP cache badly enough to starve polled SPI on core 0 (SD card reads
 * time out). We populate these once in init_vga() from
 * graphics_get_video_mode() and read them from SRAM in the ISR. */
static int __scratch_y("vga_mode_h_total") vga_h_total = 524;
static int __scratch_y("vga_mode_h_width") vga_h_width = 480;

/* VGA PIO program: shift 8 bits out per FIFO byte. */
static uint16_t pio_program_VGA_instructions[] = {
    //     .wrap_target
    0x6008, //  0: out    pins, 8
    //     .wrap
};

static const struct pio_program pio_program_VGA = {
    .instructions = pio_program_VGA_instructions,
    .length = 1,
    .origin = -1,
};

/* Line pattern ping-pong buffers + palette. Hot-path state lives in
 * __scratch_y so the ISR reads it from SRAM. */
static uint32_t * __scratch_y("vga_lp")   lines_pattern[4];
static uint16_t  __scratch_y("vga_pal")   pallette[256];
static uint32_t *lines_pattern_data = NULL;
static int _SM_VGA = -1;

static int __scratch_y("vga_vs_b") line_VS_begin = 490;
static int __scratch_y("vga_vs_e") line_VS_end   = 491;
static int __scratch_y("vga_shft") shift_picture = 0;

static int __scratch_y("vga_vls") visible_line_size = 320;

static int __scratch_y("vga_cc") dma_chan_ctrl_vga;
static int __scratch_y("vga_c")  dma_chan_vga;

static uint32_t __scratch_y("vga_bg") bg_color[2];
static uint16_t palette16_mask = 0;

static enum graphics_mode_t __scratch_y("vga_mode") vga_graphics_mode = GRAPHICSMODE_DEFAULT;

static uint32_t frame_number = 0;
static uint32_t __scratch_y("vga_sl") screen_line = 0;

/* ==========================================================================
 * DMA ISR: runs in ~32 µs windows (per-line) and rewrites the next line
 * buffer based on current screen_line position (sync / image / back porch).
 *
 * Placed in SRAM (__scratch_y) like the HDMI ISR — the VGA path fires at
 * ~31 kHz, so a flash-resident ISR causes enough XIP-cache thrashing to
 * starve polled SPI traffic on core 0 (FatFS directory reads time out
 * after the scanner starts running).
 * ========================================================================== */
static void __scratch_y("vga_driver") dma_handler_VGA(void) {
    /* Frank-msx uses DMA_IRQ_1 for the video path. */
    dma_hw->ints1 = 1u << dma_chan_ctrl_vga;
    screen_line++;

    const uint32_t h_total = (uint32_t)vga_h_total;
    const uint32_t h_width = (uint32_t)vga_h_width;

    if (screen_line == h_total) {
        screen_line = 0;
        frame_number++;
    }

    if (screen_line >= h_width) {
        /* Vertical blanking: either VS or back-porch line template. */
        if (screen_line == h_width || screen_line == h_width + 3) {
            uint32_t *output_buffer_32bit = lines_pattern[2 + (screen_line & 1)];
            output_buffer_32bit += shift_picture / 4;
            uint32_t color32 = bg_color[0];
            for (int i = visible_line_size / 2; i--;) {
                *output_buffer_32bit++ = color32;
            }
        }

        if (screen_line >= (uint32_t)line_VS_begin && screen_line <= (uint32_t)line_VS_end)
            dma_channel_set_read_addr(dma_chan_ctrl_vga, &lines_pattern[1], false); /* VS pulse */
        else
            dma_channel_set_read_addr(dma_chan_ctrl_vga, &lines_pattern[0], false); /* empty line */
        return;
    }

    int y, line_number;

    uint32_t **output_buffer = &lines_pattern[2 + (screen_line & 1)];
    switch (vga_graphics_mode) {
        case GRAPHICSMODE_DEFAULT:
            line_number = screen_line / 2;
            if (screen_line % 2) return;             /* 2x vertical scaling */
            y = screen_line / 2 - graphics_buffer_shift_y;
            break;
        default:
            dma_channel_set_read_addr(dma_chan_ctrl_vga, &lines_pattern[0], false);
            return;
    }

    if (y < 0) {
        dma_channel_set_read_addr(dma_chan_ctrl_vga, &lines_pattern[0], false);
        return;
    }
    if (y >= graphics_buffer_height) {
        /* Fill a background stripe for the first few post-image rows so
         * the bottom border is a clean bg colour rather than stale bits. */
        if (y == graphics_buffer_height || y == graphics_buffer_height + 1 ||
            y == graphics_buffer_height + 2) {
            uint32_t *output_buffer_32bit = *output_buffer;
            uint32_t color32 = bg_color[0];

            output_buffer_32bit += shift_picture / 4;
            for (int i = visible_line_size / 2; i--;) {
                *output_buffer_32bit++ = color32;
            }
        }
        dma_channel_set_read_addr(dma_chan_ctrl_vga, output_buffer, false);
        return;
    }

    /* Active image area. Read the framebuffer pointer from SRAM mirror
     * to avoid a flash-resident helper call on every scanline. */
    uint8_t *fb = vga_fb;
    if (!fb || y >= graphics_buffer_height) {
        dma_channel_set_read_addr(dma_chan_ctrl_vga, &lines_pattern[0], false);
        return;
    }
    uint8_t *input_buffer_8bit = fb + y * graphics_buffer_width;

    uint16_t *output_buffer_16bit = (uint16_t *)(*output_buffer);
    output_buffer_16bit += shift_picture / 2;

    graphics_buffer_shift_x &= 0xfffffff2; /* 2-bit-aligned shift */

    uint max_width = graphics_buffer_width;
    if (graphics_buffer_shift_x < 0) {
        input_buffer_8bit -= graphics_buffer_shift_x / 4;
        max_width += graphics_buffer_shift_x;
    }
    else {
        output_buffer_16bit += graphics_buffer_shift_x;
    }

    int width = MIN((visible_line_size - ((graphics_buffer_shift_x > 0) ? graphics_buffer_shift_x : 0)),
                    (int)max_width);
    if (width < 0) {
        dma_channel_set_read_addr(dma_chan_ctrl_vga, output_buffer, false);
        return;
    }

    /* Palette index -> 16-bit line pattern (two VGA pixels per source
     * pixel — gives 2x horizontal scaling to fill 640 from 320 source). */
    uint16_t *current_palette = pallette;
    switch (vga_graphics_mode) {
        case GRAPHICSMODE_DEFAULT:
            for (register int x = 0; x < width; ++x) {
                register uint8_t cx = input_buffer_8bit[x];
                *output_buffer_16bit++ = current_palette[cx];
            }
            break;
        default:
            break;
    }
    dma_channel_set_read_addr(dma_chan_ctrl_vga, output_buffer, false);
}

/* ==========================================================================
 * VGA line/sync template setup (called from graphics_init when SELECT_VGA).
 * ========================================================================== */
static void graphics_set_mode_vga(enum graphics_mode_t mode) {
    if (_SM_VGA < 0) return;
    vga_graphics_mode = mode;

    /* Already configured? Only mode was updated. */
    if (lines_pattern_data) return;

    uint8_t TMPL_VHS8  = 0;
    uint8_t TMPL_VS8   = 0;
    uint8_t TMPL_HS8   = 0;
    uint8_t TMPL_LINE8 = 0b11000000;

    int HS_SHIFT = 328 * 2;
    int HS_SIZE  = 48  * 2;
    int line_size = 400 * 2;
    shift_picture     = line_size - HS_SHIFT;
    palette16_mask    = 0xc0c0;
    visible_line_size = 320;
    line_VS_begin     = 490;
    line_VS_end       = 491;

    double fdiv;
    {
        struct video_mode_t vMode = graphics_get_video_mode(get_video_mode());
        fdiv = clock_get_hz(clk_sys) / (double)vMode.vgaPxClk;
    }

    /* Adjust bg colour to ride the sync-bit mask. */
    bg_color[0] = bg_color[0] & 0x3f3f3f3f | palette16_mask | palette16_mask << 16;
    bg_color[1] = bg_color[1] & 0x3f3f3f3f | palette16_mask | palette16_mask << 16;

    const uint32_t div32 = (uint32_t)(fdiv * (1 << 16) + 0.0);
    PIO_VGA->sm[_SM_VGA].clkdiv = div32 & 0xfffff000;
    dma_channel_set_trans_count(dma_chan_vga, line_size / 4, false);

    lines_pattern_data = conv_color;
    for (int i = 0; i < 4; i++) {
        lines_pattern[i] = &lines_pattern_data[i * (line_size / 4)];
    }
    TMPL_VHS8 = TMPL_LINE8 ^ 0b11000000;
    TMPL_VS8  = TMPL_LINE8 ^ 0b10000000;
    TMPL_HS8  = TMPL_LINE8 ^ 0b01000000;

    uint8_t *base_ptr = (uint8_t *)lines_pattern[0];
    /* Empty line: full bg + HS pulse at start. */
    memset(base_ptr, TMPL_LINE8, line_size);
    memset(base_ptr, TMPL_HS8,   HS_SIZE);

    /* Vsync line: VS + combined HS/VS pulse at start. */
    base_ptr = (uint8_t *)lines_pattern[1];
    memset(base_ptr, TMPL_VS8,  line_size);
    memset(base_ptr, TMPL_VHS8, HS_SIZE);

    /* Image-line templates (ping-pong). */
    base_ptr = (uint8_t *)lines_pattern[2];
    memcpy(base_ptr, lines_pattern[0], line_size);
    base_ptr = (uint8_t *)lines_pattern[3];
    memcpy(base_ptr, lines_pattern[0], line_size);
}

/* ==========================================================================
 * Public API — dispatches to VGA or HDMI based on SELECT_VGA.
 * ========================================================================== */

static void graphics_set_palette_vga(uint8_t i, uint32_t color888) {
    const uint8_t conv0[] = { 0b00, 0b00, 0b01, 0b10, 0b10, 0b10, 0b11, 0b11 };
    const uint8_t conv1[] = { 0b00, 0b01, 0b01, 0b01, 0b10, 0b11, 0b11, 0b11 };

    const uint8_t b = (color888         & 0xff) / 42;
    const uint8_t r = ((color888 >> 16) & 0xff) / 42;
    const uint8_t g = ((color888 >> 8)  & 0xff) / 42;

    const uint8_t c_hi = conv0[r] << 4 | conv0[g] << 2 | conv0[b];
    const uint8_t c_lo = conv1[r] << 4 | conv1[g] << 2 | conv1[b];

    pallette[i] = (c_hi << 8 | c_lo) & 0x3f3f | palette16_mask;
}

static void graphics_set_bgcolor_vga(uint32_t color888) {
    const uint8_t conv0[] = { 0b00, 0b00, 0b01, 0b10, 0b10, 0b10, 0b11, 0b11 };
    const uint8_t conv1[] = { 0b00, 0b01, 0b01, 0b01, 0b10, 0b11, 0b11, 0b11 };

    const uint8_t b = (color888         & 0xff) / 42;
    const uint8_t r = ((color888 >> 16) & 0xff) / 42;
    const uint8_t g = ((color888 >> 8)  & 0xff) / 42;

    const uint8_t c_hi = conv0[r] << 4 | conv0[g] << 2 | conv0[b];
    const uint8_t c_lo = conv1[r] << 4 | conv1[g] << 2 | conv1[b];
    bg_color[0] = ((c_hi << 8 | c_lo) & 0x3f3f | palette16_mask) << 16 |
                  ((c_hi << 8 | c_lo) & 0x3f3f | palette16_mask);
    bg_color[1] = ((c_lo << 8 | c_hi) & 0x3f3f | palette16_mask) << 16 |
                  ((c_lo << 8 | c_hi) & 0x3f3f | palette16_mask);
}

void graphics_set_palette(uint8_t i, uint32_t color888) {
    if (SELECT_VGA) graphics_set_palette_vga(i, color888);
    else            graphics_set_palette_hdmi(i, color888);
}

void graphics_set_bgcolor(uint32_t color888) {
    if (SELECT_VGA) graphics_set_bgcolor_vga(color888);
    else            graphics_set_bgcolor_hdmi(color888);
}

static void graphics_init_vga(void) {
    /* Cache the active video mode in SRAM so the ISR never reads from
     * flash. Matches the HDMI driver's __scratch_y placement. */
    {
        struct video_mode_t vm = graphics_get_video_mode(get_video_mode());
        vga_h_total = vm.h_total;
        vga_h_width = vm.h_width;
    }

    /* Seed text palette (idx 0..15). VGA here only runs in graphics mode
     * but keeping the palette sane simplifies any future text overlay. */
    for (int i = 0; i < 16; i++) {
        const uint8_t b = i & 1 ? (i >> 3 ? 3 : 2) : 0;
        const uint8_t r = i & 4 ? (i >> 3 ? 3 : 2) : 0;
        const uint8_t g = i & 2 ? (i >> 3 ? 3 : 2) : 0;
        const uint8_t c = r << 4 | g << 2 | b;
        pallette[i] = c & 0x3f | 0xc0;
    }

#if VGA_BASE_PIN >= 32
    pio_set_gpio_base(PIO_VGA, 32);
#elif VGA_BASE_PIN >= 16
    pio_set_gpio_base(PIO_VGA, 16);
#endif

    /* PIO program + SM claim. */
    const uint offset = pio_add_program(PIO_VGA, &pio_program_VGA);
    _SM_VGA = pio_claim_unused_sm(PIO_VGA, true);
    const uint sm = _SM_VGA;

    for (int i = 0; i < 8; i++) {
        gpio_init(VGA_BASE_PIN + i);
        gpio_set_dir(VGA_BASE_PIN + i, GPIO_OUT);
        pio_gpio_init(PIO_VGA, VGA_BASE_PIN + i);
    }
    pio_sm_set_consecutive_pindirs(PIO_VGA, sm, VGA_BASE_PIN, 8, true);

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + 0, offset + (pio_program_VGA.length - 1));
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_out_shift(&c, true, true, 32);
    sm_config_set_out_pins(&c, VGA_BASE_PIN, 8);
    pio_sm_init(PIO_VGA, sm, offset, &c);
    pio_sm_set_enabled(PIO_VGA, sm, true);

    /* DMA channels. */
    dma_chan_ctrl_vga = dma_claim_unused_channel(true);
    dma_chan_vga      = dma_claim_unused_channel(true);

    /* Main DMA channel: FIFO feed, chains to ctrl to restart with new src. */
    dma_channel_config c0 = dma_channel_get_default_config(dma_chan_vga);
    channel_config_set_transfer_data_size(&c0, DMA_SIZE_32);
    channel_config_set_read_increment(&c0, true);
    channel_config_set_write_increment(&c0, false);

    uint dreq = DREQ_PIO1_TX0 + sm;
    if (PIO_VGA == pio0) dreq = DREQ_PIO0_TX0 + sm;
    channel_config_set_dreq(&c0, dreq);
    channel_config_set_chain_to(&c0, dma_chan_ctrl_vga);

    dma_channel_configure(
        dma_chan_vga,
        &c0,
        &PIO_VGA->txf[sm],        /* write */
        lines_pattern[0],          /* read  (updated once set_mode runs) */
        600 / 4,
        false
    );

    /* Control channel: rewrites the main channel's read_addr from the
     * lines_pattern[] pointer that dma_handler_VGA selected. */
    dma_channel_config c1 = dma_channel_get_default_config(dma_chan_ctrl_vga);
    channel_config_set_transfer_data_size(&c1, DMA_SIZE_32);
    channel_config_set_read_increment(&c1, false);
    channel_config_set_write_increment(&c1, false);
    channel_config_set_chain_to(&c1, dma_chan_vga);

    dma_channel_configure(
        dma_chan_ctrl_vga,
        &c1,
        &dma_hw->ch[dma_chan_vga].read_addr, /* write */
        &lines_pattern[0],                    /* read  */
        1,
        false
    );

    /* Build line templates + clock divider. */
    graphics_set_mode_vga(GRAPHICSMODE_DEFAULT);

    /* Hook ISR on DMA_IRQ_1 (audio owns DMA_IRQ_0). */
    irq_set_exclusive_handler(VGA_DMA_IRQ, dma_handler_VGA);
    if (VGA_DMA_IRQ == DMA_IRQ_0) {
        dma_channel_set_irq0_enabled(dma_chan_ctrl_vga, true);
    } else {
        dma_channel_set_irq1_enabled(dma_chan_ctrl_vga, true);
    }
    irq_set_enabled(VGA_DMA_IRQ, true);
    dma_start_channel_mask(1u << dma_chan_vga);
}

/* ==========================================================================
 * Top-level entry point used by main.c — replaces the HDMI-only wrapper
 * that used to live in HDMI.c (the stub there is now removed).
 * ========================================================================== */
void graphics_init(g_out g_out) {
    (void)g_out;
    if (SELECT_VGA) graphics_init_vga();
    else            graphics_init_hdmi();
}
