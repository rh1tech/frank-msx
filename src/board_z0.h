/*
 * board_z0.h — Waveshare RP2350-PiZero GPIO layout for frank-msx.
 *
 * Supported video: PIO HDMI / PIO VGA on GPIO 32..39 (no TV, no HSTX).
 * Supported audio: PWM / Disabled.
 *
 * Selected when -DPLATFORM=z0. The PICO_BOARD header is overridden to
 * waveshare_rp2350_pizero in CMakeLists.txt.
 *
 * The PIO HDMI driver has a Z0-specific variant (data lanes first, BGR,
 * non-inverted diff pairs) — HDMI.h keys off PLATFORM_Z0 to flip the
 * data/clock pin ordering.
 */
#ifndef BOARD_Z0_H
#define BOARD_Z0_H

/* ---- Video capabilities ---- */

/* ---- Audio capabilities ---- */
#define HAS_PWM 1

/* ---- HDMI / VGA pins ---- */
#define HDMI_BASE_PIN 32
#define VGA_BASE_PIN  32

/* ---- SD Card (hardware SPI1 — pins above 29 require SPI1) ---- */
#define SDCARD_SPI_BUS       spi1
#define SDCARD_PIN_SPI0_CS   43
#define SDCARD_PIN_SPI0_SCK  30
#define SDCARD_PIN_SPI0_MOSI 31
#define SDCARD_PIN_SPI0_MISO 40

/* ---- PS/2 ---- */
#define PS2_PIN_CLK    14
#define PS2_PIN_DATA   15

/* ---- NES/SNES pad ---- */
#define NESPAD_GPIO_CLK   4
#define NESPAD_GPIO_LATCH 5
#define NESPAD_GPIO_DATA  7

/* ---- PWM audio ---- */
#define PWM_PIN0 10
#define PWM_PIN1 11

/* ---- UART logging disabled (Waveshare pinout does not expose UART0) ---- */
#define NO_UART_LOGGING 1

/* ---- PSRAM: built-in on GP47 (RP2350B) ---- */
#define PSRAM_CS_PIN_RP2350A 47
#define PSRAM_CS_PIN_RP2350B 47

#endif /* BOARD_Z0_H */
