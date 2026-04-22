/*
 * msx_settings.c — implementation of the runtime settings store.
 */

#include "msx_settings.h"
#include "MSX.h"
#include "HDMI.h"

#include <string.h>
#include <stdio.h>

msx_settings_t g_settings = {
    .model     = 2,                 /* MSX2+ — matches main.c default */
    .region    = 1,                 /* PAL   — matches main.c default */
    .ram       = 3,                 /* 512 kB — matches main.c default */
    .vram      = 4,                 /* 512 kB — matches main.c default */
    .joy1      = 1,                 /* Joystick */
    .joy2      = 1,                 /* Joystick */
    .scanlines = MSX_SCAN_OFF,
    .color     = MSX_COLOR_NORMAL,
};

/* ---- value tables ---------------------------------------------------- */

static const char *MODEL_LABELS[]  = { "MSX1", "MSX2", "MSX2+" };
static const char *REGION_LABELS[] = { "NTSC", "PAL" };

/* Main RAM: page count = kB / 16. fMSX clamps to pow2 and to
 * MSX1>=4, MSX2/2+>=8, max 256. */
static const int   RAM_PAGES[]   = { 4, 8, 16, 32 };     /* 64, 128, 256, 512 kB */
static const char *RAM_LABELS[]  = { "64 KB", "128 KB", "256 KB", "512 KB" };

/* Video RAM: our fMSX patch raises the ceiling from 8 to 32 pages. */
static const int   VRAM_PAGES[]  = { 2, 4, 8, 16, 32 };  /* 32, 64, 128, 256, 512 kB */
static const char *VRAM_LABELS[] = { "32 KB", "64 KB", "128 KB", "256 KB", "512 KB" };

/* Index = fMSX's JOY_* encoding (see MSX.h: NONE=0, JOY=1,
 * MOUSTICK=2, MOUSE=3). Keep this order so the enum doubles as the
 * Mode-bits value. */
static const char *JOY_LABELS[]  = {
    "Empty",
    "Joystick",
    "Mouse as Joystick",
    "Mouse",
};

static const char *SCANLINE_LABELS[MSX_SCAN_COUNT] = {
    "Off",
    "On",
};

static const char *COLOR_LABELS[MSX_COLOR_COUNT] = {
    "Normal",
    "Monochrome",
    "Sepia",
    "Green",
    "Amber",
};

int msx_settings_choices(msx_setting_id_t id) {
    switch (id) {
        case MSX_SETTING_MODEL:     return (int)(sizeof(MODEL_LABELS) / sizeof(MODEL_LABELS[0]));
        case MSX_SETTING_REGION:    return (int)(sizeof(REGION_LABELS)/ sizeof(REGION_LABELS[0]));
        case MSX_SETTING_RAM:       return (int)(sizeof(RAM_LABELS)   / sizeof(RAM_LABELS[0]));
        case MSX_SETTING_VRAM:      return (int)(sizeof(VRAM_LABELS)  / sizeof(VRAM_LABELS[0]));
        case MSX_SETTING_JOY1:
        case MSX_SETTING_JOY2:      return (int)(sizeof(JOY_LABELS)   / sizeof(JOY_LABELS[0]));
        case MSX_SETTING_SCANLINES: return MSX_SCAN_COUNT;
        case MSX_SETTING_COLOR:     return MSX_COLOR_COUNT;
        default:                    return 0;
    }
}

const char *msx_settings_label(msx_setting_id_t id) {
    switch (id) {
        case MSX_SETTING_MODEL:     return "Model";
        case MSX_SETTING_REGION:    return "Region";
        case MSX_SETTING_RAM:       return "Main RAM";
        case MSX_SETTING_VRAM:      return "Video RAM";
        case MSX_SETTING_JOY1:      return "Joystick 1";
        case MSX_SETTING_JOY2:      return "Joystick 2";
        case MSX_SETTING_SCANLINES: return "Scanlines";
        case MSX_SETTING_COLOR:     return "Color filter";
        default:                    return "?";
    }
}

const char *msx_settings_value_label(msx_setting_id_t id) {
    switch (id) {
        case MSX_SETTING_MODEL:     return MODEL_LABELS[g_settings.model];
        case MSX_SETTING_REGION:    return REGION_LABELS[g_settings.region];
        case MSX_SETTING_RAM:       return RAM_LABELS[g_settings.ram];
        case MSX_SETTING_VRAM:      return VRAM_LABELS[g_settings.vram];
        case MSX_SETTING_JOY1:      return JOY_LABELS[g_settings.joy1];
        case MSX_SETTING_JOY2:      return JOY_LABELS[g_settings.joy2];
        case MSX_SETTING_SCANLINES: return SCANLINE_LABELS[g_settings.scanlines];
        case MSX_SETTING_COLOR:     return COLOR_LABELS[g_settings.color];
        default:                    return "?";
    }
}

bool msx_settings_needs_reset(msx_setting_id_t id) {
    /* Visual-only settings apply live; everything else rebuilds the
     * machine. */
    return !(id == MSX_SETTING_SCANLINES || id == MSX_SETTING_COLOR);
}

