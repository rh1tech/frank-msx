/*
 * frank-msx — fMSX for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-msx
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * msx_settings.c — implementation of the runtime settings store.
 *
 * Live state is in g_settings. On boot we call msx_settings_load() to
 * pull overrides from /MSX/msx.ini; on Apply-and-Reset / any live
 * change we call msx_settings_save() so the next power-on matches.
 */

#include "msx_settings.h"
#include "board_config.h"   /* brings in HAS_I2S / HAS_PWM / platform defines */
#include "msx_tape.h"
#include "MSX.h"
#include "HDMI.h"
#include "ff.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

msx_settings_t g_settings = {
    .model        = 2,                 /* MSX2+ — matches main.c default */
    .region       = 1,                 /* PAL */
    .ram          = 3,                 /* 512 kB */
    .vram         = 4,                 /* 512 kB */
    .joy1         = 1,                 /* Joystick */
    .joy2         = 1,                 /* Joystick */
    .scanlines    = MSX_SCAN_OFF,
    .color        = MSX_COLOR_NORMAL,
#if defined(HDMI_HSTX)
    .audio_mode   = MSX_AUDIO_HDMI,
#elif defined(HAS_I2S)
    .audio_mode   = MSX_AUDIO_I2S,
#else
    .audio_mode   = MSX_AUDIO_PWM,
#endif
    .frameskip    = 0,
    .all_sprites  = 0,
    .autofire_a   = 0,
    .autofire_b   = 0,
    .autospace    = 0,
    .fixed_font   = 0,
    .fmpac        = 1,                 /* try to load FMPAC.ROM on boot */
    .cheats       = 0,
    .turbo_tape   = 0,                 /* real-time tape waveform off */
};

/* ---- value tables ---------------------------------------------------- */

static const char *MODEL_LABELS[]  = { "MSX1", "MSX2", "MSX2+" };
static const char *REGION_LABELS[] = { "NTSC", "PAL" };

static const int   RAM_PAGES[]   = { 4, 8, 16, 32 };
static const char *RAM_LABELS[]  = { "64 KB", "128 KB", "256 KB", "512 KB" };

static const int   VRAM_PAGES[]  = { 2, 4, 8, 16, 32 };
static const char *VRAM_LABELS[] = { "32 KB", "64 KB", "128 KB", "256 KB", "512 KB" };

static const char *JOY_LABELS[]  = {
    "Empty",
    "Joystick",
    "Mouse as Joystick",
    "Mouse",
};

static const char *SCANLINE_LABELS[MSX_SCAN_COUNT] = { "Off", "On" };

static const char *COLOR_LABELS[MSX_COLOR_COUNT] = {
    "Normal", "Monochrome", "Sepia", "Green", "Amber",
};

static const char *AUDIO_LABELS[MSX_AUDIO_COUNT] = {
    "HDMI", "I2S", "PWM", "Disabled",
};

static const char *FRAMESKIP_LABELS[] = {
    "None", "1/2", "1/3", "1/4", "1/5",
};

static const char *ONOFF_LABELS[] = { "Off", "On" };

/* Audio cycle per board. Same logic as before. */
#if defined(HDMI_HSTX)
static const uint8_t AUDIO_CYCLE[] = {
    MSX_AUDIO_HDMI, MSX_AUDIO_I2S, MSX_AUDIO_PWM, MSX_AUDIO_DISABLED,
};
#elif defined(HAS_I2S)
static const uint8_t AUDIO_CYCLE[] = {
    MSX_AUDIO_I2S, MSX_AUDIO_PWM, MSX_AUDIO_DISABLED,
};
#else
static const uint8_t AUDIO_CYCLE[] = {
    MSX_AUDIO_PWM, MSX_AUDIO_DISABLED,
};
#endif
#define AUDIO_CYCLE_LEN ((int)(sizeof(AUDIO_CYCLE) / sizeof(AUDIO_CYCLE[0])))

