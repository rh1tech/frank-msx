/*
 * msx_boot.c — boot-time splash and fatal-error screens.
 *
 * Draws directly into SCREEN[] before InitMachine() has been called.
 * Uses ui_draw's 6x8 font (for body text) and a 3x-scaled version of
 * the same font for the "FRANK MSX" title.
 *
 * Palette slots we touch here are all in the 230..249 range so they
 * don't collide with:
 *   - fMSX's 16-color palette (0..15)
 *   - our loader UI palette (243..247, see ui_draw.h)
 *   - HDMI sync control entries (250..253)
 */

#include "msx_boot.h"
#include "ui_draw.h"
#include "board_config.h"
#include "HDMI.h"
#include "ff.h"
#include "usbhid_wrapper.h"
#include "ps2kbd_wrapper.h"
#include "nespad/nespad.h"

#include "pico/stdlib.h"
#include "pico/time.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ---- Framebuffer access -------------------------------------------- */

#define FB_W  MSX_FB_WIDTH   /* 256 */
#define FB_H  MSX_FB_HEIGHT  /* 228 */

extern uint8_t         *SCREEN[2];
extern volatile uint32_t current_buffer;

/* ---- Palette slots reserved for the boot screens ------------------- */

/* Kept well below 243 (UI_COLOR_DIM) so they can't be overwritten by
 * ui_draw_install_palette() later on the emulator path. */
#define BOOT_COLOR_BG        230   /* deep navy background */
#define BOOT_COLOR_STAR_FAR  231   /* dim blue-grey */
#define BOOT_COLOR_STAR_MID  232   /* light blue */
#define BOOT_COLOR_STAR_NEAR 233   /* white */
#define BOOT_COLOR_LOGO      234   /* warm white */
#define BOOT_COLOR_LOGO_SH   235   /* near-black drop shadow */
#define BOOT_COLOR_SUBTLE    236   /* secondary text */
#define BOOT_COLOR_BLINK     237   /* "press any key" blink */
#define BOOT_COLOR_ERR_BG    238   /* error background (slightly red) */
#define BOOT_COLOR_ERR_HI    239   /* error headline */
#define BOOT_COLOR_ERR_TXT   240   /* error body */
#define BOOT_COLOR_ERR_SUB   241   /* error tertiary */

static void install_boot_palette(void) {
    graphics_set_palette(BOOT_COLOR_BG,        0x0a0e1c);
    graphics_set_palette(BOOT_COLOR_STAR_FAR,  0x3a4056);
    graphics_set_palette(BOOT_COLOR_STAR_MID,  0x8088a8);
    graphics_set_palette(BOOT_COLOR_STAR_NEAR, 0xf0f0f0);
    graphics_set_palette(BOOT_COLOR_LOGO,      0xffffff);
    graphics_set_palette(BOOT_COLOR_LOGO_SH,   0x000000);
    graphics_set_palette(BOOT_COLOR_SUBTLE,    0x9fb2d0);
    graphics_set_palette(BOOT_COLOR_BLINK,     0xffd060);
    graphics_set_palette(BOOT_COLOR_ERR_BG,    0x280808);
    graphics_set_palette(BOOT_COLOR_ERR_HI,    0xffe060);
    graphics_set_palette(BOOT_COLOR_ERR_TXT,   0xf0e0d0);
    graphics_set_palette(BOOT_COLOR_ERR_SUB,   0xa08070);
}

/* ---- Drawing primitives ------------------------------------------- */

static void fb_fill(uint8_t *fb, uint8_t color) {
    memset(fb, color, (size_t)FB_W * FB_H);
}

static inline void fb_pixel(uint8_t *fb, int x, int y, uint8_t color) {
    if ((unsigned)x >= (unsigned)FB_W) return;
    if ((unsigned)y >= (unsigned)FB_H) return;
    fb[y * FB_W + x] = color;
}

static void fb_hline(uint8_t *fb, int x, int y, int w, uint8_t color) {
    if (y < 0 || y >= FB_H) return;
    int x0 = x < 0 ? 0 : x;
    int x1 = x + w > FB_W ? FB_W : x + w;
    if (x1 > x0) memset(&fb[y * FB_W + x0], color, (size_t)(x1 - x0));
}

static void fb_rect_fill(uint8_t *fb, int x, int y, int w, int h, uint8_t color) {
    for (int j = 0; j < h; ++j) fb_hline(fb, x, y + j, w, color);
}

