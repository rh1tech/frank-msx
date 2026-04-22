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
    MSX_SETTING_MODEL = 0,    /* MSX1 / MSX2 / MSX2+ */
    MSX_SETTING_REGION,       /* NTSC / PAL */
    MSX_SETTING_RAM,          /* 64/128/256/512 KB */
    MSX_SETTING_VRAM,         /* 32/64/128/256/512 KB */
    MSX_SETTING_JOY1,         /* Empty/Joystick/Mouse/Moustick */
    MSX_SETTING_JOY2,
    MSX_SETTING_COUNT
} msx_setting_id_t;

typedef struct {
    uint8_t model;    /* 0=MSX1, 1=MSX2, 2=MSX2+ */
    uint8_t region;   /* 0=NTSC, 1=PAL */
    uint8_t ram;      /* index into RAM_CHOICES */
    uint8_t vram;     /* index into VRAM_CHOICES */
    uint8_t joy1;     /* 0..3 */
    uint8_t joy2;     /* 0..3 */
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

/* Apply pending settings: rebuild Mode, reset the MSX, reboot. */
void msx_settings_apply_and_reset(void);

#endif /* MSX_SETTINGS_H */
