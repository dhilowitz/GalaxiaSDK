/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Hilowitz */
/* 6. Phaser: a chain of all-pass filters whose frequency sweeps up and down.
 *   main  rate, 0.05 to 8 Hz (or the tap tempo)        b  resonance: feedback around the chain
 *   a     depth: how wide the sweep is                  c  stages: 2, 4, 6 or 8 (more notches)
 * An all-pass filter changes a signal's phase, not its level, by an amount that depends on frequency.
 * Mixed back with the dry signal, every 180 degrees of shift is a notch: a chain of 2N stages gives N
 * notches. Sweeping the filters' frequency (exponentially, as the ear hears pitch) moves the notches.
 * Mix at noon is the classic sound; the right side sweeps half a cycle behind the left. */
#include "fx_multi.h"
#include "fx_dsp.h"

#define STAGES 8u
#define CONTROL 16u                             /* recompute the filters every 16 samples */

struct state {
    float zl[STAGES], zr[STAGES], fb_l, fb_r, phase;
};
_Static_assert(sizeof(struct state) <= MFX_ARENA_BYTES, "phaser: more than the arena");

static inline float coef(float hz)                     /* first-order all-pass coefficient for a break frequency */
{
    float x = MFX_PI * mfx_clamp(hz, 20.0f, 9000.0f) / MFX_SR;
    float t = x * (1.0f + x * x * (0.33333f + x * x * 0.13333f));   /* tan(x), x below 0.65 */
    return (t - 1.0f) / (t + 1.0f);
}

static MFX_RAM void process(void *p, float *l, float *r, uint32_t frames, const struct mfx_knobs *k)
{
    struct state *s = p;
    float rate, octaves, fb, comp, al = 0.0f, ar = 0.0f;
    uint32_t n, i, stages;

    rate = (k->tempo > 0.0f ? 1.0f / k->tempo : mfx_expo(k->main, 0.05f, 8.0f)) / MFX_SR;
    octaves = 0.5f + 4.5f * k->a;               /* the sweep spans up to 5 octaves above 180 Hz */
    fb = 0.85f * k->b;
    comp = 1.0f - 0.5f * fb;                    /* resonance raises the peaks between the notches */
    stages = 2u * (1u + (uint32_t)(k->c * 3.99f));
    for (n = 0; n < frames; n++) {
        float xl, xr;
        if (n % CONTROL == 0u) {
            float sl = 0.5f + 0.5f * mfx_sin_turns(s->phase);
            float sr = 0.5f + 0.5f * mfx_sin_turns(s->phase + 0.5f);
            al = coef(180.0f * mfx_exp2(octaves * sl));
            ar = coef(180.0f * mfx_exp2(octaves * sr));
        }
        s->phase += rate;
        if (s->phase >= 1.0f)
            s->phase -= 1.0f;
        xl = l[n] + fb * s->fb_l;
        xr = r[n] + fb * s->fb_r;
        for (i = 0; i < stages; i++) {          /* y = a x + z;  z = x - a y */
            float yl = al * xl + s->zl[i], yr = ar * xr + s->zr[i];
            s->zl[i] = xl - al * yl;
            s->zr[i] = xr - ar * yr;
            xl = yl;
            xr = yr;
        }
        s->fb_l = xl;
        s->fb_r = xr;
        l[n] = mfx_soft(comp * xl);
        r[n] = mfx_soft(comp * xr);
    }
}

const struct mfx_effect mfx_phaser = {"Phaser", sizeof(struct state), process};