/* 1x draw of the ui_draw 6x8 font. (ui_draw_string draws at cell pitch
 * 6, we reuse that here by calling the low-level entry.) */
static void fb_text(uint8_t *fb, int x, int y, const char *s, uint8_t color) {
    ui_draw_string(fb, FB_W, x, y, s, color);
}

/* Centered 6x8 text at row `y`. */
static void fb_text_center(uint8_t *fb, int y, const char *s, uint8_t color) {
    int w = (int)strlen(s) * UI_CHAR_W;
    fb_text(fb, (FB_W - w) / 2, y, s, color);
}

/* Centered 6x8 text with a drop shadow — used over the starfield. */
static void fb_text_center_shadow(uint8_t *fb, int y, const char *s,
                                  uint8_t fg, uint8_t sh) {
    int w = (int)strlen(s) * UI_CHAR_W;
    int x = (FB_W - w) / 2;
    fb_text(fb, x + 1, y + 1, s, sh);
    fb_text(fb, x,     y,     s, fg);
}

/* Scaled-up glyph: draw a single char at `scale` size. Uses the 6x8
 * font bitmap from ui_font_6x8. */
static void fb_char_scaled(uint8_t *fb, int x, int y, char c,
                           int scale, uint8_t color) {
    if (c < 32 || c > 126) return;
    const uint8_t *g = ui_font_6x8[(int)c - 32];
    for (int row = 0; row < UI_CHAR_H; ++row) {
        uint8_t bits = g[row];
        for (int col = 0; col < UI_CHAR_W; ++col) {
            if (bits & (0x80 >> col)) {
                for (int dy = 0; dy < scale; ++dy)
                    for (int dx = 0; dx < scale; ++dx)
                        fb_pixel(fb, x + col * scale + dx,
                                     y + row * scale + dy, color);
            }
        }
    }
}

static int fb_text_scaled_width(const char *s, int scale) {
    return (int)strlen(s) * UI_CHAR_W * scale;
}

/* Centered scaled text with drop shadow. */
static void fb_text_scaled_center(uint8_t *fb, int y, const char *s,
                                  int scale, uint8_t fg, uint8_t sh) {
    int w = fb_text_scaled_width(s, scale);
    int x = (FB_W - w) / 2;
    for (size_t i = 0; s[i]; ++i) {
        int cx = x + (int)i * UI_CHAR_W * scale;
        /* Two-pixel drop shadow (proportional to scale). */
        fb_char_scaled(fb, cx + scale, y + scale, s[i], scale, sh);
        fb_char_scaled(fb, cx,          y,         s[i], scale, fg);
    }
}

/* ---- Input probing ------------------------------------------------ */

/* Returns true if any host input device registered a key/button press
 * since the last call. Drains the queues so we don't accumulate. */
static bool any_input_pressed(void) {
    bool pressed = false;

    /* PS/2 keyboard queue. */
    ps2kbd_tick();
    int down; unsigned char sc;
    while (ps2kbd_get_key(&down, &sc)) {
        if (down) pressed = true;
    }
    /* USB HID keyboard queue. */
    usbhid_wrapper_tick();
    while (usbhid_wrapper_get_key(&down, &sc)) {
        if (down) pressed = true;
    }
    /* USB gamepads — any button. */
    if (usbhid_wrapper_get_joystick() != 0) pressed = true;

    /* NES/SNES gamepad — any bit set in either pad. */
#ifdef NESPAD_GPIO_CLK
    nespad_read();
    if (nespad_state != 0 || nespad_state2 != 0) pressed = true;
#endif
    return pressed;
}

/* ---- Frame presentation -------------------------------------------
 *
 * Double-buffer protocol (matches platform.c:PutImage):
 *   - PIO HDMI scanner reads SCREEN[!current_buffer] every scanline
 *     (drivers/HDMI.c:407).
 *   - So SCREEN[current_buffer] is the BACK buffer we can safely draw
 *     into without tearing.
 *   - To present, flip current_buffer — the freshly-drawn buffer is
 *     now SCREEN[!current_buffer] and becomes the one being scanned.
 *
 * We also call graphics_set_buffer() to keep HSTX / VGA paths in
 * sync (they honor the pointer even though PIO HDMI doesn't). */
static uint8_t *boot_back(void) {
    return SCREEN[current_buffer];
}

static void boot_flip(void) {
    uint32_t next = current_buffer ^ 1;
    graphics_set_buffer(SCREEN[current_buffer]);
    current_buffer = next;
}