int msx_settings_choices(msx_setting_id_t id) {
    switch (id) {
        case MSX_SETTING_MODEL:       return (int)(sizeof(MODEL_LABELS)/sizeof(MODEL_LABELS[0]));
        case MSX_SETTING_REGION:      return (int)(sizeof(REGION_LABELS)/sizeof(REGION_LABELS[0]));
        case MSX_SETTING_RAM:         return (int)(sizeof(RAM_LABELS)/sizeof(RAM_LABELS[0]));
        case MSX_SETTING_VRAM:        return (int)(sizeof(VRAM_LABELS)/sizeof(VRAM_LABELS[0]));
        case MSX_SETTING_JOY1:
        case MSX_SETTING_JOY2:        return (int)(sizeof(JOY_LABELS)/sizeof(JOY_LABELS[0]));
        case MSX_SETTING_SCANLINES:   return MSX_SCAN_COUNT;
        case MSX_SETTING_COLOR:       return MSX_COLOR_COUNT;
        case MSX_SETTING_AUDIO:       return AUDIO_CYCLE_LEN;
        case MSX_SETTING_FRAMESKIP:   return (int)(sizeof(FRAMESKIP_LABELS)/sizeof(FRAMESKIP_LABELS[0]));
        case MSX_SETTING_ALLSPRITES:
        case MSX_SETTING_AUTOFIRE_A:
        case MSX_SETTING_AUTOFIRE_B:
        case MSX_SETTING_AUTOSPACE:
        case MSX_SETTING_FIXEDFONT:
        case MSX_SETTING_FMPAC:
        case MSX_SETTING_CHEATS:      return 2;
        default:                      return 0;
    }
}

const char *msx_settings_label(msx_setting_id_t id) {
    switch (id) {
        case MSX_SETTING_MODEL:       return "Model";
        case MSX_SETTING_REGION:      return "Region";
        case MSX_SETTING_RAM:         return "Main RAM";
        case MSX_SETTING_VRAM:        return "Video RAM";
        case MSX_SETTING_JOY1:        return "Joystick 1";
        case MSX_SETTING_JOY2:        return "Joystick 2";
        case MSX_SETTING_SCANLINES:   return "Scanlines";
        case MSX_SETTING_COLOR:       return "Color filter";
        case MSX_SETTING_AUDIO:       return "Audio";
        case MSX_SETTING_FRAMESKIP:   return "Frame skip";
        case MSX_SETTING_ALLSPRITES:  return "All sprites";
        case MSX_SETTING_AUTOFIRE_A:  return "Autofire A";
        case MSX_SETTING_AUTOFIRE_B:  return "Autofire B";
        case MSX_SETTING_AUTOSPACE:   return "Autofire Space";
        case MSX_SETTING_FIXEDFONT:   return "Fixed 8x8 font";
        case MSX_SETTING_FMPAC:       return "MSX-MUSIC (FMPAC)";
        case MSX_SETTING_CHEATS:      return "Cheats";
        default:                      return "?";
    }
}

const char *msx_settings_value_label(msx_setting_id_t id) {
    switch (id) {
        case MSX_SETTING_MODEL:       return MODEL_LABELS[g_settings.model];
        case MSX_SETTING_REGION:      return REGION_LABELS[g_settings.region];
        case MSX_SETTING_RAM:         return RAM_LABELS[g_settings.ram];
        case MSX_SETTING_VRAM:        return VRAM_LABELS[g_settings.vram];
        case MSX_SETTING_JOY1:        return JOY_LABELS[g_settings.joy1];
        case MSX_SETTING_JOY2:        return JOY_LABELS[g_settings.joy2];
        case MSX_SETTING_SCANLINES:   return SCANLINE_LABELS[g_settings.scanlines];
        case MSX_SETTING_COLOR:       return COLOR_LABELS[g_settings.color];
        case MSX_SETTING_AUDIO:
            if (g_settings.audio_mode < MSX_AUDIO_COUNT)
                return AUDIO_LABELS[g_settings.audio_mode];
            return "?";
        case MSX_SETTING_FRAMESKIP:   return FRAMESKIP_LABELS[g_settings.frameskip];
        case MSX_SETTING_ALLSPRITES:  return ONOFF_LABELS[g_settings.all_sprites & 1];
        case MSX_SETTING_AUTOFIRE_A:  return ONOFF_LABELS[g_settings.autofire_a & 1];
        case MSX_SETTING_AUTOFIRE_B:  return ONOFF_LABELS[g_settings.autofire_b & 1];
        case MSX_SETTING_AUTOSPACE:   return ONOFF_LABELS[g_settings.autospace & 1];
        case MSX_SETTING_FIXEDFONT:   return ONOFF_LABELS[g_settings.fixed_font & 1];
        case MSX_SETTING_FMPAC:       return ONOFF_LABELS[g_settings.fmpac & 1];
        case MSX_SETTING_CHEATS:      return ONOFF_LABELS[g_settings.cheats & 1];
        default:                      return "?";
    }
}

