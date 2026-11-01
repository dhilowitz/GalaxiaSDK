/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Hilowitz */
/* 4. Chorus: three slightly detuned copies of the input, spread across the stereo field.
 *   main  depth, how far the copies drift in pitch     b  width: from all in the middle to left, centre, right
 *   a     rate of the drift, 0.08 to 6 Hz (or the tap)  c  tone of the copies
 * One short delay line holds the last few milliseconds; three voices read it at delays around 10 ms that
 * a slow sine (a third of a cycle apart for each voice) moves back and forth. A moving delay is a small
 * pitch change, so each voice is a little sharp, then a little flat. With mix fully up it is a vibrato. */
#include "fx_multi.h"
#include "fx_dsp.h"

#define LEN 2048u                               /* 46 ms */
#define VOICES 3u

struct state {
    uint32_t started;
    struct mfx_fline line;
    float phase, lp_l, lp_r;
    float mem[LEN];
};
_Static_assert(sizeof(struct state) <= MFX_ARENA_BYTES, "chorus: more than the arena");

static MFX_RAM void process(void *p, float *l, float *r, uint32_t frames, const struct mfx_knobs *k)
{
    static const float base_ms[VOICES] = {8.0f, 11.0f, 14.5f};
    struct state *s = p;
    float depth, rate, lp, pan[VOICES], base[VOICES];
    uint32_t n, v;

    depth = (0.2f + 3.8f * k->main) * 0.001f * MFX_SR;   /* the swing, 0.2 to 4 ms */
    rate = (k->tempo > 0.0f ? 1.0f / k->tempo : mfx_expo(k->a, 0.08f, 6.0f)) / MFX_SR;
    lp = mfx_lp_coef(mfx_expo(k->c, 1500.0f, 16000.0f));
    pan[0] = 0.5f + 0.5f * k->b;                /* voice 0 towards the left, 2 the right, 1 the middle */
    pan[1] = 0.5f;
    pan[2] = 0.5f - 0.5f * k->b;
    for (v = 0; v < VOICES; v++)
        base[v] = base_ms[v] * 0.001f * MFX_SR;
    if (!s->started) {
        s->line = (struct mfx_fline){s->mem, LEN, 0};
        s->started = 1;
    }
    for (n = 0; n < frames; n++) {
        float wl = 0.0f, wr = 0.0f;
        mfx_fline_write(&s->line, 0.5f * (l[n] + r[n]));
        for (v = 0; v < VOICES; v++) {
            float y = mfx_fline_read(&s->line, base[v] + depth * (1.0f + mfx_sin_turns(s->phase + (float)v * 0.3333f)));
            wl += pan[v] * y;
            wr += (1.0f - pan[v]) * y;
        }
        s->phase += rate;
        if (s->phase >= 1.0f)
            s->phase -= 1.0f;
        s->lp_l += lp * (0.8f * wl - s->lp_l);
        s->lp_r += lp * (0.8f * wr - s->lp_r);
        l[n] = s->lp_l;
        r[n] = s->lp_r;
    }
}

const struct mfx_effect mfx_chorus = {"Chorus", sizeof(struct state), process};