static void boot_clear_front(uint8_t color) {
    /* One-time initial paint of the front buffer so the first frame
     * doesn't show whatever was in RAM before. */
    memset(SCREEN[!current_buffer], color, (size_t)FB_W * FB_H);
}

static void sleep_vsync(void) {
    /* Match ~60 fps — we don't have a real vsync hook here yet. */
    sleep_us(16666);
}

/* ---- Starfield ---------------------------------------------------- */

#define STAR_COUNT 48

typedef struct { int32_t x, y; int16_t vx; uint8_t color; } star_t;
static star_t s_stars[STAR_COUNT];
static uint32_t s_rng = 0xC0FFEE17u;

static uint32_t star_rng(void) {
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng;
}

static void init_starfield(void) {
    const uint8_t tier_color[3] = {
        BOOT_COLOR_STAR_FAR,
        BOOT_COLOR_STAR_MID,
        BOOT_COLOR_STAR_NEAR,
    };
    /* Q8.8 velocity per frame — three parallax tiers, right-to-left. */
    const int16_t tier_vx[3] = { -24, -64, -128 };
    for (int i = 0; i < STAR_COUNT; ++i) {
        int tier = (int)(star_rng() % 3);
        s_stars[i].x  = (int32_t)(star_rng() % (FB_W << 8));
        s_stars[i].y  = (int32_t)(star_rng() % (FB_H << 8));
        s_stars[i].vx = tier_vx[tier];
        s_stars[i].color = tier_color[tier];
    }
}

static void tick_starfield(uint8_t *fb) {
    for (int i = 0; i < STAR_COUNT; ++i) {
        s_stars[i].x += s_stars[i].vx;
        if (s_stars[i].x < 0) s_stars[i].x += (FB_W << 8);
        int sx = s_stars[i].x >> 8;
        int sy = s_stars[i].y >> 8;
        fb_pixel(fb, sx, sy, s_stars[i].color);
    }
}

/* ---- Welcome screen ----------------------------------------------- */

void msx_boot_welcome(uint32_t timeout_ms) {
    install_boot_palette();
    init_starfield();
    boot_clear_front(BOOT_COLOR_BG);

    uint64_t t0 = time_us_64();
    uint32_t frame = 0;

    /* Ignore whatever state the PS/2 line is in at boot so we don't
     * immediately trip on a phantom key. */
    (void)any_input_pressed();

    while (true) {
        uint64_t now = time_us_64();
        if ((now - t0) / 1000 >= timeout_ms) break;

        uint8_t *fb = boot_back();
        fb_fill(fb, BOOT_COLOR_BG);
        tick_starfield(fb);

        /* "FRANK MSX" large, centered around the upper-middle band.
         * 9 glyphs × 6 px × 4 scale = 216 px wide, fits in 256. */
        fb_text_scaled_center(fb, 72, "FRANK MSX", 4,
                              BOOT_COLOR_LOGO, BOOT_COLOR_LOGO_SH);

        /* Version / byline. */
#ifdef FRANK_MSX_VERSION
        char vline[24];
        snprintf(vline, sizeof(vline), "v%s", FRANK_MSX_VERSION);
        fb_text_center_shadow(fb, 120, vline,
                              BOOT_COLOR_SUBTLE, BOOT_COLOR_LOGO_SH);
#endif
        fb_text_center_shadow(fb, 140, "fMSX 6.0 port for RP2350",
                              BOOT_COLOR_SUBTLE, BOOT_COLOR_LOGO_SH);
        fb_text_center_shadow(fb, 154, "by Mikhail Matveev",
                              BOOT_COLOR_SUBTLE, BOOT_COLOR_LOGO_SH);
        fb_text_center_shadow(fb, 168, "github.com/rh1tech/frank-msx",
                              BOOT_COLOR_SUBTLE, BOOT_COLOR_LOGO_SH);

        /* Blinking hint after ~1 second so users see it after the
         * animation has settled. */
        if (frame >= 60 && ((frame / 30) & 1) == 0) {
            fb_text_center_shadow(fb, 200, "PRESS ANY KEY",
                                  BOOT_COLOR_BLINK, BOOT_COLOR_LOGO_SH);
        }

        /* Present the finished frame, then fall through to input
         * polling while the scanner reads it. */
        boot_flip();
        ++frame;

        /* Check input only after a brief settle so stale bits don't
         * skip the splash. */
        if (frame >= 30 && any_input_pressed()) break;

        sleep_vsync();
    }
}