bool msx_settings_needs_reset(msx_setting_id_t id) {
    switch (id) {
        case MSX_SETTING_MODEL:
        case MSX_SETTING_REGION:
        case MSX_SETTING_RAM:
        case MSX_SETTING_VRAM:
        case MSX_SETTING_JOY1:
        case MSX_SETTING_JOY2:
        case MSX_SETTING_FMPAC:
            return true;
        default:
            return false;
    }
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
        case MSX_SETTING_MODEL:       step_u8(&g_settings.model,       delta, n); break;
        case MSX_SETTING_REGION:      step_u8(&g_settings.region,      delta, n); break;
        case MSX_SETTING_RAM:         step_u8(&g_settings.ram,         delta, n); break;
        case MSX_SETTING_VRAM:        step_u8(&g_settings.vram,        delta, n); break;
        case MSX_SETTING_JOY1:        step_u8(&g_settings.joy1,        delta, n); break;
        case MSX_SETTING_JOY2:        step_u8(&g_settings.joy2,        delta, n); break;
        case MSX_SETTING_SCANLINES:   step_u8(&g_settings.scanlines,   delta, n);
                                      msx_settings_apply_visual();                break;
        case MSX_SETTING_COLOR:       step_u8(&g_settings.color,       delta, n);
                                      msx_settings_apply_visual();                break;
        case MSX_SETTING_FRAMESKIP:   step_u8(&g_settings.frameskip,   delta, n);
                                      msx_settings_apply_visual();                break;
        case MSX_SETTING_ALLSPRITES:  step_u8(&g_settings.all_sprites, delta, n);
                                      msx_settings_apply_visual();                break;
        case MSX_SETTING_AUTOFIRE_A:  step_u8(&g_settings.autofire_a,  delta, n);
                                      msx_settings_apply_visual();                break;
        case MSX_SETTING_AUTOFIRE_B:  step_u8(&g_settings.autofire_b,  delta, n);
                                      msx_settings_apply_visual();                break;
        case MSX_SETTING_AUTOSPACE:   step_u8(&g_settings.autospace,   delta, n);
                                      msx_settings_apply_visual();                break;
        case MSX_SETTING_FIXEDFONT:   step_u8(&g_settings.fixed_font,  delta, n);
                                      msx_settings_apply_visual();                break;
        case MSX_SETTING_FMPAC:       step_u8(&g_settings.fmpac,       delta, n); break;
        case MSX_SETTING_CHEATS:      step_u8(&g_settings.cheats,      delta, n);
                                      msx_settings_apply_visual();                break;
        case MSX_SETTING_AUDIO: {
            int idx = 0;
            for (int i = 0; i < AUDIO_CYCLE_LEN; i++)
                if (AUDIO_CYCLE[i] == g_settings.audio_mode) { idx = i; break; }
            idx = (idx + AUDIO_CYCLE_LEN + delta) % AUDIO_CYCLE_LEN;
            g_settings.audio_mode = AUDIO_CYCLE[idx];
            break;
        }
        default: break;
    }

    /* Persist every change so power-loss doesn't lose the setting.
     * The SD write is tens of ms at most — acceptable from a menu
     * key-press context. */
    msx_settings_save();
}

/* Externally-visible byte UPeriod lives in fMSX's MSX.c. */
extern byte UPeriod;

