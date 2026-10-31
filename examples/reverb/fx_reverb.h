/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Hilowitz */
/* Galaxia stereo reverb: predelay -> two allpass diffusers -> 8-line feedback delay network
 * (Hadamard feedback, one-pole damping per line) -> two decorrelated outputs.
 *
 * Float, compiled for the FPU (-mfprev1). Only the audio interrupt runs float code: the main loop
 * hands over integer controls in rv_ctl, and rv_process derives the coefficients itself. */
#pragma once
#include <stdint.h>

struct rv_controls {
    volatile uint16_t decay, tone, predelay, mix, level;   /* 0..1023 */
    volatile uint8_t size;                                 /* 0..8 */
    volatile uint8_t freeze, bypass;
};
extern struct rv_controls rv_ctl;
extern const uint32_t rv_bytes;                            /* the reverb's buffers, for the status line */

void rv_init(void);
/* in, out: interleaved signed 24-bit stereo, frames frames; mono_input: the left input feeds both */
void rv_process(const int32_t *in, int32_t *out, uint32_t frames, uint32_t mono_input);
