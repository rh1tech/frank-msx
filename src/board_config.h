/*
 * board_config.h
 *
 * Board configuration dispatcher for frank-msx — fMSX for RP2350.
 *
 * Select platform via CMake: -DPLATFORM=m1|m2|dv|pc|z0 (default m2).
 *
 * Supported video/audio matrix:
 *   M2 — HDMI_HSTX, HDMI_PIO/VGA, TV          audio: HSTX / I2S / PWM
 *   M1 — HDMI_PIO/VGA, TV                     audio: I2S / PWM
 *   DV — HDMI_PIO/VGA                         audio: PWM
 *   PC — HDMI_PIO/VGA                         audio: PWM
 *   Z0 — HDMI_PIO/VGA                         audio: PWM
 */
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "pico.h"
#include "hardware/structs/sysinfo.h"
#include "hardware/vreg.h"

/* ---- Platform selection ----
 *
 * The CMakeLists.txt already defines both PLATFORM_<VARIANT> and
 * BOARD_<VARIANT> via target_compile_definitions, so we only ensure
 * one of them is set when the file is pulled in from tooling without
 * CMake's flags. Do not re-define either macro unconditionally — that
 * triggers a redefinition warning on repeat includes. */
#if !defined(PLATFORM_M1) && !defined(PLATFORM_M2) && \
    !defined(PLATFORM_DV) && !defined(PLATFORM_PC) && !defined(PLATFORM_Z0)
#  define PLATFORM_M2 1
#endif

#if defined(PLATFORM_M1)
#  ifndef BOARD_M1
#    define BOARD_M1 1
#  endif
#  include "board_m1.h"
#elif defined(PLATFORM_DV)
#  ifndef BOARD_DV
#    define BOARD_DV 1
#  endif
#  include "board_dv.h"
#elif defined(PLATFORM_PC)
#  ifndef BOARD_PC
#    define BOARD_PC 1
#  endif
#  include "board_pc.h"
#elif defined(PLATFORM_Z0)
#  ifndef BOARD_Z0
#    define BOARD_Z0 1
#  endif
#  include "board_z0.h"
#else
#  ifndef BOARD_M2
#    define BOARD_M2 1
#  endif
#  include "board_m2.h"
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

/* ---- PSRAM pin (RP2350A vs RP2350B autodetect) ----
 *
 * Each board header defines PSRAM_CS_PIN_RP2350A / PSRAM_CS_PIN_RP2350B.
 * Boards without PSRAM can set both to the same pin; the psram_init
 * probe will fail cleanly on boards that lack the chip entirely. */
#ifndef PSRAM_CS_PIN_RP2350A
#  error "Board header must define PSRAM_CS_PIN_RP2350A"
#endif
#ifndef PSRAM_CS_PIN_RP2350B
#  error "Board header must define PSRAM_CS_PIN_RP2350B"
#endif

/* Kept for legacy TU references that still use the shorter name. */
#define PSRAM_PIN_RP2350A PSRAM_CS_PIN_RP2350A
#define PSRAM_PIN_RP2350B PSRAM_CS_PIN_RP2350B

static inline uint get_psram_pin(void) {
#if PICO_RP2350
    uint32_t package_sel = *((io_ro_32*)(SYSINFO_BASE + SYSINFO_PACKAGE_SEL_OFFSET));
    if (package_sel & 1) return PSRAM_CS_PIN_RP2350A;
    return PSRAM_CS_PIN_RP2350B;
#else
    return 0;
#endif
}

/* ---- Optional peripherals ----
 *
 * HAS_PS2_MOUSE is set when the board header defined PS2_MOUSE_CLK —
 * only M2 currently wires a second PS/2 port (keyboard on GPIO 2/3,
 * mouse on GPIO 0/1). Boards without the second port skip the mouse
 * PIO state machine + reset exchange entirely. */
#ifdef PS2_MOUSE_CLK
#  define HAS_PS2_MOUSE 1
#else
/* Stub pins so ps2_init(...) still gets a valid argument on boards
 * without a mouse port — the SM is claimed but the wrapper skips
 * ps2_mouse_init_device(), so no reset traffic is emitted and the
 * SM just sits idle listening for bytes that never arrive. */
#  define PS2_MOUSE_CLK  PS2_PIN_CLK
#  define PS2_MOUSE_DATA PS2_PIN_DATA
#endif

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
