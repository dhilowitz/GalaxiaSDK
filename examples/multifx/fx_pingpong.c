/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Hilowitz */
/* 3. Ping-pong delay: echoes that bounce from left to right.
 *   main  time between bounces, 20 ms to 1.5 s (or the tap tempo)
 *   a     feedback                                     c  spread: from both sides alike to hard left/right
 *   b     tone of the repeats
 * Two delay lines that feed each other: the input goes into the left one, whose echo is passed to the
 * right one, whose echo comes back to the left with the feedback, and so on. So the first echo is on the
 * left, the second on the right, each a little quieter. */
#include "fx_multi.h"
#include "fx_dsp.h"

#define LEN 66176u                              /* 1.5 s per side */

struct state {
    uint32_t started;
    struct mfx_line l, r;
    float time, lp_l, lp_r;
    int16_t mem[2 * LEN];
};
_Static_assert(sizeof(struct state) <= MFX_ARENA_BYTES, "ping-pong delay: more than the arena");

static void process(void *p, float *l, float *r, uint32_t frames, const struct mfx_knobs *k)
{
    struct state *s = p;
    float target, fb, lp, near, far;
    uint32_t n;

    target = k->tempo > 0.0f ? k->tempo : 0.02f + 1.48f * k->main * k->main;
    target = mfx_clamp(target * MFX_SR, 1.0f, (float)(LEN - 4u));
    fb = 0.95f * k->a;
    lp = mfx_lp_coef(mfx_expo(k->b, 800.0f, 16000.0f));
    near = 0.5f + 0.5f * k->c;                  /* how much of each side's echo stays on its side */
    far = 1.0f - near;
    if (!s->started) {
        s->l = (struct mfx_line){s->mem, LEN, 0};
        s->r = (struct mfx_line){s->mem + LEN, LEN, 0};
        s->time = target;
        s->started = 1;
    }
    for (n = 0; n < frames; n++) {
        float yl, yr;
        s->time += 0.0004f * (target - s->time);
        yl = mfx_line_read(&s->l, s->time);
        yr = mfx_line_read(&s->r, s->time);
        s->lp_l += lp * (yl - s->lp_l);
        s->lp_r += lp * (yr - s->lp_r);
        mfx_line_write(&s->l, 0.5f * (l[n] + r[n]) + fb * s->lp_r);   /* in, plus the echo from the right */
        mfx_line_write(&s->r, fb * s->lp_l);                         /* the left echo crosses over */
        l[n] = near * s->lp_l + far * s->lp_r;
        r[n] = near * s->lp_r + far * s->lp_l;
    }
}

const struct mfx_effect mfx_pingpong = {"Ping-pong delay", sizeof(struct state), process};
