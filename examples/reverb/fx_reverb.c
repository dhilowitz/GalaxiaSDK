/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Hilowitz */
/* Galaxia stereo reverb (see fx_reverb.h). Measured on a Galaxia: T60 3.07-3.14 s for a 3.23 s setting,
 * L/R correlation 0.15, 13 % of the audio block. */
#include "fx_reverb.h"

#define RAMFN_FX __attribute__((section(".ram_text.fx"), noinline))
#define SR 44118.0f
#define LINES 8
#define PREDELAY_MAX 5292u                         /* 120 ms */
#define AP_COUNT 2
#define SIZE_MIN 0.35f                             /* smallest room: 35 % of each length */

/* Mutually prime line lengths at the largest room size (about 34 to 99 ms). */
static const uint16_t line_max[LINES] = {1499, 1877, 2251, 2647, 3001, 3373, 3797, 4373};
static const uint16_t ap_len[AP_COUNT] = {347, 557};
#define LINE_TOTAL (1499 + 1877 + 2251 + 2647 + 3001 + 3373 + 3797 + 4373)

static float line_mem[LINE_TOTAL];
static float ap_mem[347 + 557];
static float pre_buf[PREDELAY_MAX];
const uint32_t rv_bytes = sizeof line_mem + sizeof ap_mem + sizeof pre_buf;

static float *line_buf[LINES], *ap_buf[AP_COUNT];
static uint16_t line_pos[LINES], ap_pos[AP_COUNT], pre_pos;
static float line_lp[LINES];
static float s_mix, s_level, s_bypass;

struct rv_controls rv_ctl = {512, 512, 0, 400, 512, 6, 0, 0};

void rv_init(void)
{
    uint32_t i, at = 0;
    for (i = 0; i < LINES; i++) {
        line_buf[i] = &line_mem[at];
        at += line_max[i];
    }
    ap_buf[0] = &ap_mem[0];
    ap_buf[1] = &ap_mem[347];
}

/* e^x: 2^(x log2 e), the integer part in the float exponent, a 5th-order polynomial for the
 * fraction (relative error under 1e-4 over the range used here). No libm in this build. */
