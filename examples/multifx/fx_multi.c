/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Hilowitz */
/* The frame the nine effects run in (see fx_multi.h): samples in and out, knob smoothing, dry/wet mix,
 * bypass, and the fade out, clear and fade in when the selector picks another effect. The mix knob keeps
 * the dry signal at full level up to noon while the effect comes in, then fades the dry out: no dip in
 * level in the middle, as with many pedals. */
#include "fx_multi.h"
#include "fx_dsp.h"

#define MAX_FRAMES 128u                         /* the runtime's block (HALF_FRAMES) */
#define FADE_BLOCKS 4u                          /* fade in over 4 blocks, about 12 ms */

const struct mfx_effect *const mfx_effects[MFX_COUNT] = {
    &mfx_delay, &mfx_tape, &mfx_pingpong, &mfx_chorus, &mfx_flanger, &mfx_phaser, &mfx_tremolo, &mfx_plate,
    &mfx_lofi,
};

struct mfx_controls mfx_ctl = {{512, 400, 600, 300}, 400, 0, 0, 0, 0};
uint8_t mfx_arena[MFX_ARENA_BYTES] __attribute__((aligned(8)));
volatile uint32_t mfx_clear_pending;

static volatile uint32_t cleared;
static uint32_t running;
static enum { RUN, FADE_OUT, WAIT_CLEAR } state;
static float wet_gain, s_knob[4], s_mix, s_bypass, tone_phase;
static uint32_t primed;
static float bl[MAX_FRAMES], br[MAX_FRAMES], dl[MAX_FRAMES], dr[MAX_FRAMES];

void mfx_cleared(void)
{
    mfx_clear_pending = 0;
    cleared = 1;
}

uint32_t mfx_running(void) { return running; }

void mfx_process(const int32_t *in, int32_t *out, uint32_t frames, uint32_t mono_input)
{
    const float to_f = 1.0f / 8388608.0f, to_i = 8388607.0f, k = 1.0f / 1023.0f;
    struct mfx_knobs kn;
    uint32_t n, i, want = mfx_ctl.effect;
    float g0, g1, mix_t, byp_t, d_mix, d_byp, d_g;

    if (frames > MAX_FRAMES)
        frames = MAX_FRAMES;
    /* knobs: one smoothing step a block (about 10 ms to settle), so a knob never steps audibly */
    for (i = 0; i < 4u; i++) {
        float t = (float)mfx_ctl.knob[i] * k;
        s_knob[i] = primed ? s_knob[i] + 0.3f * (t - s_knob[i]) : t;
    }
    if (!primed) {
        s_mix = (float)mfx_ctl.mix * k;
        primed = 1;
    }
    kn.main = s_knob[0];
    kn.a = s_knob[1];
    kn.b = s_knob[2];
    kn.c = s_knob[3];
    kn.tempo = (float)mfx_ctl.tempo_ms * 0.001f;

    for (n = 0; n < frames; n++) {
        float xl, xr;
        if (mfx_ctl.test_tone) {                /* 440 Hz at -15 dBFS in place of the input (console G) */
            xl = xr = 0.178f * mfx_sin_turns(tone_phase);
            tone_phase += 440.0f / MFX_SR;
            if (tone_phase >= 1.0f)
                tone_phase -= 1.0f;
        } else {
            xl = (float)in[2 * n] * to_f;
            xr = mono_input ? xl : (float)in[2 * n + 1] * to_f;
        }
        dl[n] = bl[n] = xl;
        dr[n] = br[n] = xr;
    }

    /* the effect: fade it out when the selector moves on, wait for the main loop to clear the arena (no
     * float code there), then fade the new one in from its all-zero state */
    g0 = wet_gain;
    if (state == RUN && want != running && want < MFX_COUNT)
        state = FADE_OUT;
    if (state == WAIT_CLEAR && cleared) {
        cleared = 0;
        running = want < MFX_COUNT ? want : running;
        state = RUN;
    }
    if (state == WAIT_CLEAR) {
        for (n = 0; n < frames; n++)
            bl[n] = br[n] = 0.0f;
        g1 = 0.0f;
    } else {
        mfx_effects[running]->process(mfx_arena, bl, br, frames, &kn);
        if (state == FADE_OUT) {
            g1 = 0.0f;
            state = WAIT_CLEAR;
            mfx_clear_pending = 1;
        } else {
            g1 = g0 + 1.0f / (float)FADE_BLOCKS;
            if (g1 > 1.0f)
                g1 = 1.0f;
        }
    }

    mix_t = (float)mfx_ctl.mix * k;
    byp_t = mfx_ctl.bypass ? 1.0f : 0.0f;
    d_mix = (mix_t - s_mix) / (float)frames;
    d_byp = (byp_t - s_bypass) / (float)frames;
    d_g = (g1 - g0) / (float)frames;
    for (n = 0; n < frames; n++) {             /* the mix: dry full up to noon, wet full from noon */
        float ol, or_, wet, dry;
        s_mix += d_mix;
        s_bypass += d_byp;
        g0 += d_g;
        dry = mfx_clamp(2.0f - 2.0f * s_mix, 0.0f, 1.0f);
        wet = mfx_clamp(2.0f * s_mix, 0.0f, 1.0f) * g0;
        ol = dry * dl[n] + wet * bl[n];
        or_ = dry * dr[n] + wet * br[n];
        ol = s_bypass * dl[n] + (1.0f - s_bypass) * ol;
        or_ = s_bypass * dr[n] + (1.0f - s_bypass) * or_;
        out[2 * n] = (int32_t)(mfx_clamp(ol, -0.999f, 0.999f) * to_i);
        out[2 * n + 1] = (int32_t)(mfx_clamp(or_, -0.999f, 0.999f) * to_i);
    }
    s_mix = mix_t;
    s_bypass = byp_t;
    wet_gain = g1;
}
