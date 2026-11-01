/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Hilowitz */
/* Nine effects, one for each position of the selector (fx_multi.h, one fx_*.c file each):
 *   1 digital delay   2 tape echo   3 ping-pong delay   4 chorus   5 flanger   6 phaser
 *   7 tremolo / auto-pan   8 plate reverb   9 lo-fi
 * The same layout for every effect:
 *   top-left knob      the main control: time, depth, rate, decay or crush
 *   top-right knob     mix, dry to wet
 *   bottom knobs       the effect's next three controls, left to right (its fx_*.c file lists them)
 *   left footswitch    bypass (LED green while the effect is on)
 *   right footswitch   tap tempo for the delays and the modulation effects (tap twice or more; the LED
 *                      blinks red in time); turning the top-left knob gives it control again
 *   MIDI out           knobs CC 20-25, selector CC 26, bypass CC 80, tap CC 81
 *   console            A<0-8> effect, N<1-5>:<0-1023> a knob (1 main, 2 mix, 3-5 the bottom row), T<ms> tap
 *                      tempo (T0 off), B bypass, G 440 Hz test tone in place of the input, K knobs back in
 *                      control (each followed by Enter) */
#define GX_USB_PRODUCT "Galaxia Multi-FX"
#include "gx_runtime.c"
#include "fx_multi.h"

enum { K_MAIN, K_MIX, K_A, K_B, K_C, K_N };
static const uint8_t knob_of[K_N] = {GX_KNOB_LEFT_TOP, GX_KNOB_RIGHT_TOP, GX_KNOB_LEFT_BOTTOM,
                                     GX_KNOB_MIDDLE_BOTTOM, GX_KNOB_RIGHT_BOTTOM};
static int32_t override[K_N] = {-1, -1, -1, -1, -1}, override_effect = -1;
static uint32_t smooth[GX_KNOBS], step, bypass, sent_cc[GX_KNOBS + 1u];
static uint32_t tempo_ms, last_tap, tap_main;   /* tap tempo; the main knob where it was tapped */
static uint32_t held[K_N];                      /* each knob's steady value (gx_knob_steady: no warble in the
                                                 * delays, whose time glides to it) */

static void app_init(void) {}

static void app_audio(const int32_t *in, int32_t *out, uint32_t frames)
{
    mfx_process(in, out, frames, (uint32_t)gx_mono_input());
}

static void tap(uint32_t now, uint32_t main_knob)
{
    uint32_t gap = now - last_tap;
    if (last_tap && gap >= 100u && gap <= 2000u)                /* a second tap within 2 s: the tempo */
        tempo_ms = tempo_ms && gap + 300u > tempo_ms && gap < tempo_ms + 300u ? (tempo_ms + gap) / 2u : gap;
    last_tap = now;
    tap_main = main_knob;
    gx_midi_cc(81, 127);
}

