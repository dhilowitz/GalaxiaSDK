/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Hilowitz */
/* 8. Plate reverb: the bright, dense, smooth reverb of a vibrating steel plate.
 *   main  decay, short to long                          b  damping: bright to dark
 *   a     predelay, 0 to 200 ms                         c  modulation: a slow shimmer that smooths the tail
 * The structure is Jon Dattorro's plate ("Effect Design, Part 1", J. Audio Eng. Soc. 45(9), 1997): the
 * input is low-passed and smeared by four all-pass diffusers, then fed into a "tank", two chains of
 * all-pass and delay that feed each other in a figure eight, losing a little level and a little treble
 * on each lap. The two outputs are sums of taps from all over the tank, which is why they differ: wide
 * stereo from a mono input. The paper's lengths are for 29.8 kHz; here they are scaled to 44.1 kHz. The
 * first all-pass of each half wobbles its length slowly (the modulation knob), which keeps the tail from
 * ringing at fixed frequencies. */
#include "fx_multi.h"
#include "fx_dsp.h"

#define PRE 8824u                               /* 200 ms */
#define EXC 24u                                 /* the most a modulated all-pass moves, samples */
/* line lengths (the paper's, times 44118 / 29761), each plus 2 so the longest read still fits */
#define D1 211u
#define D2 159u
#define D3 562u
#define D4 411u
#define LA1 996u                                /* left half: modulated all-pass, delay, all-pass, delay */
#define LD1 6601u
#define LA2 2668u
#define LD2 5515u
#define RA1 1346u                               /* right half */
#define RD1 6251u
#define RA2 3937u
#define RD2 4689u

struct state {
    uint32_t started;
    struct mfx_fline pre, d1, d2, d3, d4, la1, ld1, la2, ld2, ra1, rd1, ra2, rd2;
    float bw, damp_l, damp_r, end_l, end_r, phase;
    float m_pre[PRE + 2], m_d1[D1 + 2], m_d2[D2 + 2], m_d3[D3 + 2], m_d4[D4 + 2];
    float m_la1[LA1 + EXC + 2], m_ld1[LD1 + 2], m_la2[LA2 + 2], m_ld2[LD2 + 2];
    float m_ra1[RA1 + EXC + 2], m_rd1[RD1 + 2], m_ra2[RA2 + 2], m_rd2[RD2 + 2];
};
_Static_assert(sizeof(struct state) <= MFX_ARENA_BYTES, "plate: more than the arena");

#define LINE(m) ((struct mfx_fline){m, sizeof m / sizeof m[0], 0})

/* an all-pass of length len and gain g: smears a signal in time without colouring its spectrum */
static inline float allpass(struct mfx_fline *d, float delay, float g, float x)
{
    float y = mfx_fline_read(d, delay), w = x - g * y;
    mfx_fline_write(d, w);
    return y + g * w;
}

static void process(void *p, float *l, float *r, uint32_t frames, const struct mfx_knobs *k)
{
    struct state *s = p;
    float decay, diff2, damp, pre, exc, rate;
    uint32_t n;

    decay = 0.2f + 0.77f * k->main;             /* the level kept on each lap of the tank */
    diff2 = mfx_clamp(decay + 0.15f, 0.25f, 0.5f);   /* the paper's "decay diffusion 2" */
    damp = 0.05f + 0.7f * k->b * k->b;          /* treble lost on each lap */
    pre = 1.0f + k->a * (float)(PRE - 1u);
    exc = k->c * (float)(EXC - 2u);
    rate = 1.0f / MFX_SR;                       /* the modulation's speed, 1 Hz */
    if (!s->started) {
        s->pre = LINE(s->m_pre);
        s->d1 = LINE(s->m_d1); s->d2 = LINE(s->m_d2); s->d3 = LINE(s->m_d3); s->d4 = LINE(s->m_d4);
        s->la1 = LINE(s->m_la1); s->ld1 = LINE(s->m_ld1); s->la2 = LINE(s->m_la2); s->ld2 = LINE(s->m_ld2);
        s->ra1 = LINE(s->m_ra1); s->rd1 = LINE(s->m_rd1); s->ra2 = LINE(s->m_ra2); s->rd2 = LINE(s->m_rd2);
        s->started = 1;
    }
    for (n = 0; n < frames; n++) {
        float x, a, b, mod_l, mod_r;

        /* input: predelay, bandwidth (a gentle low-pass), four diffusers */
        mfx_fline_write(&s->pre, 0.5f * (l[n] + r[n]));
        x = mfx_fline_read(&s->pre, pre);
        s->bw += 0.9995f * (x - s->bw);
        x = allpass(&s->d1, (float)D1, 0.75f, s->bw);
        x = allpass(&s->d2, (float)D2, 0.75f, x);
        x = allpass(&s->d3, (float)D3, 0.625f, x);
        x = allpass(&s->d4, (float)D4, 0.625f, x);

        /* the tank: each half takes the input plus the other half's end, decayed */
        mod_l = exc * (1.0f + mfx_sin_turns(s->phase)) * 0.5f;
        mod_r = exc * (1.0f + mfx_sin_turns(s->phase + 0.25f)) * 0.5f;
        s->phase += rate;
        if (s->phase >= 1.0f)
            s->phase -= 1.0f;

        a = allpass(&s->la1, (float)LA1 + mod_l, -0.7f, x + decay * s->end_r);
        mfx_fline_write(&s->ld1, a);
        a = mfx_fline_tap(&s->ld1, LD1);
        s->damp_l += (1.0f - damp) * (a - s->damp_l);
        a = allpass(&s->la2, (float)LA2, diff2, decay * s->damp_l);
        mfx_fline_write(&s->ld2, a);

        b = allpass(&s->ra1, (float)RA1 + mod_r, -0.7f, x + decay * s->end_l);
        mfx_fline_write(&s->rd1, b);
        b = mfx_fline_tap(&s->rd1, RD1);
        s->damp_r += (1.0f - damp) * (b - s->damp_r);
        b = allpass(&s->ra2, (float)RA2, diff2, decay * s->damp_r);
        mfx_fline_write(&s->rd2, b);

        s->end_l = mfx_fline_tap(&s->ld2, LD2);
        s->end_r = mfx_fline_tap(&s->rd2, RD2);

        /* the outputs: the paper's taps (their lengths scaled too) */
        l[n] = 0.6f * (mfx_fline_tap(&s->rd1, 394) + mfx_fline_tap(&s->rd1, 4409) - mfx_fline_tap(&s->ra2, 2836) +
                       mfx_fline_tap(&s->rd2, 2959) - mfx_fline_tap(&s->ld1, 2950) - mfx_fline_tap(&s->la2, 277) -
                       mfx_fline_tap(&s->ld2, 1580));
        r[n] = 0.6f * (mfx_fline_tap(&s->ld1, 523) + mfx_fline_tap(&s->ld1, 5377) - mfx_fline_tap(&s->la2, 1820) +
                       mfx_fline_tap(&s->ld2, 3962) - mfx_fline_tap(&s->rd1, 3129) - mfx_fline_tap(&s->ra2, 497) -
                       mfx_fline_tap(&s->rd2, 179));
    }
}

const struct mfx_effect mfx_plate = {"Plate reverb", sizeof(struct state), process};
