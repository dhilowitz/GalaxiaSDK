/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Hilowitz */
/* Nine effects, one per selector position, and the frame they run in (fx_multi.c).
 *
 * Every effect has the same shape: its state is a struct at the start of a shared memory arena (only one
 * effect runs at a time, so they all use the same memory), and an all-zero arena is a valid fresh start.
 * process() gets the dry signal in l[] and r[] and leaves its wet signal there; fx_multi.c does the mix,
 * bypass and the fades when the effect changes. Knobs arrive as 0..1, already smoothed. */
#pragma once
#include <stdint.h>

#define MFX_COUNT 9u
#define MFX_ARENA_BYTES (272u * 1024u)         /* the largest effect: 1.5 s of stereo delay at 16 bits */

struct mfx_knobs {
    float main;                                 /* top-left: time, depth, rate, decay or crush */
    float a, b, c;                              /* bottom-left, bottom-middle, bottom-right */
    float tempo;                                /* tap tempo in seconds, 0 = none (the time knobs rule) */
};

struct mfx_effect {
    const char *name;
    uint32_t bytes;                             /* its state, from the arena */
    void (*process)(void *state, float *l, float *r, uint32_t frames, const struct mfx_knobs *k);
};

extern const struct mfx_effect mfx_delay, mfx_tape, mfx_pingpong, mfx_chorus, mfx_flanger, mfx_phaser,
    mfx_tremolo, mfx_plate, mfx_lofi;
extern const struct mfx_effect *const mfx_effects[MFX_COUNT];

/* The controls, from the main loop: integers only (float code belongs in the audio interrupt). */
struct mfx_controls {
    volatile uint16_t knob[4];                  /* main, a, b, c: 0..1023 */
    volatile uint16_t mix;                      /* 0..1023 */
    volatile uint16_t tempo_ms;                 /* tap tempo, 0 = none */
    volatile uint8_t effect;                    /* 0..8 */
    volatile uint8_t bypass, test_tone;
};
extern struct mfx_controls mfx_ctl;

/* Changing effect: the audio fades the old one out, then asks the main loop to clear the arena
 * (mfx_clear_pending); the main loop clears it and calls mfx_cleared(); the new effect fades in. */
extern volatile uint32_t mfx_clear_pending;
extern uint8_t mfx_arena[MFX_ARENA_BYTES];
void mfx_cleared(void);
uint32_t mfx_running(void);                     /* the effect playing now (for the status line) */

/* in, out: interleaved signed 24-bit stereo; mono_input: the left input feeds both sides */
void mfx_process(const int32_t *in, int32_t *out, uint32_t frames, uint32_t mono_input);
