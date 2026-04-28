/*
 * msx_settings.h — mutable runtime settings for the fMSX core.
 *
 * Held in RAM only (no SD persistence yet). Changing any field
 * requires a ResetMSX() call because model / RAM / VRAM all touch
 * the core's memory map. Joystick-socket changes go through fMSX's
 * Mode bits, which ResetMSX also respects.
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
    MSX_SETTING_SCANLINES,    /* None / TV / LCD / LCD Raster (live) */
    MSX_SETTING_COLOR,        /* Normal / Monochrome / Sepia / Green / Amber (live) */
    MSX_SETTING_AUDIO,        /* HDMI / I2S / PWM / Disabled (live) */
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

/* Scanline enum values match the order of SCANLINE_LABELS.
 * Only Off/On are supported today — our HDMI driver has a single
 * line-blanker, so the previous TV/LCD/Raster split all rendered
 * identically. We'll re-introduce the subtypes once the driver grows
 * a proper raster-mask mode. */
typedef enum {
    MSX_SCAN_OFF = 0,
    MSX_SCAN_ON,
    MSX_SCAN_COUNT
} msx_scanlines_t;

/* Color-filter enum values match the order of COLOR_LABELS. */
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
} msx_settings_t;

extern msx_settings_t g_settings;

/* Number of available values for setting `id`. */
int  msx_settings_choices(msx_setting_id_t id);

/* Human-readable setting name (fixed label on the left). */
const char *msx_settings_label(msx_setting_id_t id);

/* Human-readable current value (right side of the row). */
const char *msx_settings_value_label(msx_setting_id_t id);

/* Step the current value by `delta` (-1 or +1), wrapping. */
void msx_settings_step(msx_setting_id_t id, int delta);

/* Pull the current boot-time values from main.c into g_settings. */
void msx_settings_init_from_bootstate(void);

/* Compose Mode bits + RAMPages + VRAMPages for the current settings. */
void msx_settings_compose(int *mode_out, int *ram_pages_out, int *vram_pages_out);

/* True if the given setting change requires ResetMSX(). */
bool msx_settings_needs_reset(msx_setting_id_t id);

/* Re-tint the HDMI palette and refresh the scanline flag based on
 * the current color/scanlines settings. Called once at init and again
 * whenever either of those two settings changes. */
void msx_settings_apply_visual(void);

/* Apply pending settings: rebuild Mode, reset the MSX, reboot.
 *
 * This does NOT call ResetMSX() directly — reset from inside a key
 * handler runs while RunZ80 sits on the stack, and freeing the old
 * RAM/VRAM under the interpreter's feet causes sporadic hangs. We
 * instead stash the requested Mode/RAM/VRAM and raise ExitNow, so
 * the main loop handles the reset after RunZ80 unwinds. */
void msx_settings_apply_and_reset(void);

/* True if the main loop should unwind and perform a deferred reset,
 * using the values returned by msx_settings_pending_reset_values(). */
bool msx_settings_reset_pending(void);
void msx_settings_pending_reset_values(int *mode, int *ram, int *vram);
void msx_settings_clear_pending_reset(void);

/* Convenience wrapper used by Ctrl+Alt+Del: request a reset with the
 * currently-running Mode/RAM/VRAM (no Mode rebuild). */
void msx_settings_request_reset_current(void);

/* Provided by platform.c — re-pushes every palette slot through the
 * color filter. Called by msx_settings_apply_visual(). */
void platform_repaint_palette(uint8_t color_filter);

#endif /* MSX_SETTINGS_H */
