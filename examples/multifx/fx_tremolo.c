/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Hilowitz */
/* 7. Tremolo and auto-pan: the volume rising and falling in time.
 *   main  rate, 0.5 to 12 Hz (or the tap tempo)        b  shape: smooth sine to choppy square
 *   a     depth                                         c  pan: tremolo on both sides, to left against right
 * A sine wave turns the volume down and up. Shaping it towards a square makes the change abrupt. Inverting
 * it on the right side turns a tremolo into an auto-pan: the sound moves from side to side. Mix fully up
 * for the full effect (lower mixes make it shallower). */
#include "fx_multi.h"
#include "fx_dsp.h"

struct state {
    float phase;
};
_Static_assert(sizeof(struct state) <= MFX_ARENA_BYTES, "tremolo: more than the arena");

static void process(void *p, float *l, float *r, uint32_t frames, const struct mfx_knobs *k)
{
    struct state *s = p;
    float rate, depth, sq, inv;
    uint32_t n;

    rate = (k->tempo > 0.0f ? 1.0f / k->tempo : mfx_expo(k->main, 0.5f, 12.0f)) / MFX_SR;
    depth = k->a;
    sq = 12.0f * k->b * k->b;                   /* 0: sine; 12: nearly square */
    inv = 1.0f - 2.0f * k->c;                   /* the right side's LFO: same (1) to opposite (-1) */
    for (n = 0; n < frames; n++) {
        float w = mfx_sin_turns(s->phase), a = w < 0.0f ? -w : w;
        w = w * (1.0f + sq) / (1.0f + sq * a);  /* soft-squared: same peaks, steeper edges */
        s->phase += rate;
        if (s->phase >= 1.0f)
            s->phase -= 1.0f;
        l[n] *= 1.0f - depth * (0.5f + 0.5f * w);
        r[n] *= 1.0f - depth * (0.5f + 0.5f * w * inv);
    }
}

const struct mfx_effect mfx_tremolo = {"Tremolo", sizeof(struct state), process};
