/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Hilowitz */
/* 1. Digital delay: a clean stereo echo, up to 1.5 s.
 *   main  time, 20 ms to 1.5 s (or the tap tempo)      b  tone of the repeats, dark to bright
 *   a     feedback, one repeat to nearly endless        c  modulation: a slow wobble on the repeats
 * Each channel has its own delay line. The repeats pass through a low-pass filter on every trip, so they
 * darken as they fade, as in most delay pedals. Turning the time knob glides the delay time, so the
 * repeats bend in pitch rather than click. */
#include "fx_multi.h"
#include "fx_dsp.h"

#define LEN 66176u                              /* 1.5 s at 44.1 kHz, per channel */

struct state {
    uint32_t started;
    struct mfx_line l, r;
    float time;                                 /* the delay time now, in samples (it glides to the knob) */
    float lp_l, lp_r, phase;
    int16_t mem[2 * LEN];
};
_Static_assert(sizeof(struct state) <= MFX_ARENA_BYTES, "digital delay: more than the arena");

static MFX_RAM void process(void *p, float *l, float *r, uint32_t frames, const struct mfx_knobs *k)
{
    struct state *s = p;
    float target, fb, lp, depth, rate;
    uint32_t n;

    target = k->tempo > 0.0f ? k->tempo : 0.02f + 1.48f * k->main * k->main;
    target = mfx_clamp(target * MFX_SR, 1.0f, (float)(LEN - 200u));   /* (room for the modulation) */
    fb = 0.95f * k->a;
    lp = mfx_lp_coef(mfx_expo(k->b, 800.0f, 16000.0f));
    depth = k->c * 0.002f * MFX_SR;             /* up to +-2 ms */
    rate = 0.7f / MFX_SR;
    if (!s->started) {
        s->l = (struct mfx_line){s->mem, LEN, 0};
        s->r = (struct mfx_line){s->mem + LEN, LEN, 0};
        s->time = target;
        s->started = 1;
    }
    for (n = 0; n < frames; n++) {
        float d, yl, yr;
        s->time += 0.0004f * (target - s->time);
        d = s->time + depth * (1.0f + mfx_sin_turns(s->phase));
        s->phase += rate;
        if (s->phase >= 1.0f)
            s->phase -= 1.0f;
        yl = mfx_line_read(&s->l, d);
        yr = mfx_line_read(&s->r, d);
        s->lp_l += lp * (yl - s->lp_l);
        s->lp_r += lp * (yr - s->lp_r);
        mfx_line_write(&s->l, l[n] + fb * s->lp_l);
        mfx_line_write(&s->r, r[n] + fb * s->lp_r);
        l[n] = s->lp_l;
        r[n] = s->lp_r;
    }
}

const struct mfx_effect mfx_delay = {"Digital delay", sizeof(struct state), process};
