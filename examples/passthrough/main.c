/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Hilowitz */
/* The smallest Galaxia firmware: the input goes straight to the output.
 *   left footswitch   mute on / off (LED green while the sound passes)
 *   left-bottom knob  output level
 *   no plug in the right input: the left input feeds both outputs */
#define GX_USB_PRODUCT "Galaxia Passthrough"
#include "gx_runtime.c"

static volatile uint32_t level = 1024;          /* Q10: 1024 = unity */
static volatile uint32_t muted;

static void app_init(void) { gx_led(0, 1); }

static void app_audio(const int32_t *in, int32_t *out, uint32_t frames)
{
    uint32_t i, g = muted ? 0u : level, mono = gx_mono_input();
    for (i = 0; i < frames; i++) {
        int32_t l = in[2 * i], r = mono ? l : in[2 * i + 1];
        out[2 * i] = (int32_t)(((int64_t)l * g) >> 10);
        out[2 * i + 1] = (int32_t)(((int64_t)r * g) >> 10);
    }
}

static void app_poll(uint32_t now_ms)
{
    static uint32_t prev;
    uint32_t sw = gx_in.switches;
    (void)now_ms;
    if ((sw & ~prev) & GX_SW_LEFT)
        muted ^= 1u;
    prev = sw;
    level = gx_in.knob[GX_KNOB_LEFT_BOTTOM] >= 1020u ? 1024u : gx_in.knob[GX_KNOB_LEFT_BOTTOM];
    gx_led(0, !muted);
}

static int app_command(const char *line, uint32_t n)
{
    (void)line; (void)n;
    return 0;
}

static void app_status(void)
{
    gx_putd("muted=", muted);
    gx_putd("level_q10=", level);
}