static void step_u8(uint8_t *v, int delta, int n) {
    int cur = (int)*v + delta;
    while (cur < 0)   cur += n;
    while (cur >= n)  cur -= n;
    *v = (uint8_t)cur;
}

void msx_settings_step(msx_setting_id_t id, int delta) {
    int n = msx_settings_choices(id);
    if (n <= 0) return;
    switch (id) {
        case MSX_SETTING_MODEL:     step_u8(&g_settings.model,     delta, n); break;
        case MSX_SETTING_REGION:    step_u8(&g_settings.region,    delta, n); break;
        case MSX_SETTING_RAM:       step_u8(&g_settings.ram,       delta, n); break;
        case MSX_SETTING_VRAM:      step_u8(&g_settings.vram,      delta, n); break;
        case MSX_SETTING_JOY1:      step_u8(&g_settings.joy1,      delta, n); break;
        case MSX_SETTING_JOY2:      step_u8(&g_settings.joy2,      delta, n); break;
        case MSX_SETTING_SCANLINES: step_u8(&g_settings.scanlines, delta, n);
                                    msx_settings_apply_visual();            break;
        case MSX_SETTING_COLOR:     step_u8(&g_settings.color,     delta, n);
                                    msx_settings_apply_visual();            break;
        default: break;
    }
}

void msx_settings_apply_visual(void) {
    /* Scanlines: simple on/off today — the HDMI driver has a single
     * line-blanker effect. */
    graphics_set_crt_active(g_settings.scanlines == MSX_SCAN_ON);

    /* Colour filter -> re-tint every palette slot via platform.c. */
    platform_repaint_palette(g_settings.color);
}

/* ---- compose + apply ------------------------------------------------- */

void msx_settings_compose(int *mode_out, int *ram_pages_out, int *vram_pages_out) {
    int mode = 0;
    switch (g_settings.model) {
        case 0: mode |= MSX_MSX1;  break;
        case 1: mode |= MSX_MSX2;  break;
        default: mode |= MSX_MSX2P; break;
    }
    mode |= (g_settings.region == 1) ? MSX_PAL : MSX_NTSC;

    /* Joystick port bits live at SOCKET1 (bits 4-5) and SOCKET2 (6-7).
     * Values: 0=none, 1=joystick, 2=moustick, 3=mouse. */
    mode |= ((int)g_settings.joy1 & 0x03) << 4;
    mode |= ((int)g_settings.joy2 & 0x03) << 6;

    /* Mapper guessing for user cartridge slots stays on. */
    mode |= MSX_GUESSA | MSX_GUESSB;

    /* RAM / VRAM — resolved through the lookup tables. fMSX clamps
     * MSX1 RAM to >= 4 pages and MSX2/2+ RAM to >= 8, so small-RAM
     * choices on a bigger model are bumped up at Reset time. */
    if (mode_out)       *mode_out       = mode;
    if (ram_pages_out)  *ram_pages_out  = RAM_PAGES[g_settings.ram];
    if (vram_pages_out) *vram_pages_out = VRAM_PAGES[g_settings.vram];
}

/* Pull initial values from whatever main.c set before InitMachine.
 * The defaults above match the build-time #defines so if main.c
 * hasn't touched Mode yet, we agree. */
void msx_settings_init_from_bootstate(void) {
    switch (Mode & MSX_MODEL) {
        case MSX_MSX1:  g_settings.model = 0; break;
        case MSX_MSX2:  g_settings.model = 1; break;
        default:        g_settings.model = 2; break;
    }
    g_settings.region = (Mode & MSX_PAL) ? 1 : 0;
    g_settings.joy1   = (Mode >> 4) & 0x03;
    g_settings.joy2   = (Mode >> 6) & 0x03;

    /* Pick the RAM/VRAM choice that matches the live page count.
     * If no match (e.g. non-standard size), keep the existing
     * g_settings value rather than silently falling back to a
     * smaller one — falling back to 64 kB on MSX2+ triggers the
     * Vampire Killer mapper-range hang. */
    for (size_t i = 0; i < sizeof(RAM_PAGES)/sizeof(RAM_PAGES[0]); ++i)
        if (RAMPages == RAM_PAGES[i]) { g_settings.ram = (uint8_t)i; break; }
    for (size_t i = 0; i < sizeof(VRAM_PAGES)/sizeof(VRAM_PAGES[0]); ++i)
        if (VRAMPages == VRAM_PAGES[i]) { g_settings.vram = (uint8_t)i; break; }
}

void msx_settings_apply_and_reset(void) {
    int mode, ram, vram;
    msx_settings_compose(&mode, &ram, &vram);
    printf("settings: apply mode=0x%08X ram=%dp vram=%dp\n", mode, ram, vram);
    ResetMSX(mode, ram, vram);
}

void msx_settings_request_reset_current(void) {
    printf("settings: reset (current mode=0x%08X)\n", Mode);
    ResetMSX(Mode, RAMPages, VRAMPages);
}

/* Legacy pending-reset API — retained as no-ops so main.c still
 * builds. We now call ResetMSX() directly. */
bool msx_settings_reset_pending(void) { return false; }
void msx_settings_pending_reset_values(int *m, int *r, int *v) {
    (void)m; (void)r; (void)v;
}
void msx_settings_clear_pending_reset(void) { }
