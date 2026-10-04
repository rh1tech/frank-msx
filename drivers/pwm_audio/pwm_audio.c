/*
 * frank-msx — fMSX for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-msx
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * frank-msx - PWM Audio Driver (timer-driven, no DMA)
 *
 *   - The audio pins run 12-bit PWM: wrap 4095, clkdiv 1 (carrier
 *     sys_clk / 4096, ~61.5 kHz at 252 MHz), the same scheme as murm386
 *     and PICO-BK on the Olimex PICO-PC. Samples use the full range.
 *   - A separate, otherwise unused PWM slice serves as the sample clock:
 *     its wrap interrupt fires once per sample period (sys_clk /
 *     sample_rate). The handler takes exactly one sample from the ring and
 *     writes it into the CC halves of the audio pins, like the timer
 *     callbacks of murm386 / pico-speccy / PICO-BK. No DMA, no buffer
 *     switching: every output sample is placed by this one handler.
 *   - If the ring runs dry the handler repeats the last sample (no stale
 *     data, no jump to silence).
 *   - The emulator pushes ~11 samples at a time (fMSX renders sound every
 *     8 scanlines). The producer keeps the fill level near RING_TARGET by
 *     inserting or skipping one sample per push, absorbing the small rate
 *     difference between the emulated machine and the sample clock.
 *
 * SPDX-License-Identifier: MIT
 */

#include "pwm_audio.h"

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"

#include <stdio.h>

#define PWM_BITS     12
#define PWM_WRAP     ((1u << PWM_BITS) - 1u)
#define PWM_CENTER   (PWM_WRAP / 2u)

/* Slice used only as the sample clock. Its GPIOs must not be in PWM
 * function; on the PICO-PC slice 7 maps to GPIO14/15 (HDMI, PIO-driven). */
#ifndef PWM_AUDIO_CLOCK_SLICE
#define PWM_AUDIO_CLOCK_SLICE 7
#endif
#define PWM_AUDIO_IRQ        PWM_IRQ_WRAP_1
#define PWM_AUDIO_IRQ_INDEX  1

#define RING_BITS    11
#define RING_LEN     (1u << RING_BITS)        /* ~93 ms at 22.05 kHz */
#define RING_MASK    (RING_LEN - 1u)
#define RING_TARGET  768u                     /* ~35 ms buffered      */
#define RING_SLACK   256u

static uint16_t ring[RING_LEN];
static volatile uint32_t rd_cnt = 0;          /* advanced by the IRQ   */
static volatile uint32_t wr_cnt = 0;          /* advanced by producer  */
static uint16_t last_level = PWM_CENTER;      /* IRQ: held on underrun */
static uint16_t last_pushed = PWM_CENTER;     /* producer: for inserts */

/* CC register halves of the two audio pins. */
static io_rw_32 *cc_a, *cc_b;
static uint32_t shift_a, shift_b, mask_a, mask_b;

static bool initialized = false;
static volatile bool muted = false;

static void __not_in_flash_func(sample_clock_irq)(void) {
    pwm_hw->intr = 1u << PWM_AUDIO_CLOCK_SLICE;

    uint32_t r = rd_cnt;
    uint32_t level = last_level;
    if (r != wr_cnt) {
        level = ring[r & RING_MASK];
        rd_cnt = r + 1;
        last_level = (uint16_t)level;
    }
    if (muted) level = 0;
    hw_write_masked(cc_a, level << shift_a, mask_a);
    hw_write_masked(cc_b, level << shift_b, mask_b);
}

static void cc_half(uint pin, io_rw_32 **cc, uint32_t *shift, uint32_t *mask) {
    uint slice = pwm_gpio_to_slice_num(pin);
    bool chan_b = pwm_gpio_to_channel(pin) == PWM_CHAN_B;
    *cc = &pwm_hw->slice[slice].cc;
    *shift = chan_b ? PWM_CH0_CC_B_LSB : PWM_CH0_CC_A_LSB;
    *mask = chan_b ? PWM_CH0_CC_B_BITS : PWM_CH0_CC_A_BITS;
}

