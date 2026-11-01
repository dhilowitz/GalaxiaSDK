/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Hilowitz */
/* 9. Lo-fi: fewer samples, fewer bits, a darker tone and some grit, like an early sampler.
 *   main  crush: the sample rate, from 44.1 kHz down to 1.5 kHz   b  tone, dark to bright
 *   a     bits, from 16 down to 3                                 c  drive into the crusher
 * Holding each sample for several output samples lowers the sample rate; frequencies above half of it fold
 * back down as the metallic aliasing early samplers were known for. Rounding to fewer levels adds the
 * fizz of a low bit depth. The drive saturates first, the tone filter (two poles) smooths last. */
#include "fx_multi.h"
#include "fx_dsp.h"

struct state {
    float hold_l, hold_r, acc, lp1_l, lp1_r, lp2_l, lp2_r;
};
_Static_assert(sizeof(struct state) <= MFX_ARENA_BYTES, "lo-fi: more than the arena");

static inline float quantise(float x, float levels)   /* round to the nearest of 2 x levels steps */
{
    float y = x * levels;
    int32_t i = (int32_t)(y < 0.0f ? y - 0.5f : y + 0.5f);
    return (float)i / levels;
}

static void process(void *p, float *l, float *r, uint32_t frames, const struct mfx_knobs *k)
{
    struct state *s = p;
    float step, levels, lp, drive, makeup;
    uint32_t n;

    step = mfx_expo(1.0f - k->main, 1500.0f, MFX_SR) / MFX_SR;   /* new samples per output sample */
    levels = mfx_exp2(15.0f - 13.0f * k->a);    /* half the number of levels: 16 bits down to 3 */
    lp = mfx_lp_coef(mfx_expo(k->b, 800.0f, 16000.0f));
    drive = 1.0f + 15.0f * k->c * k->c;
    makeup = 1.0f / (1.0f + 0.45f * (drive - 1.0f));
    for (n = 0; n < frames; n++) {
        s->acc += step;
        if (s->acc >= 1.0f) {                   /* time for a new sample */
            s->acc -= 1.0f;
            s->hold_l = quantise(mfx_soft(drive * l[n]) * makeup, levels);
            s->hold_r = quantise(mfx_soft(drive * r[n]) * makeup, levels);
        }
        s->lp1_l += lp * (s->hold_l - s->lp1_l);
        s->lp1_r += lp * (s->hold_r - s->lp1_r);
        s->lp2_l += lp * (s->lp1_l - s->lp2_l);
        s->lp2_r += lp * (s->lp1_r - s->lp2_r);
        l[n] = s->lp2_l;
        r[n] = s->lp2_r;
    }
}

const struct mfx_effect mfx_lofi = {"Lo-fi", sizeof(struct state), process};
