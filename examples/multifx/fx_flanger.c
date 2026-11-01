/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Hilowitz */
/* 5. Flanger: the jet-plane sweep of a very short, moving delay mixed with the dry signal.
 *   main  manual: where the sweep sits, 0.25 to 8 ms    b  depth of the sweep
 *   a     rate, 0.03 to 4 Hz (or the tap tempo)        c  feedback: negative left of noon, positive right
 * Mixing a signal with a copy delayed by a few milliseconds cancels some frequencies and doubles others: a
 * comb of notches. Moving the delay moves the comb. Feedback deepens it into a ringing, metallic tone
 * (negative feedback puts the peaks where positive feedback has the notches). Mix at noon is the classic
 * flanger; the right side sweeps a quarter of a cycle behind the left. */
#include "fx_multi.h"
#include "fx_dsp.h"

#define LEN 1024u                               /* 23 ms */

struct state {
    uint32_t started;
    struct mfx_fline l, r;
    float phase;
    float mem[2 * LEN];
};
_Static_assert(sizeof(struct state) <= MFX_ARENA_BYTES, "flanger: more than the arena");

static void process(void *p, float *l, float *r, uint32_t frames, const struct mfx_knobs *k)
{
    struct state *s = p;
    float centre, depth, rate, fb, comp;
    uint32_t n;

    centre = mfx_expo(k->main, 0.25f, 8.0f) * 0.001f * MFX_SR;
    depth = 0.9f * k->b;
    rate = (k->tempo > 0.0f ? 1.0f / k->tempo : mfx_expo(k->a, 0.03f, 4.0f)) / MFX_SR;
    fb = 1.8f * (k->c - 0.5f);                  /* -0.9 .. +0.9 */
    comp = 1.0f - 0.6f * (fb < 0.0f ? -fb : fb);   /* feedback piles energy into the comb's peaks: less out */
    if (!s->started) {
        s->l = (struct mfx_fline){s->mem, LEN, 0};
        s->r = (struct mfx_fline){s->mem + LEN, LEN, 0};
        s->started = 1;
    }
    for (n = 0; n < frames; n++) {
        float dl = centre * (1.0f + depth * mfx_sin_turns(s->phase));
        float dr = centre * (1.0f + depth * mfx_sin_turns(s->phase + 0.25f));
        float yl = mfx_fline_read(&s->l, mfx_clamp(dl, 1.0f, (float)(LEN - 2u)));
        float yr = mfx_fline_read(&s->r, mfx_clamp(dr, 1.0f, (float)(LEN - 2u)));
        mfx_fline_write(&s->l, l[n] + fb * yl);
        mfx_fline_write(&s->r, r[n] + fb * yr);
        s->phase += rate;
        if (s->phase >= 1.0f)
            s->phase -= 1.0f;
        l[n] = mfx_soft(comp * yl);             /* and a soft limit for the sharpest resonances */
        r[n] = mfx_soft(comp * yr);
    }
}

const struct mfx_effect mfx_flanger = {"Flanger", sizeof(struct state), process};
