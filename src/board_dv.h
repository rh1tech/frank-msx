/*
 * board_dv.h — Pimoroni Pico DV Demo GPIO layout for frank-msx.
 *
 * Supported video: PIO HDMI / PIO VGA (no HSTX, no TV).
 * Supported audio: PWM / Disabled (no I2S pins, no HDMI audio).
 *
 * Selected when -DPLATFORM=dv.
 */
#ifndef BOARD_DV_H
#define BOARD_DV_H

/* ---- Video capabilities ---- */
/* No HSTX, no TV. */

/* ---- Audio capabilities ---- */
#define HAS_PWM 1

/* ---- HDMI / VGA pins (PIO driver; GPIO 6..13) ---- */
#define HDMI_BASE_PIN 6
#define VGA_BASE_PIN  6

/* ---- SD Card (SPI0 pins aren't a single HW peripheral; PIO-SPI needed).
 * SCK=5 forces pio_spi on pio1/SM3 — see CMakeLists.txt. */
#define SDCARD_PIN_SPI0_CS   22
#define SDCARD_PIN_SPI0_SCK  5
#define SDCARD_PIN_SPI0_MOSI 18
#define SDCARD_PIN_SPI0_MISO 19

/* ---- PS/2 ---- */
#define PS2_PIN_CLK    0
#define PS2_PIN_DATA   1

/* ---- NES/SNES pad ---- */
#define NESPAD_GPIO_CLK   14
#define NESPAD_GPIO_LATCH 15
#define NESPAD_GPIO_DATA  20

/* ---- Cassette tape input (EAR / CAS-IN) ----
 * GP22 is taken by SDCARD_PIN_SPI0_CS on this board, so we park tape-in
 * on GP28 — still free, far from HDMI/VGA lanes. */
#define TAPE_IN_PIN 28

/* ---- PWM audio ---- */
#define PWM_PIN0 26
#define PWM_PIN1 27

/* ---- UART logging disabled (GPIO 0 is PS/2 CLK on DV) ---- */
#define NO_UART_LOGGING 1

/* ---- PSRAM: GP47 only (RP2350B pinout on the Pico DV Demo) ---- */
#define PSRAM_CS_PIN_RP2350A 47
#define PSRAM_CS_PIN_RP2350B 47

#endif /* BOARD_DV_H */