void msx_settings_apply_visual(void) {
    /* Scanlines: simple on/off today. */
    graphics_set_crt_active(g_settings.scanlines == MSX_SCAN_ON);

    /* Colour filter — re-tint every palette slot via platform.c. */
    platform_repaint_palette(g_settings.color);

    /* Frame skip: UPeriod is "percent of frames drawn", 1..100. */
    static const byte UPERIOD_FOR[] = { 100, 50, 33, 25, 20 };
    byte u = UPERIOD_FOR[g_settings.frameskip <= 4 ? g_settings.frameskip : 0];
    UPeriod = u;

    /* Mode option bits — MSX_ALLSPRITE / MSX_AUTOFIREA / MSX_AUTOFIREB /
     * MSX_AUTOSPACE / MSX_FIXEDFONT are sampled by the core on every
     * frame, so flipping them here takes effect immediately. */
    int m = Mode & ~(MSX_ALLSPRITE | MSX_AUTOFIREA | MSX_AUTOFIREB |
                     MSX_AUTOSPACE | MSX_FIXEDFONT);
    if (g_settings.all_sprites) m |= MSX_ALLSPRITE;
    if (g_settings.autofire_a)  m |= MSX_AUTOFIREA;
    if (g_settings.autofire_b)  m |= MSX_AUTOFIREB;
    if (g_settings.autospace)   m |= MSX_AUTOSPACE;
    if (g_settings.fixed_font)  m |= MSX_FIXEDFONT;
    Mode = m;

    /* Cheats master switch. Cheats(CHTS_ON/CHTS_OFF) flips CheatsON
     * and re-applies / reverts the active cheat patches in the MSX
     * address space. Safe to call every settings touch. */
    Cheats(g_settings.cheats ? 1 : 0);

    /* Tape waveform generator (turbo loader). Off by default so BIOS
     * TAPION traps work unmolested; flip on for games with custom
     * loaders that poll PSG[14] bit 7 directly. */
    msx_tape_set_waveform_enabled(g_settings.turbo_tape != 0);
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

    mode |= ((int)g_settings.joy1 & 0x03) << 4;
    mode |= ((int)g_settings.joy2 & 0x03) << 6;

    mode |= MSX_GUESSA | MSX_GUESSB;

    /* Include live option bits so StartMSX/ResetMSX see them. */
    if (g_settings.all_sprites) mode |= MSX_ALLSPRITE;
    if (g_settings.autofire_a)  mode |= MSX_AUTOFIREA;
    if (g_settings.autofire_b)  mode |= MSX_AUTOFIREB;
    if (g_settings.autospace)   mode |= MSX_AUTOSPACE;
    if (g_settings.fixed_font)  mode |= MSX_FIXEDFONT;

    if (mode_out)       *mode_out       = mode;
    if (ram_pages_out)  *ram_pages_out  = RAM_PAGES[g_settings.ram];
    if (vram_pages_out) *vram_pages_out = VRAM_PAGES[g_settings.vram];
}

void msx_settings_init_from_bootstate(void) {
    switch (Mode & MSX_MODEL) {
        case MSX_MSX1:  g_settings.model = 0; break;
        case MSX_MSX2:  g_settings.model = 1; break;
        default:        g_settings.model = 2; break;
    }
    g_settings.region = (Mode & MSX_PAL) ? 1 : 0;
    g_settings.joy1   = (Mode >> 4) & 0x03;
    g_settings.joy2   = (Mode >> 6) & 0x03;

    for (size_t i = 0; i < sizeof(RAM_PAGES)/sizeof(RAM_PAGES[0]); ++i)
        if (RAMPages == RAM_PAGES[i]) { g_settings.ram = (uint8_t)i; break; }
    for (size_t i = 0; i < sizeof(VRAM_PAGES)/sizeof(VRAM_PAGES[0]); ++i)
        if (VRAMPages == VRAM_PAGES[i]) { g_settings.vram = (uint8_t)i; break; }
}

extern int  msx_disk_flush_if_dirty(int drv);
extern void fmsx_save_cmos_if_dirty(void);
extern int  msx_sram_flush_slot(int slot);

static void flush_all_persistence(void) {
    /* Disks first — a dirty DSK surviving a reboot is the most
     * visible data-loss failure mode. */
    for (int d = 0; d < 2; ++d) {
        int r = msx_disk_flush_if_dirty(d);
        if (r < 0 && r != -2 && r != -3)
            printf("settings: disk %c flush returned %d\n", 'A' + d, r);
    }
    /* Cartridge SRAM (FMPAC, Konami ASCII/GameMaster2). Each slot
     * has its own .sav file keyed by cart path. */
    for (int s = 0; s < 6; ++s)
        (void)msx_sram_flush_slot(s);
    /* RTC / MSX2 CMOS. */
    fmsx_save_cmos_if_dirty();
}

void msx_settings_apply_and_reset(void) {
    int mode, ram, vram;
    msx_settings_compose(&mode, &ram, &vram);
    printf("settings: apply mode=0x%08X ram=%dp vram=%dp\n", mode, ram, vram);
    msx_settings_save();
    flush_all_persistence();
    ResetMSX(mode, ram, vram);
    /* Reapply live bits in case ResetMSX clears them. */
    msx_settings_apply_visual();
}

void msx_settings_request_reset_current(void) {
    printf("settings: reset (current mode=0x%08X)\n", Mode);
    flush_all_persistence();
    ResetMSX(Mode, RAMPages, VRAMPages);
    msx_settings_apply_visual();
}

/* Legacy no-ops. */
bool msx_settings_reset_pending(void) { return false; }
void msx_settings_pending_reset_values(int *m, int *r, int *v) {
    (void)m; (void)r; (void)v;
}
void msx_settings_clear_pending_reset(void) { }

