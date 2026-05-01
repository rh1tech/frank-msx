/*
 * frank-msx — fMSX for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-msx
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * frank-msx - PWM Audio Driver
 * DMA-paced double-buffered PWM playback via DREQ from the audio PWM slice.
 * Ported from murmnes.
 * SPDX-License-Identifier: MIT
 */

#ifndef PWM_AUDIO_H
#define PWM_AUDIO_H

#include <stdint.h>
#include "pico/stdlib.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the PWM audio path.
 *   pin_l, pin_r: GPIOs wired to the audio PWM slice (may be the same slice
 *                 or two different slices; if they share a slice the single
 *                 DMA stream drives both, otherwise pin_r is parked at a
 *                 static mid-level PWM).
 *   sample_rate:  output sample rate in Hz (e.g. 22050).
 * Safe to call multiple times — subsequent calls are no-ops. */
void pwm_audio_init(uint pin_l, uint pin_r, uint32_t sample_rate);

/* Push signed 16-bit mono samples. Non-blocking as long as a DMA buffer is
 * free; blocks briefly if both buffers are still playing. */
void pwm_audio_push_samples(const int16_t *buf, int count);

/* Drop `count` samples' worth of silence at the configured sample rate. */
void pwm_audio_fill_silence(int count);

/* Resize the per-DMA-chunk transfer count to match the emulation frame rate.
 * frame_rate = 60 for NTSC, 50 for PAL. */
void pwm_audio_set_frame_rate(int frame_rate);

#ifdef __cplusplus
}
#endif

#endif /* PWM_AUDIO_H */