static void app_poll(uint32_t now)
{
    static uint32_t last_ms, prev_sw;
    uint32_t i, sw = gx_in.switches, v[K_N], blink;
    if (mfx_clear_pending) {                    /* the audio has faded the old effect out */
        memset(mfx_arena, 0, sizeof mfx_arena);
        mfx_cleared();
    }
    if (now - last_ms < 5u)
        return;
    last_ms = now;
    for (i = 0; i < GX_KNOBS; i++) {             /* smoothing in 1/16 counts, integer only */
        uint32_t raw = (uint32_t)gx_in.knob[i] << 4, k;
        smooth[i] = smooth[i] ? smooth[i] + (raw >> 2) - (smooth[i] >> 2) : raw + 1u;
        k = smooth[i] >> 4;
        if (k > sent_cc[i] * 8u + 12u || k + 12u < sent_cc[i] * 8u ||
            (k >= 1016u && sent_cc[i] != 127u) || (k <= 4u && sent_cc[i] != 0u)) {
            sent_cc[i] = k >> 3;
            gx_midi_cc(20u + i, sent_cc[i]);
        }
    }
    step = gx_selector_position(smooth[GX_KNOB_SELECTOR] >> 4, step);
    if (step != sent_cc[GX_KNOBS]) {
        sent_cc[GX_KNOBS] = step;
        gx_midi_cc(26, step);
    }
    for (i = 0; i < K_N; i++)
        v[i] = override[i] >= 0 ? (uint32_t)override[i] : gx_knob_steady(&held[i], smooth[knob_of[i]] >> 4);
    if ((sw & ~prev_sw) & GX_SW_LEFT) {
        bypass ^= 1u;
        gx_midi_cc(80, bypass ? 0u : 127u);
    }
    if ((sw & ~prev_sw) & GX_SW_RIGHT)
        tap(now, v[K_MAIN]);
    prev_sw = sw;
    if (tempo_ms && (v[K_MAIN] > tap_main + 40u || v[K_MAIN] + 40u < tap_main))
        tempo_ms = 0;                           /* the knob moved: it rules again */

    mfx_ctl.knob[0] = (uint16_t)v[K_MAIN];
    mfx_ctl.knob[1] = (uint16_t)v[K_A];
    mfx_ctl.knob[2] = (uint16_t)v[K_B];
    mfx_ctl.knob[3] = (uint16_t)v[K_C];
    mfx_ctl.mix = (uint16_t)v[K_MIX];
    mfx_ctl.tempo_ms = (uint16_t)tempo_ms;
    mfx_ctl.effect = (uint8_t)(override_effect >= 0 ? (uint32_t)override_effect : step);
    mfx_ctl.bypass = (uint8_t)bypass;
    blink = tempo_ms && (now - last_tap) % tempo_ms < 60u;   /* a short red flash on each beat */
    gx_led((int)blink, !bypass);
}

static int app_command(const char *s, uint32_t n)
{
    uint32_t v = 0, w = 0, i, have = 0, colon = 0;
    for (i = 1; i < n; i++) {
        if (s[i] == ':') {
            colon = 1;
            w = v;
            v = 0;
            have = 0;
        } else if (s[i] >= '0' && s[i] <= '9') {
            v = v * 10u + (uint32_t)(s[i] - '0');
            have = 1;
        }
    }
    switch (s[0]) {
    case 'h':
        gx_puts("multifx: A<0-8> effect, N<1-5>:<0-1023> knob (1 main, 2 mix, 3-5 bottom row), T<ms> tempo, "
                "B bypass, G test tone, K knobs (each then Enter)\r\n");
        return 0;
    case 'A': if (!have) return 0; override_effect = (int32_t)(v > 8u ? 8u : v); return 1;
    case 'N':
        if (!colon || !have || w < 1u || w > K_N) return 0;
        override[w - 1u] = (int32_t)(v > 1023u ? 1023u : v);
        return 1;
    case 'T': if (!have) return 0; tempo_ms = v > 2000u ? 2000u : v; tap_main = mfx_ctl.knob[0]; return 1;
    case 'B': bypass ^= 1u; return 1;
    case 'G': mfx_ctl.test_tone ^= 1u; return 1;
    case 'K':
        for (i = 0; i < K_N; i++)
            override[i] = -1;
        override_effect = -1;
        return 1;
    }
    return 0;
}

static void app_status(void)
{
    uint32_t e = mfx_running();
    gx_putd("effect=", e);                      /* 0..8: digital delay .. lo-fi */
    gx_putd("step=", step);
    gx_putd("bypass=", bypass);
    gx_putd("tempo_ms=", tempo_ms);
    gx_putd("test_tone=", mfx_ctl.test_tone);
    gx_putd("effect_bytes=", mfx_effects[e]->bytes);
    gx_putd("ov=", (uint32_t)((override[0] >= 0) | (override[1] >= 0) << 1 | (override[2] >= 0) << 2 |
                              (override[3] >= 0) << 3 | (override[4] >= 0) << 4 | (override_effect >= 0) << 5));
}