/* ---- Fatal error screen ------------------------------------------- */

_Noreturn void msx_boot_fatal(msx_boot_err_t reason, const char *detail) {
    install_boot_palette();

    const char *headline;
    const char *line1;
    const char *line2;
    switch (reason) {
        case MSX_BOOT_ERR_NO_SD:
            headline = "SD CARD NOT FOUND";
            line1    = "Insert a FAT32-formatted microSD";
            line2    = "with /MSX/ holding BIOS ROMs.";
            break;
        case MSX_BOOT_ERR_NO_DIR:
            headline = "/MSX DIRECTORY MISSING";
            line1    = "Create a /MSX folder on the SD card";
            line2    = "and drop BIOS ROMs + games inside.";
            break;
        case MSX_BOOT_ERR_NO_BIOS:
        default:
            headline = "REQUIRED BIOS ROM MISSING";
            line1    = "Copy the BIOS ROM(s) into /MSX/";
            line2    = "(see README for the exact list).";
            break;
    }

    /* Paint once into the back buffer, then flip. The scanner will
     * keep reading the now-front buffer forever; we never touch it
     * again. */
    uint8_t *fb = boot_back();
    fb_fill(fb, BOOT_COLOR_ERR_BG);

    /* Top band with the headline. */
    fb_rect_fill(fb, 0, 16, FB_W, 20, BOOT_COLOR_ERR_HI);
    {
        /* Render headline in inverted color inside the band. */
        int w = (int)strlen(headline) * UI_CHAR_W * 2;
        int x = (FB_W - w) / 2;
        for (size_t i = 0; headline[i]; ++i)
            fb_char_scaled(fb, x + (int)i * UI_CHAR_W * 2, 18,
                           headline[i], 2, BOOT_COLOR_ERR_BG);
    }

    int y = 64;
    fb_text_center(fb, y, line1, BOOT_COLOR_ERR_TXT); y += 16;
    fb_text_center(fb, y, line2, BOOT_COLOR_ERR_TXT); y += 24;

    if (detail && *detail) {
        fb_text_center(fb, y, "Missing:", BOOT_COLOR_ERR_SUB); y += 12;
        fb_text_center(fb, y, detail, BOOT_COLOR_ERR_HI);      y += 20;
    }

    /* Bottom hint. */
    fb_text_center(fb, FB_H - 24,
                   "Power-cycle after fixing the card.",
                   BOOT_COLOR_ERR_SUB);

    boot_flip();

    /* Spin forever — the user has to reboot. */
    while (true) tight_loop_contents();
}

/* ---- Boot-time sanity checks -------------------------------------- */

/* Return true if `path` exists as a regular file. */
static bool file_exists(const char *path) {
    FILINFO fno;
    return f_stat(path, &fno) == FR_OK && !(fno.fattrib & AM_DIR);
}

/* Return true if `path` exists as a directory. */
static bool dir_exists(const char *path) {
    FILINFO fno;
    return f_stat(path, &fno) == FR_OK && (fno.fattrib & AM_DIR);
}

void msx_boot_require_bios(bool sd_mounted, int model) {
    if (!sd_mounted) msx_boot_fatal(MSX_BOOT_ERR_NO_SD, NULL);

    if (!dir_exists("/MSX"))
        msx_boot_fatal(MSX_BOOT_ERR_NO_DIR, NULL);

    /* Different models need different BIOS blobs. fMSX looks them up
     * by exact name with no case-folding on FatFS, so the names have
     * to match what the README documents. */
    const char *missing = NULL;
    switch (model) {
        case 1:
            if (!file_exists("/MSX/MSX.ROM"))
                missing = "MSX.ROM";
            break;
        case 2:
            if (!file_exists("/MSX/MSX2.ROM"))
                missing = "MSX2.ROM";
            else if (!file_exists("/MSX/MSX2EXT.ROM"))
                missing = "MSX2EXT.ROM";
            break;
        case 3:
        default:
            if (!file_exists("/MSX/MSX2P.ROM"))
                missing = "MSX2P.ROM";
            else if (!file_exists("/MSX/MSX2PEXT.ROM"))
                missing = "MSX2PEXT.ROM";
            break;
    }
    if (missing) msx_boot_fatal(MSX_BOOT_ERR_NO_BIOS, missing);
}