void pwm_audio_init(uint pin_l, uint pin_r, uint32_t sample_rate) {
    if (initialized) return;

    for (uint32_t i = 0; i < RING_LEN; i++) ring[i] = PWM_CENTER;

    /* Audio pins: 12-bit PWM at mid level. */
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, 1.0f);
    pwm_config_set_wrap(&cfg, PWM_WRAP);
    uint slice_l = pwm_gpio_to_slice_num(pin_l);
    uint slice_r = pwm_gpio_to_slice_num(pin_r);
    pwm_init(slice_l, &cfg, false);
    if (slice_r != slice_l) pwm_init(slice_r, &cfg, false);
    gpio_set_function(pin_l, GPIO_FUNC_PWM);
    gpio_set_function(pin_r, GPIO_FUNC_PWM);
    pwm_set_gpio_level(pin_l, PWM_CENTER);
    pwm_set_gpio_level(pin_r, PWM_CENTER);
    cc_half(pin_l, &cc_a, &shift_a, &mask_a);
    cc_half(pin_r, &cc_b, &shift_b, &mask_b);
    pwm_set_mask_enabled(pwm_hw->en | (1u << slice_l) | (1u << slice_r));

    /* Sample clock: one wrap per sample period. */
    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t period = sys_clk / sample_rate;           /* 11428 @ 252 MHz */
    pwm_config clk = pwm_get_default_config();
    if (period > 65536) {                              /* keep wrap 16-bit */
        uint32_t div = (period + 65535) / 65536;
        pwm_config_set_clkdiv_int(&clk, div);
        period /= div;
    }
    pwm_config_set_wrap(&clk, (uint16_t)(period - 1));
    pwm_init(PWM_AUDIO_CLOCK_SLICE, &clk, false);

    /* Prefill so the stream starts RING_TARGET samples ahead. */
    rd_cnt = 0;
    wr_cnt = RING_TARGET;

    pwm_hw->intr = 1u << PWM_AUDIO_CLOCK_SLICE;
    irq_set_exclusive_handler(PWM_AUDIO_IRQ, sample_clock_irq);
    irq_set_priority(PWM_AUDIO_IRQ, 0x40);   /* above USB / SD / timers */
    pwm_irqn_set_slice_enabled(PWM_AUDIO_IRQ_INDEX, PWM_AUDIO_CLOCK_SLICE, true);
    irq_set_enabled(PWM_AUDIO_IRQ, true);
    pwm_set_enabled(PWM_AUDIO_CLOCK_SLICE, true);

    initialized = true;
    printf("PWM audio: pins %u/%u, 12-bit, sample clock slice %u, %lu Hz\n",
           pin_l, pin_r, PWM_AUDIO_CLOCK_SLICE,
           (unsigned long)(sys_clk / period));
}

static inline uint16_t to_level(int16_t s) {
    int32_t lvl = (((int32_t)s * (int32_t)PWM_CENTER) >> 15) + (int32_t)PWM_CENTER;
    if (lvl < 0) lvl = 0;
    if (lvl > (int32_t)PWM_WRAP) lvl = PWM_WRAP;
    return (uint16_t)lvl;
}

/* Append levels to the ring (producer side, Core 0 outside the IRQ). */
static void ring_put(const int16_t *samples, int count, bool silence) {
    if (!initialized || count <= 0) return;

    uint32_t w = wr_cnt;
    uint32_t fill = w - rd_cnt;
    if (fill > RING_LEN) fill = RING_LEN;               /* defensive */

    bool skip = fill > RING_TARGET + RING_SLACK;                    /* drop one */
    bool dup  = fill + (uint32_t)count < RING_TARGET - RING_SLACK;   /* add one  */

    for (int i = skip ? 1 : 0; i < count; i++) {
        if (w - rd_cnt >= RING_LEN - 1) break;          /* full: drop rest */
        uint16_t lvl = silence ? (uint16_t)PWM_CENTER : to_level(samples[i]);
        ring[w & RING_MASK] = lvl;
        last_pushed = lvl;
        w++;
    }
    if (dup && (w - rd_cnt < RING_LEN - 1)) {
        ring[w & RING_MASK] = last_pushed;
        w++;
    }
    __dmb();
    wr_cnt = w;
}

void pwm_audio_push_samples(const int16_t *buf, int count) {
    ring_put(buf, count, false);
}

void pwm_audio_fill_silence(int count) {
    ring_put(NULL, count, true);
}

void pwm_audio_set_muted(bool m) {
    muted = m;
}

void pwm_audio_set_frame_rate(int frame_rate) {
    /* The sample clock and the ring make the frame rate irrelevant here. */
    (void)frame_rate;
}
