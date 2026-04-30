/*
 * board_m2.h — Murmulator 2.0 GPIO layout for frank-msx.
 *
 * Supported video: HSTX HDMI (default) / PIO HDMI / PIO VGA / composite TV.
 * Supported audio: HSTX HDMI / I2S / PWM / Disabled.
 *
 * Selected when -DPLATFORM=m2 (or left unset).
 */
#ifndef BOARD_M2_H
#define BOARD_M2_H

/* ---- Video capabilities ---- */
#define HAS_HSTX 1                /* HDMI base pins on GPIO 12..19 */
#define HAS_TV   1                /* Software composite on HDMI DAC pins */

/* ---- Audio capabilities ---- */
#define HAS_I2S 1                 /* External DAC on GPIO 9/10/11 */
#define HAS_PWM 1
/* HDMI audio is implicit from HAS_HSTX but HAS_HDMI_AUDIO makes the
 * audio-cycle table platform-independent. */
#define HAS_HDMI_AUDIO 1

/* ---- HDMI / VGA pins ---- */
#define HDMI_BASE_PIN 12
#define VGA_BASE_PIN  12
/* Kept for any TU that still references the individual pair symbols. */
#define HDMI_PIN_CLKN 12
#define HDMI_PIN_CLKP 13
#define HDMI_PIN_D0N  14
#define HDMI_PIN_D0P  15
#define HDMI_PIN_D1N  16
#define HDMI_PIN_D1P  17
#define HDMI_PIN_D2N  18
#define HDMI_PIN_D2P  19

/* ---- Composite TV ---- */
#define TV_BASE_PIN 12

/* ---- SD Card (SPI0) ---- */
#define SDCARD_PIN_SPI0_CS   5
#define SDCARD_PIN_SPI0_SCK  6
#define SDCARD_PIN_SPI0_MOSI 7
#define SDCARD_PIN_SPI0_MISO 4

/* ---- PS/2 ---- */
#define PS2_PIN_CLK    2
#define PS2_PIN_DATA   3
#define PS2_MOUSE_CLK  0
#define PS2_MOUSE_DATA 1

/* ---- NES/SNES pad ---- */
#define NESPAD_GPIO_CLK   20
#define NESPAD_GPIO_LATCH 21
#define NESPAD_GPIO_DATA  26

/* ---- Cassette tape input (free GPIO — EAR / CAS-IN) ---- */
#define TAPE_IN_PIN 22

/* ---- I2S audio ---- */
#define I2S_DATA_PIN       9
#define I2S_CLOCK_PIN_BASE 10

/* ---- PWM audio ---- */
#define PWM_PIN0 10
#define PWM_PIN1 11

/* ---- PSRAM (RP2350A vs RP2350B autodetect) ---- */
#define PSRAM_CS_PIN_RP2350A 8
#define PSRAM_CS_PIN_RP2350B 47

#endif /* BOARD_M2_H */
