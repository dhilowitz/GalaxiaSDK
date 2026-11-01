/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Hilowitz */
/* 2. Tape echo: one tape loop, its speed wavering, its repeats saturating and darkening.
 *   main  time, 60 ms to 1 s (or the tap tempo)        b  wow and flutter: slow drift and fast waver
 *   a     feedback; past 3 o'clock it runs away into    c  drive: tape saturation, and darker repeats
 *         self-oscillation, held in check by the tape
 * A mono echo, as a tape machine has one head: the input is summed to mono, the echo goes to both sides.
 * The record path saturates (mfx_soft), which is what lets the feedback go past 1 without exploding. A
 * low-pass and a gentle high-pass in the loop give each repeat less top and less bottom. The time knob
 * glides slowly, like changing the motor speed. */
#include "fx_multi.h"
#include "fx_dsp.h"

#define LEN 46000u                              /* 1 s, plus room for the wow */

struct state {
    uint32_t started;
    struct mfx_line tape;
    float time, lp, hp, wow1, wow2, flutter;
    int16_t mem[LEN];
};
_Static_assert(sizeof(struct state) <= MFX_ARENA_BYTES, "tape echo: more than the arena");

static MFX_RAM void process(void *p, float *l, float *r, uint32_t frames, const struct mfx_knobs *k)
{
    struct state *s = p;
    float target, fb, wow, flut, drive, makeup, lp, hp;
    uint32_t n;

    target = k->tempo > 0.0f ? k->tempo : 0.06f + 0.94f * k->main * k->main;
    target = mfx_clamp(target * MFX_SR, 1.0f, (float)(LEN - 200u));
    fb = 1.15f * k->a;
    wow = k->b * 0.0025f * MFX_SR;              /* drift: up to +-2.5 ms */
    flut = k->b * 0.00025f * MFX_SR;            /* waver: up to +-0.25 ms */
    drive = 1.0f + 5.0f * k->c;
    makeup = 1.0f / drive;                      /* quiet signals at their own level: drive only saturates */
    lp = mfx_lp_coef(mfx_expo(1.0f - k->c, 2500.0f, 7000.0f));
    hp = mfx_lp_coef(90.0f);
    if (!s->started) {
        s->tape = (struct mfx_line){s->mem, LEN, 0};
        s->time = target;
        s->started = 1;
    }
    for (n = 0; n < frames; n++) {
        float d, y, x;
        s->time += 0.00015f * (target - s->time);
        d = s->time + 120.0f + wow * (0.6f * mfx_sin_turns(s->wow1) + 0.4f * mfx_sin_turns(s->wow2)) +
            flut * mfx_sin_turns(s->flutter);
        s->wow1 += 0.33f / MFX_SR;
        s->wow2 += 0.87f / MFX_SR;
        s->flutter += 6.4f / MFX_SR;
        if (s->wow1 >= 1.0f) s->wow1 -= 1.0f;
        if (s->wow2 >= 1.0f) s->wow2 -= 1.0f;
        if (s->flutter >= 1.0f) s->flutter -= 1.0f;
        y = mfx_line_read(&s->tape, d);
        s->lp += lp * (y - s->lp);              /* less top ... */
        s->hp += hp * (s->lp - s->hp);
        y = s->lp - s->hp;                      /* ... and less bottom on every pass */
        x = 0.5f * (l[n] + r[n]) + fb * y;
        mfx_line_write(&s->tape, mfx_soft(drive * x) * makeup);
        l[n] = r[n] = y;
    }
}

const struct mfx_effect mfx_tape = {"Tape echo", sizeof(struct state), process};
