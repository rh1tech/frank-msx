/*
 * frank-msx — fMSX for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-msx
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * msx_settings.h — mutable runtime settings for the fMSX core.
 *
 * Persisted to /MSX/msx.ini on the SD card (see msx_settings_save /
 * msx_settings_load). Changing model / region / RAM / VRAM / joystick
 * requires ResetMSX(). Live-apply settings (scanlines, colour filter,
 * audio mode, frame skip, sprites, autofire, font, FMPAC) take effect
 * immediately.
 */
#ifndef MSX_SETTINGS_H
#define MSX_SETTINGS_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    MSX_SETTING_MODEL = 0,    /* MSX1 / MSX2 / MSX2+        (needs reset) */
    MSX_SETTING_REGION,       /* NTSC / PAL                 (needs reset) */
    MSX_SETTING_RAM,          /* 64/128/256/512 KB          (needs reset) */
    MSX_SETTING_VRAM,         /* 32/64/128/256/512 KB       (needs reset) */
    MSX_SETTING_JOY1,         /* Empty/Joystick/Mouse/...   (needs reset) */
    MSX_SETTING_JOY2,
    MSX_SETTING_SCANLINES,    /* None / TV                  (live)       */
    MSX_SETTING_COLOR,        /* Normal / Mono / Sepia / Green / Amber   */
    MSX_SETTING_AUDIO,        /* HDMI / I2S / PWM / Disabled (live)      */
    MSX_SETTING_FRAMESKIP,    /* 0..4 skipped frames         (live)      */
    MSX_SETTING_ALLSPRITES,   /* Off / On                    (live)      */
    MSX_SETTING_AUTOFIRE_A,   /* Off / On                    (live)      */
    MSX_SETTING_AUTOFIRE_B,   /* Off / On                    (live)      */
    MSX_SETTING_AUTOSPACE,    /* Off / On                    (live)      */
    MSX_SETTING_FIXEDFONT,    /* Off / On                    (live)      */
    MSX_SETTING_FMPAC,        /* Off / On (MSX-MUSIC ROM)    (needs reset)*/
    MSX_SETTING_CHEATS,       /* Off / On                    (live)      */
    MSX_SETTING_COUNT
} msx_setting_id_t;

/* Audio-output backend. HDMI is only reachable on HSTX builds — the
 * PIO HDMI path never carries audio. I2S targets the external DAC on
 * GP9/10/11; PWM drives GP10/11 through an RC filter. Both SHARE the
 * same GPIOs, so switching between them re-configures pin function. */
typedef enum {
    MSX_AUDIO_HDMI = 0,
    MSX_AUDIO_I2S,
    MSX_AUDIO_PWM,
    MSX_AUDIO_DISABLED,
    MSX_AUDIO_COUNT
} msx_audio_mode_t;

typedef enum {
    MSX_SCAN_OFF = 0,
    MSX_SCAN_ON,
    MSX_SCAN_COUNT
} msx_scanlines_t;

typedef enum {
    MSX_COLOR_NORMAL = 0,
    MSX_COLOR_MONO,
    MSX_COLOR_SEPIA,
    MSX_COLOR_GREEN,
    MSX_COLOR_AMBER,
    MSX_COLOR_COUNT
} msx_color_filter_t;

typedef struct {
    uint8_t model;       /* 0=MSX1, 1=MSX2, 2=MSX2+ */
    uint8_t region;      /* 0=NTSC, 1=PAL */
    uint8_t ram;         /* index into RAM_CHOICES */
    uint8_t vram;        /* index into VRAM_CHOICES */
    uint8_t joy1;        /* 0..3 */
    uint8_t joy2;        /* 0..3 */
    uint8_t scanlines;   /* msx_scanlines_t */
    uint8_t color;       /* msx_color_filter_t */
    uint8_t audio_mode;  /* msx_audio_mode_t */
    uint8_t frameskip;   /* 0..4 — draws every (frameskip+1)-th frame */
    uint8_t all_sprites; /* 0/1 — disable sprites-per-line limit */
    uint8_t autofire_a;  /* 0/1 */
    uint8_t autofire_b;  /* 0/1 */
    uint8_t autospace;   /* 0/1 */
    uint8_t fixed_font;  /* 0/1 — MSX_FIXEDFONT + DEFAULT.FNT */
    uint8_t fmpac;       /* 0/1 — attempt FMPAC.ROM load on reset */
    uint8_t cheats;      /* 0/1 — master cheat enable */
    uint8_t turbo_tape;  /* 0/1 — real-time tape waveform generator.
                          * Off by default; only a handful of games with
                          * custom loaders that bypass BIOS tape traps
                          * actually need this. Kept off the F12 menu
                          * and exposed only on the F11 tape slot page. */
} msx_settings_t;

extern msx_settings_t g_settings;

int  msx_settings_choices(msx_setting_id_t id);
const char *msx_settings_label(msx_setting_id_t id);
const char *msx_settings_value_label(msx_setting_id_t id);
void msx_settings_step(msx_setting_id_t id, int delta);

/* Pull the current boot-time values from main.c into g_settings. */
void msx_settings_init_from_bootstate(void);

/* Compose Mode bits + RAMPages + VRAMPages for the current settings. */
void msx_settings_compose(int *mode_out, int *ram_pages_out, int *vram_pages_out);

/* True if the given setting change requires ResetMSX(). */
bool msx_settings_needs_reset(msx_setting_id_t id);

/* Re-push visual / live settings: palette filter, scanlines, UPeriod,
 * MSX_ALLSPRITE / MSX_AUTOFIREA|B / MSX_AUTOSPACE / MSX_FIXEDFONT bits,
 * Cheats(). Safe to call while fMSX is running. */
void msx_settings_apply_visual(void);

/* Apply pending settings: rebuild Mode, reset the MSX. */
void msx_settings_apply_and_reset(void);

/* Legacy pending-reset API — retained as no-ops so main.c still builds. */
bool msx_settings_reset_pending(void);
void msx_settings_pending_reset_values(int *mode, int *ram, int *vram);
void msx_settings_clear_pending_reset(void);

/* Convenience wrapper used by Ctrl+Alt+Del: request a reset with the
 * currently-running Mode/RAM/VRAM (no Mode rebuild). */
void msx_settings_request_reset_current(void);

/* Provided by platform.c — re-pushes every palette slot through the
 * color filter. Called by msx_settings_apply_visual(). */
void platform_repaint_palette(uint8_t color_filter);

/* ---- Persistence ------------------------------------------------ */

/* Load /MSX/msx.ini into g_settings. Missing file => keep defaults and
 * return false. Malformed keys are skipped with a printf(). */
bool msx_settings_load(void);

/* Serialise g_settings to /MSX/msx.ini. Returns true on success. */
bool msx_settings_save(void);

#endif /* MSX_SETTINGS_H */
