/*
 * board_config.h
 *
 * Board configuration for frank-msx — fMSX for RP2350.
 * Only the M2 layout is supported at this stage.
 */
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "pico.h"
#include "hardware/structs/sysinfo.h"
#include "hardware/vreg.h"

#if !defined(BOARD_M2)
#define BOARD_M2
#endif

/* ---- Clocking defaults ---- */
#ifndef CPU_CLOCK_MHZ
#define CPU_CLOCK_MHZ 252
#endif

#ifndef PSRAM_MAX_FREQ_MHZ
#define PSRAM_MAX_FREQ_MHZ 133
#endif

#ifndef FLASH_MAX_FREQ_MHZ
#define FLASH_MAX_FREQ_MHZ 66
#endif

#ifndef CPU_VOLTAGE
/* RP2350 vreg exposes 1.50 → 1.60 → 1.65 → 1.70 V steps; no 1.55 V. */
#  if CPU_CLOCK_MHZ >= 504
#    define CPU_VOLTAGE VREG_VOLTAGE_1_65
#  elif CPU_CLOCK_MHZ >= 300
#    define CPU_VOLTAGE VREG_VOLTAGE_1_60
#  else
#    define CPU_VOLTAGE VREG_VOLTAGE_1_50
#  endif
#endif

/* ---- PSRAM pin (RP2350A vs RP2350B autodetect) ---- */
#define PSRAM_PIN_RP2350A 8     /* M2 RP2350A variant */
#define PSRAM_PIN_RP2350B 47    /* RP2350B always GPIO47 */

static inline uint get_psram_pin(void) {
#if PICO_RP2350
    uint32_t package_sel = *((io_ro_32*)(SYSINFO_BASE + SYSINFO_PACKAGE_SEL_OFFSET));
    if (package_sel & 1) return PSRAM_PIN_RP2350A;
    return PSRAM_PIN_RP2350B;
#else
    return 0;
#endif
}

/* ---- M2 GPIO layout ---- */
#ifdef BOARD_M2

/* HDMI */
#define HDMI_PIN_CLKN 12
#define HDMI_PIN_CLKP 13
#define HDMI_PIN_D0N  14
#define HDMI_PIN_D0P  15
#define HDMI_PIN_D1N  16
#define HDMI_PIN_D1P  17
#define HDMI_PIN_D2N  18
#define HDMI_PIN_D2P  19
#define HDMI_BASE_PIN HDMI_PIN_CLKN

/* SD Card (SPI0). Matches the pin names used by drivers/sdcard/sdcard.c.
 * These values are also passed via `target_compile_definitions` in the
 * top-level CMakeLists so the SD driver TU sees them; we keep the
 * defines here for any other translation unit that needs to reason
 * about SD wiring. */
#ifndef SDCARD_PIN_SPI0_CS
#define SDCARD_PIN_SPI0_CS   5
#endif
#ifndef SDCARD_PIN_SPI0_SCK
#define SDCARD_PIN_SPI0_SCK  6
#endif
#ifndef SDCARD_PIN_SPI0_MOSI
#define SDCARD_PIN_SPI0_MOSI 7
#endif
#ifndef SDCARD_PIN_SPI0_MISO
#define SDCARD_PIN_SPI0_MISO 4
#endif

/* PS/2 keyboard + mouse */
#define PS2_PIN_CLK    2
#define PS2_PIN_DATA   3
#define PS2_MOUSE_CLK  0
#define PS2_MOUSE_DATA 1

/* NES/SNES pad (M2 layout — matches murmsnes) */
#ifndef NESPAD_GPIO_CLK
#define NESPAD_GPIO_CLK   20
#endif
#ifndef NESPAD_GPIO_LATCH
#define NESPAD_GPIO_LATCH 21
#endif
#ifndef NESPAD_GPIO_DATA
#define NESPAD_GPIO_DATA  26
#endif

/* I2S audio (external DAC) */
#define I2S_DATA_PIN       9
#define I2S_CLOCK_PIN_BASE 10

/* PWM audio (RC low-pass filter on pins). Shares GPIO 10/11 with the
 * I2S output — only one audio backend can drive those pins at a time,
 * selected at runtime via Settings → Audio. */
#define PWM_PIN0           10
#define PWM_PIN1           11

#endif /* BOARD_M2 */

/* ---- MSX display ----
 *
 * fMSX's native drawing buffer is 272x228 (8 px side border + 256 active
 * + 8 px side border). The HDMI driver we inherited from murmsnes only
 * reserves room for 256 pixels of content per scanline (32 + 256 + 32 =
 * 320 pixel output line); wider buffers overrun the sync region and
 * break the HDMI signal.
 *
 * We keep HEIGHT=228 so fMSX's FirstLine offset (18) leaves room for
 * full 212-line screen modes, but trim WIDTH to 256. RefreshBorder's
 * side-border loop collapses to zero when WIDTH==256 — borders are
 * hidden, which is fine; the driver already adds a 32-px margin.
 * Only rows 0..223 are actually scanned out (HDMI content_scanlines
 * = 448 doubled), so a few overscan lines are lost for now.
 */
#define MSX_FB_WIDTH   256
#define MSX_FB_HEIGHT  228

#endif /* BOARD_CONFIG_H */