/* ===================================================================
 * msx.ini persistence
 *
 * Plain key=value INI, one setting per line. Unknown keys are ignored,
 * so we can add fields later without breaking old files. Values that
 * parse out of range are clamped and printed to the serial log.
 *
 * Path:      /MSX/msx.ini
 * Format:    "key=value" (ASCII, LF terminated)
 * =================================================================== */

#define MSX_INI_PATH "/MSX/msx.ini"

/* Integer fields. Each entry maps a key name to a pointer in
 * g_settings plus a clamp range [0..max). */
typedef struct {
    const char *key;
    uint8_t    *slot;
    uint8_t     max; /* exclusive upper bound */
} ini_field_t;

static const ini_field_t INI_FIELDS[] = {
    { "model",        &g_settings.model,       3  },
    { "region",       &g_settings.region,      2  },
    { "ram",          &g_settings.ram,         4  },
    { "vram",         &g_settings.vram,        5  },
    { "joy1",         &g_settings.joy1,        4  },
    { "joy2",         &g_settings.joy2,        4  },
    { "scanlines",    &g_settings.scanlines,   MSX_SCAN_COUNT  },
    { "color",        &g_settings.color,       MSX_COLOR_COUNT },
    { "audio",        &g_settings.audio_mode,  MSX_AUDIO_COUNT },
    { "frameskip",    &g_settings.frameskip,   5  },
    { "all_sprites",  &g_settings.all_sprites, 2  },
    { "autofire_a",   &g_settings.autofire_a,  2  },
    { "autofire_b",   &g_settings.autofire_b,  2  },
    { "autospace",    &g_settings.autospace,   2  },
    { "fixed_font",   &g_settings.fixed_font,  2  },
    { "fmpac",        &g_settings.fmpac,       2  },
    { "cheats",       &g_settings.cheats,      2  },
    { "turbo_tape",   &g_settings.turbo_tape,  2  },
};
#define INI_FIELD_COUNT ((int)(sizeof(INI_FIELDS)/sizeof(INI_FIELDS[0])))

static void ini_trim(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n-1])) s[--n] = 0;
}

bool msx_settings_load(void) {
    FIL f;
    FRESULT fr = f_open(&f, MSX_INI_PATH, FA_READ);
    if (fr != FR_OK) {
        printf("settings: no msx.ini (fr=%d), using defaults\n", fr);
        return false;
    }

    char line[128];
    int applied = 0, skipped = 0;
    while (f_gets(line, sizeof(line), &f)) {
        ini_trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;
        char *eq = strchr(line, '=');
        if (!eq) { ++skipped; continue; }
        *eq = 0;
        char *key = line;
        char *val = eq + 1;
        ini_trim(key);
        ini_trim(val);

        int handled = 0;
        for (int i = 0; i < INI_FIELD_COUNT; ++i) {
            if (strcmp(key, INI_FIELDS[i].key) == 0) {
                char *endp = NULL;
                long v = strtol(val, &endp, 10);
                if (endp && endp != val && v >= 0 && v < INI_FIELDS[i].max) {
                    *INI_FIELDS[i].slot = (uint8_t)v;
                    ++applied;
                } else {
                    printf("settings: '%s' out of range: '%s' (max %d)\n",
                           key, val, INI_FIELDS[i].max);
                    ++skipped;
                }
                handled = 1;
                break;
            }
        }
        if (!handled) {
            printf("settings: unknown key '%s' (ignored)\n", key);
            ++skipped;
        }
    }
    f_close(&f);
    printf("settings: loaded %d keys, skipped %d from %s\n",
           applied, skipped, MSX_INI_PATH);
    return true;
}

bool msx_settings_save(void) {
    FIL f;
    FRESULT fr = f_open(&f, MSX_INI_PATH, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        printf("settings: save failed to open %s (fr=%d)\n", MSX_INI_PATH, fr);
        return false;
    }

    char buf[64];
    bool ok = true;
    int n = snprintf(buf, sizeof(buf),
                     "# frank-msx settings — edit carefully\n");
    UINT bw = 0;
    if (f_write(&f, buf, n, &bw) != FR_OK || (int)bw != n) ok = false;

    for (int i = 0; ok && i < INI_FIELD_COUNT; ++i) {
        n = snprintf(buf, sizeof(buf), "%s=%u\n",
                     INI_FIELDS[i].key, (unsigned)*INI_FIELDS[i].slot);
        if (f_write(&f, buf, n, &bw) != FR_OK || (int)bw != n) ok = false;
    }
    f_close(&f);
    if (!ok) {
        printf("settings: save failed writing %s\n", MSX_INI_PATH);
        return false;
    }
    return true;
}
