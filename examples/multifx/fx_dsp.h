/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Hilowitz */
/* Small DSP helpers shared by the effects: no libm in this build, so the few functions they need are here,
 * accurate enough for audio and cheap on the Galaxia's single-precision FPU.
 *
 * Float code runs in the audio interrupt only (it does not save the FPU state for the main loop). Plain C:
 * tests/mfx_render.c compiles the effects on a computer too. */
#pragma once
#include <stdint.h>

#define MFX_SR 44118.0f                         /* the Galaxia's sample rate (gx_board.h: GX_AUDIO_RATE) */
#define MFX_PI 3.14159265f

/* The effects run from flash, through the cache. They used to run from RAM (.ram_text), which measured slower
 * for every one of them: the code's fetches then compete with the effect's own reads of its buffers. With a 440 Hz
 * tone in, the plate took 27 % of the block from RAM and 16 % from flash, the chorus 12 % and 7 %. */

static inline float mfx_clamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

/* 2^x: the integer part into the float exponent, a polynomial for the fraction (error under 1e-4) */
static inline float mfx_exp2(float x)
{
    float f, p;
    int i;
    union { uint32_t u; float f; } s;
    x = mfx_clamp(x, -126.0f, 126.0f);
    i = (int)x;
    if ((float)i > x)
        i--;
    f = x - (float)i;
    p = 1.0f + f * (0.69314718f + f * (0.24022650f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
    s.u = (uint32_t)(i + 127) << 23;
    return p * s.f;
}

/* a knob (0..1) onto lo..hi on a logarithmic scale: rates, frequencies, times that span decades */
static inline float mfx_expo(float k, float lo, float hi)
{
    float r = hi / lo, l2 = 0.0f;
    union { float f; uint32_t u; } v = {r};
    /* log2(r) from the float's exponent and a short series for the mantissa (only needs to be close) */
    float m;
    l2 = (float)((int)((v.u >> 23) & 0xFF) - 127);
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u;
    m = v.f - 1.0f;
    l2 += m * (1.4425f - m * (0.7213f - m * 0.3607f));
    return lo * mfx_exp2(k * l2);
}

/* sine of a phase in turns (0..1 is one cycle): a parabola, refined (error about 0.001) */
static inline float mfx_sin_turns(float ph)
{
    float x, y;
    ph -= (float)(int)ph;
    if (ph < 0.0f)
        ph += 1.0f;
    x = ph < 0.5f ? ph : ph - 1.0f;             /* -0.5 .. 0.5 */
    y = 8.0f * x - 16.0f * x * (x < 0.0f ? -x : x);
    return 0.225f * (y * (y < 0.0f ? -y : y) - y) + y;
}

/* a gentle saturation: linear near 0, levels off at +-1 (a rational tanh, exact enough for a guitar) */
static inline float mfx_soft(float x)
{
    x = mfx_clamp(x, -3.0f, 3.0f);
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

/* the one-pole low-pass coefficient for a cutoff in Hz: y += c * (x - y) */
static inline float mfx_lp_coef(float hz)
{
    float w = 2.0f * MFX_PI * hz / MFX_SR;
    return w / (1.0f + w);                      /* close to 1 - e^-w, and stays below 1 */
}

/* A delay line of 16-bit samples (half the memory of floats; about 90 dB of range, plenty in a feedback
 * loop): write one sample a frame, read any delay up to len - 2 samples, fractional delays interpolated. */
struct mfx_line {
    int16_t *buf;
    uint32_t len, pos;
};
#define MFX_LINE_SCALE 16384.0f                 /* full scale at half the 16-bit range: 6 dB of headroom */

static inline void mfx_line_write(struct mfx_line *d, float x)
{
    float s = mfx_clamp(x, -1.99f, 1.99f) * MFX_LINE_SCALE;
    d->buf[d->pos] = (int16_t)(s < 0.0f ? s - 0.5f : s + 0.5f);
    if (++d->pos >= d->len)
        d->pos = 0;
}

static inline float mfx_line_read(const struct mfx_line *d, float delay)   /* delay in samples, >= 1 */
{
    uint32_t i = (uint32_t)delay, a, b;
    float f = delay - (float)i;
    a = d->pos >= i ? d->pos - i : d->pos + d->len - i;   /* (pos is the next write) */
    b = a ? a - 1u : d->len - 1u;
    return ((float)d->buf[a] + f * (float)(d->buf[b] - d->buf[a])) * (1.0f / MFX_LINE_SCALE);
}

/* The same with float samples, for short lines where precision matters (modulated effects, the plate) */
struct mfx_fline {
    float *buf;
    uint32_t len, pos;
};

static inline void mfx_fline_write(struct mfx_fline *d, float x)
{
    d->buf[d->pos] = x;
    if (++d->pos >= d->len)
        d->pos = 0;
}

static inline float mfx_fline_read(const struct mfx_fline *d, float delay)
{
    uint32_t i = (uint32_t)delay, a, b;
    float f = delay - (float)i;
    a = d->pos >= i ? d->pos - i : d->pos + d->len - i;
    b = a ? a - 1u : d->len - 1u;
    return d->buf[a] + f * (d->buf[b] - d->buf[a]);
}

static inline float mfx_fline_tap(const struct mfx_fline *d, uint32_t delay)   /* a whole-sample delay */
{
    return d->buf[d->pos >= delay ? d->pos - delay : d->pos + d->len - delay];
}
