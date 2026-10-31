/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Hilowitz */
/* Stereo reverb (fx_reverb.c) on the Galaxia's controls.
 *   left footswitch    bypass on / off (LED green while the effect is on)
 *   right footswitch   freeze while held (LED red; yellow with green)
 *   knobs              left-bottom mix, left-top decay, right-top tone, bottom-right predelay,
 *                      middle-bottom level; the selector steps the room size through 9 positions
 *   MIDI out           knobs CC 20-25, selector step CC 26, bypass CC 80, freeze CC 81
 *   console            D/T/P/M/L<0-1023> decay tone predelay mix level, S<0-8> size, B bypass,
 *                      F freeze, K knobs back in control (each followed by Enter) */
#define GX_USB_PRODUCT "Galaxia Reverb"
#include "gx_runtime.c"
#include "fx_reverb.h"

enum { OV_DECAY, OV_TONE, OV_PREDELAY, OV_MIX, OV_LEVEL, OV_N };
static int32_t override[OV_N] = {-1, -1, -1, -1, -1};
static int32_t override_size = -1;
static uint32_t smooth[GX_KNOBS], step, bypass, freeze_latched, sent_cc[GX_KNOBS + 1u];

static void app_init(void) { rv_init(); }

static void app_audio(const int32_t *in, int32_t *out, uint32_t frames)
{
    rv_process(in, out, frames, (uint32_t)gx_mono_input());
}

static void app_poll(uint32_t now)
{
    static uint32_t last_ms, prev_sw;
    static const uint8_t knob_of[OV_N] = {GX_KNOB_LEFT_TOP, GX_KNOB_RIGHT_TOP, GX_KNOB_RIGHT_BOTTOM,
                                          GX_KNOB_LEFT_BOTTOM, GX_KNOB_MIDDLE_BOTTOM};
    uint32_t i, sw = gx_in.switches, v[OV_N];
    if (now - last_ms < 5u)
        return;
    last_ms = now;
    for (i = 0; i < GX_KNOBS; i++) {                     /* smoothing in 1/16 counts, integer only */
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
    if ((sw & ~prev_sw) & GX_SW_LEFT) {
        bypass ^= 1u;
        gx_midi_cc(80, bypass ? 0u : 127u);
    }
    if ((sw ^ prev_sw) & GX_SW_RIGHT)
        gx_midi_cc(81, (sw & GX_SW_RIGHT) ? 127u : 0u);
    prev_sw = sw;
    for (i = 0; i < OV_N; i++)
        v[i] = override[i] >= 0 ? (uint32_t)override[i] : smooth[knob_of[i]] >> 4;
    rv_ctl.decay = (uint16_t)v[OV_DECAY];
    rv_ctl.tone = (uint16_t)v[OV_TONE];
    rv_ctl.predelay = (uint16_t)v[OV_PREDELAY];
    rv_ctl.mix = (uint16_t)v[OV_MIX];
    rv_ctl.level = (uint16_t)v[OV_LEVEL];
    rv_ctl.size = (uint8_t)(override_size >= 0 ? (uint32_t)override_size : step);
    rv_ctl.freeze = (uint8_t)(((sw & GX_SW_RIGHT) != 0) | freeze_latched);
    rv_ctl.bypass = (uint8_t)bypass;
    gx_led(rv_ctl.freeze, !bypass);
}

static int app_command(const char *s, uint32_t n)
{
    uint32_t v = 0, i, have = 0;
    for (i = 1; i < n; i++)
        if (s[i] >= '0' && s[i] <= '9') {
            v = v * 10u + (uint32_t)(s[i] - '0');
            have = 1;
        }
    if (v > 1023u)
        v = 1023u;
    switch (s[0]) {
    case 'h':
        gx_puts("reverb: D/T/P/M/L<0-1023> decay tone predelay mix level, S<0-8> size, B bypass, "
                "F freeze, K knobs (each then Enter)\r\n");
        return 0;
    case 'D': case 'd': if (!have) return 0; override[OV_DECAY] = (int32_t)v; return 1;
    case 'T': case 't': if (!have) return 0; override[OV_TONE] = (int32_t)v; return 1;
    case 'P': if (!have) return 0; override[OV_PREDELAY] = (int32_t)v; return 1;   /* (lower-case p: status) */
    case 'M': case 'm': if (!have) return 0; override[OV_MIX] = (int32_t)v; return 1;
    case 'L': case 'l': if (!have) return 0; override[OV_LEVEL] = (int32_t)v; return 1;
    case 'S': case 's': if (!have) return 0; override_size = (int32_t)(v > 8u ? 8u : v); return 1;
    case 'B': case 'b': bypass ^= 1u; return 1;
    case 'F': case 'f': freeze_latched ^= 1u; return 1;
    case 'K': case 'k':
        for (i = 0; i < OV_N; i++)
            override[i] = -1;
        override_size = -1;
        freeze_latched = 0;
        return 1;
    }
    return 0;
}

static void app_status(void)
{
    gx_putd("step=", step);
    gx_putd("bypass=", rv_ctl.bypass);
    gx_putd("freeze=", rv_ctl.freeze);
    gx_putd("ov=", (uint32_t)((override[0] >= 0) | (override[1] >= 0) << 1 | (override[2] >= 0) << 2 |
                              (override[3] >= 0) << 3 | (override[4] >= 0) << 4 | (override_size >= 0) << 5));
    gx_putd("reverb_bytes=", rv_bytes);
}
