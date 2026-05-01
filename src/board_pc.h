/*
 * frank-msx — fMSX for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-msx
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * board_pc.h — Olimex RP2040-PICO-PC GPIO layout for frank-msx.
 *
 * Supported video: PIO HDMI / PIO VGA (no TV; no HSTX audio path used).
 * Supported audio: PWM / Disabled.
 *
 * Selected when -DPLATFORM=pc.
 */
#ifndef BOARD_PC_H
#define BOARD_PC_H

/* ---- Video capabilities ---- */
/* HDMI pins happen to be on GPIO 12..19 (HSTX-capable range) but we
 * expose only PIO HDMI/VGA per the supported matrix. Leave HAS_HSTX
 * unset so CMake doesn't offer the HSTX driver for this board. */

/* ---- Audio capabilities ---- */
#define HAS_PWM 1

/* ---- HDMI / VGA pins ---- */
#define HDMI_BASE_PIN 12
#define VGA_BASE_PIN  12

/* ---- SD Card (SPI0) ---- */
#define SDCARD_PIN_SPI0_CS   22
#define SDCARD_PIN_SPI0_SCK  6
#define SDCARD_PIN_SPI0_MOSI 7
#define SDCARD_PIN_SPI0_MISO 4

/* ---- PS/2 ---- */
#define PS2_PIN_CLK    0
#define PS2_PIN_DATA   1

/* ---- NES/SNES pad ---- */
#define NESPAD_GPIO_CLK   5
#define NESPAD_GPIO_LATCH 9
#define NESPAD_GPIO_DATA  20

/* ---- Cassette tape input (EAR / CAS-IN) ----
 * GP22 is taken by SDCARD_PIN_SPI0_CS on this board, so we park tape-in
 * on GP28 — still free and away from HDMI lanes. */
#define TAPE_IN_PIN 28

/* ---- PWM audio ---- */
#define PWM_PIN0 27
#define PWM_PIN1 28

/* ---- UART logging disabled (GPIO 0 is PS/2 CLK on PC) ---- */
#define NO_UART_LOGGING 1

/* ---- PSRAM: GP8 only (RP2350B) ---- */
#define PSRAM_CS_PIN_RP2350A 8
#define PSRAM_CS_PIN_RP2350B 8

#endif /* BOARD_PC_H */
