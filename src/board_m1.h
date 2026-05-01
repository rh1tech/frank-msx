/*
 * frank-msx — fMSX for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-msx
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * board_m1.h — Murmulator 1.x GPIO layout for frank-msx.
 *
 * Supported video: PIO HDMI / PIO VGA / composite TV (no HSTX).
 * Supported audio: I2S / PWM / Disabled (no HDMI audio without HSTX).
 *
 * Selected when -DPLATFORM=m1.
 */
#ifndef BOARD_M1_H
#define BOARD_M1_H

/* ---- Video capabilities ---- */
/* No HSTX — HDMI base pin is 6, outside the HSTX-capable range 12..19. */
#define HAS_TV 1

/* ---- Audio capabilities ---- */
#define HAS_I2S 1
#define HAS_PWM 1

/* ---- HDMI / VGA pins (PIO driver; GPIO 6..13) ---- */
#define HDMI_BASE_PIN 6
#define VGA_BASE_PIN  6

/* ---- Composite TV ---- */
#define TV_BASE_PIN 6

/* ---- SD Card (SPI0) ---- */
#define SDCARD_PIN_SPI0_CS   5
#define SDCARD_PIN_SPI0_SCK  2
#define SDCARD_PIN_SPI0_MOSI 3
#define SDCARD_PIN_SPI0_MISO 4

/* ---- PS/2 ---- */
#define PS2_PIN_CLK    0
#define PS2_PIN_DATA   1

/* ---- NES/SNES pad ---- */
#define NESPAD_GPIO_CLK   14
#define NESPAD_GPIO_LATCH 15
#define NESPAD_GPIO_DATA  16

/* ---- Cassette tape input (free GPIO — EAR / CAS-IN) ---- */
#define TAPE_IN_PIN 22

/* ---- I2S audio ---- */
#define I2S_DATA_PIN       26
#define I2S_CLOCK_PIN_BASE 27

/* ---- PWM audio ---- */
#define PWM_PIN0 26
#define PWM_PIN1 27

/* ---- UART logging disabled (GPIO 0 is PS/2 CLK on M1) ---- */
#define NO_UART_LOGGING 1

/* ---- PSRAM ---- */
#define PSRAM_CS_PIN_RP2350A 8
#define PSRAM_CS_PIN_RP2350B 47

#endif /* BOARD_M1_H */
