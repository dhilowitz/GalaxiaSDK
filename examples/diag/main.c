/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Hilowitz */
/* Audio link diagnostics: with cables to an audio interface, see what the converters really do.
 * M0 (the default)
 *   left output    a 1 kHz sine at -12 dBFS (in the sample format chosen with O)
 *   right output   the left input as received (converted as chosen with I), so the computer's USB capture
 *                  shows the tone (left) and what came back (right)
 * M1  both inputs straight to both outputs (left to left, right to right), unity gain
 * M2  1 kHz on the left output, 1.5 kHz on the right (separation, both converters)
 * Console (then Enter): M<0-2> the mode; O0 output as signed 24-bit in the low bits (the runtime's
 * format), O1 the same shifted up 8 bits (left-justified); I0 input words read as low 24 bits, I1 as the
 * top 24 bits; T<0|1> tone off / on. The status line adds the raw received words of the block's first
 * frames and the peak input level of each channel. */
#define GX_USB_PRODUCT "Galaxia Diag"
#include "gx_runtime.c"

static volatile uint32_t out_fmt, in_fmt, tone_on = 1, mode;
static uint32_t phase, phase_r;
static volatile int32_t peak_in[2];
static volatile int32_t raw[4], raw_min, raw_max;
static volatile uint32_t half_a, half_b, same_twice;   /* which tx half each block was written to */
static const int32_t *first_out, *last_out;
static volatile uint32_t con0_set[4], con0_flips[4], con0_last;   /* CON0 bits 12..15 at each block */

static void app_init(void) { gx_led(0, 1); }

static void app_audio(const int32_t *in, int32_t *out, uint32_t frames)
{
    const int32_t *r = gx_rx_raw;
    int32_t lo = 0x7FFFFFFF, hi = (int32_t)0x80000000;
    uint32_t i;
    (void)in;
    if (!first_out)
        first_out = out;
    if (out == first_out) half_a++; else half_b++;
    if (out == last_out) same_twice++;
    last_out = out;
    {
        uint32_t c = GX_ALNK_CON0, b;
        for (b = 0; b < 4u; b++) {
            if (c & (0x1000u << b)) con0_set[b]++;
            if ((c ^ con0_last) & (0x1000u << b)) con0_flips[b]++;
        }
        con0_last = c;
    }
    for (i = 0; i < 4u; i++)
        raw[i] = r[i];
    if (mode == 1u) {
        for (i = 0; i < 2u * frames; i++) {
            int32_t v = (int32_t)((uint32_t)r[i] << 8) >> 8, a = v < 0 ? -v : v;
            out[i] = v;
            if (a > peak_in[i & 1u])
                peak_in[i & 1u] = a;
        }
        return;
    }
    for (i = 0; i < frames; i++) {
        /* 1000 Hz at 44118 Hz: a parabolic sine (as Colorways' test tone), amplitude 0.25 */
        float ph, x, y;
        int32_t s, back;
        phase += 97352548u;                         /* 1000 Hz: 2^32 * 1000 / 44118 */
        ph = (float)phase * (1.0f / 4294967296.0f);
        x = ph < 0.5f ? ph : ph - 1.0f;
        y = 8.0f * x - 16.0f * x * (x < 0.0f ? -x : x);
        y = 0.225f * (y * (y < 0.0f ? -y : y) - y) + y;
        s = tone_on ? (int32_t)(y * 0.25f * 8388607.0f) : 0;
        out[2 * i] = out_fmt ? (int32_t)((uint32_t)s << 8) : s;
        back = in_fmt ? r[2 * i] >> 8 : (int32_t)((uint32_t)r[2 * i] << 8) >> 8;
        out[2 * i + 1] = back;
        if (mode == 2u) {                           /* 1.5 kHz on the right */
            phase_r += 146028822u;                  /* 2^32 * 1500 / 44118 */
            ph = (float)phase_r * (1.0f / 4294967296.0f);
            x = ph < 0.5f ? ph : ph - 1.0f;
            y = 8.0f * x - 16.0f * x * (x < 0.0f ? -x : x);
            y = 0.225f * (y * (y < 0.0f ? -y : y) - y) + y;
            out[2 * i + 1] = tone_on ? (int32_t)(y * 0.25f * 8388607.0f) : 0;
        }
        if (r[2 * i] < lo) lo = r[2 * i];
        if (r[2 * i] > hi) hi = r[2 * i];
    }
    raw_min = lo;
    raw_max = hi;
}

static void app_poll(uint32_t now_ms) { (void)now_ms; }

static int app_command(const char *s, uint32_t n)
{
    if (n < 2u || s[1] < '0' || s[1] > '2')
        return 0;
    switch (s[0]) {
    case 'M': case 'm': mode = (uint32_t)(s[1] - '0'); peak_in[0] = peak_in[1] = 0; return 1;
    case 'O': case 'o': if (s[1] > '1') return 0; out_fmt = (uint32_t)(s[1] - '0'); return 1;
    case 'I': case 'i': if (s[1] > '1') return 0; in_fmt = (uint32_t)(s[1] - '0'); return 1;
    case 'T': case 't': if (s[1] > '1') return 0; tone_on = (uint32_t)(s[1] - '0'); return 1;
    }
    return 0;
}

static void app_status(void)
{
    uint32_t i;
    gx_putd("mode=", mode);
    gx_putd("peak_in_l=", (uint32_t)peak_in[0]);
    gx_putd("peak_in_r=", (uint32_t)peak_in[1]);
    peak_in[0] = peak_in[1] = 0;
    gx_putd("out_fmt=", out_fmt);
    gx_putd("in_fmt=", in_fmt);
    gx_putd("tone=", tone_on);
    gx_puts("raw=");
    for (i = 0; i < 4u; i++)
        gx_puthex(" ", (uint32_t)raw[i]);
    gx_puthex(" raw_min=", (uint32_t)raw_min);
    gx_puthex(" raw_max=", (uint32_t)raw_max);
    gx_putd(" half_a=", half_a);
    gx_putd("half_b=", half_b);
    gx_putd("same_twice=", same_twice);
    for (i = 0; i < 4u; i++) {
        gx_putd(" b", 12u + i);
        gx_putd("set=", con0_set[i]);
        gx_putd("flips=", con0_flips[i]);
    }
}