static RAMFN_FX float rv_expf(float x)
{
    float y = x * 1.44269504f, f, p;
    int i;
    union { uint32_t u; float f; } s;
    if (y < -126.0f)
        return 0.0f;
    if (y > 127.0f)
        y = 127.0f;
    i = (int)y;
    if ((float)i > y)
        i--;
    f = y - (float)i;
    p = 1.0f + f * (0.69314718f + f * (0.24022650f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
    s.u = (uint32_t)(i + 127) << 23;
    return p * s.f;
}

static RAMFN_FX float allpass(uint32_t i, float x)
{
    const float g = 0.6f;
    float *b = ap_buf[i], d, v;
    uint32_t p = ap_pos[i];
    d = b[p];
    v = x + g * d;
    b[p] = v;
    if (++p >= ap_len[i])
        p = 0;
    ap_pos[i] = (uint16_t)p;
    return d - g * v;
}

RAMFN_FX void rv_process(const int32_t *in, int32_t *out, uint32_t frames, uint32_t mono_input)
{
    const float to_f = 1.0f / 8388608.0f, to_i = 8388607.0f, k = 1.0f / 1023.0f;
    uint32_t len[LINES], i, n, pre, size_step, freeze = rv_ctl.freeze;
    float gain[LINES], size, decay, t60, cutoff, damp, in_gain;
    float mix_t, level_t, byp_t, d_mix, d_level, d_byp;

    /* Coefficients, once per block. */
    size_step = rv_ctl.size > 8u ? 8u : rv_ctl.size;
    size = SIZE_MIN + (1.0f - SIZE_MIN) * (float)size_step * 0.125f;
    decay = (float)rv_ctl.decay * k;
    t60 = 0.3f + 11.7f * decay * decay;                   /* 0.3 .. 12 s */
    for (i = 0; i < LINES; i++) {
        len[i] = (uint32_t)((float)line_max[i] * size);
        gain[i] = freeze ? 1.0f : rv_expf(-6.90775528f * (float)len[i] / (t60 * SR));   /* 10^(-3 len / (t60 fs)) */
    }
    cutoff = 1500.0f * rv_expf(2.07944154f * (float)rv_ctl.tone * k);                  /* 1.5 .. 12 kHz */
    damp = freeze ? 1.0f : 1.0f - rv_expf(-6.2831853f * cutoff / SR);
    pre = (uint32_t)((float)rv_ctl.predelay * k * (float)(PREDELAY_MAX - 1u));
    in_gain = freeze ? 0.0f : 1.0f;
    mix_t = (float)rv_ctl.mix * k;
    level_t = (float)rv_ctl.level * k * 2.0f;
    byp_t = rv_ctl.bypass ? 1.0f : 0.0f;
    d_mix = (mix_t - s_mix) / (float)frames;
    d_level = (level_t - s_level) / (float)frames;
    d_byp = (byp_t - s_bypass) / (float)frames;

    for (n = 0; n < frames; n++) {
        float xl = (float)in[2 * n] * to_f;
        float xr = mono_input ? xl : (float)in[2 * n + 1] * to_f;
        float x, y[LINES], h[LINES], wl, wr, wet, dry, ol, or_;
        uint32_t rp, s, j;

        rp = pre_pos >= pre ? pre_pos - pre : pre_pos + PREDELAY_MAX - pre;
        x = pre_buf[rp];
        pre_buf[pre_pos] = 0.5f * (xl + xr);
        if (++pre_pos >= PREDELAY_MAX)
            pre_pos = 0;
        x = allpass(1, allpass(0, x)) * in_gain;

        for (i = 0; i < LINES; i++) {                       /* line outputs (delay len[i]), damped */
            uint32_t p = line_pos[i];
            uint32_t r = p >= len[i] ? p - len[i] : p + line_max[i] - len[i];
            line_lp[i] += damp * (line_buf[i][r] - line_lp[i]);
            y[i] = line_lp[i];
            h[i] = y[i];
        }
        for (s = 1; s < LINES; s <<= 1)                     /* fast Hadamard */
            for (i = 0; i < LINES; i += s << 1)
                for (j = i; j < i + s; j++) {
                    float a = h[j], b = h[j + s];
                    h[j] = a + b;
                    h[j + s] = a - b;
                }
        for (i = 0; i < LINES; i++) {
            uint32_t p = line_pos[i];
            line_buf[i][p] = h[i] * 0.35355339f * gain[i] + ((i & 1u) ? -x : x);
            if (++p >= line_max[i])
                p = 0;
            line_pos[i] = (uint16_t)p;
        }
        wl = 0.5f * (y[0] - y[2] + y[4] - y[6] + y[1] + y[5]);
        wr = 0.5f * (y[1] - y[3] + y[5] - y[7] + y[2] + y[6]);

        s_mix += d_mix;
        s_level += d_level;
        s_bypass += d_byp;
        wet = s_mix;
        dry = 1.0f - s_mix;
        ol = (dry * xl + wet * wl) * s_level;
        or_ = (dry * xr + wet * wr) * s_level;
        ol = s_bypass * xl + (1.0f - s_bypass) * ol;
        or_ = s_bypass * xr + (1.0f - s_bypass) * or_;
        if (ol > 0.999f) ol = 0.999f; else if (ol < -0.999f) ol = -0.999f;
        if (or_ > 0.999f) or_ = 0.999f; else if (or_ < -0.999f) or_ = -0.999f;
        out[2 * n] = (int32_t)(ol * to_i);
        out[2 * n + 1] = (int32_t)(or_ * to_i);
    }
    s_mix = mix_t;
    s_level = level_t;
    s_bypass = byp_t;
}
