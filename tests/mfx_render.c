/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Hilowitz */
/* examples/multifx on a computer: each effect fed plucked-string notes (Karplus-Strong), as on the pedal
 * (128-frame blocks, signed 24-bit samples, the arena cleared between effects as the main loop does).
 *   cc -O2 -DMFX_DESKTOP -Iexamples/multifx tests/mfx_render.c examples/multifx/fx_*.c -o mfx_render
 *   ./mfx_render OUTDIR        one WAV per effect at demo settings, and a check of every effect with all
 *                              its knobs at 0, at 1 and in between (finite, below clipping, not silent) */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fx_multi.h"

#define SR 44118
#define BLOCK 128

static const uint16_t demo[MFX_COUNT][5] = {   /* main, a, b, c, mix */
    {460, 460, 610, 200, 410}, {510, 560, 410, 510, 460}, {460, 510, 610, 1023, 460},
    {510, 300, 820, 820, 512}, {410, 250, 820, 770, 512}, {360, 720, 510, 410, 512},
    {560, 820, 300, 0, 1023},  {610, 100, 410, 510, 410}, {610, 510, 610, 410, 1023},
};

static float *pluck(uint32_t n)                 /* a few notes, then silence: what a tail does */
{
    static const float hz[] = {196.0f, 246.9f, 293.7f, 392.0f, 329.6f, 246.9f};
    float *x = calloc(n, sizeof *x), buf[1024];
    uint32_t note, i, len, at;
    for (note = 0; note < 6; note++) {
        at = note * (SR / 2);
        len = (uint32_t)(SR / hz[note]);
        for (i = 0; i < len; i++)
            buf[i] = (float)rand() / RAND_MAX * 2.0f - 1.0f;
        for (i = 0; i < (uint32_t)SR * 2 && at + i < n; i++) {
            float y = buf[i % len], z = buf[(i + 1) % len];
            buf[i % len] = 0.996f * 0.5f * (y + z);
            x[at + i] += 0.35f * y;
        }
    }
    return x;
}

static void run(uint32_t fx, const uint16_t *k, const float *x, uint32_t n, float *out, float *peak, float *rms_tail)
{
    int32_t in[2 * BLOCK], o[2 * BLOCK];
    uint32_t at, i;
    double tail = 0.0;
    mfx_ctl.effect = (uint8_t)fx;
    mfx_ctl.knob[0] = k[0]; mfx_ctl.knob[1] = k[1]; mfx_ctl.knob[2] = k[2]; mfx_ctl.knob[3] = k[3];
    mfx_ctl.mix = k[4];
    *peak = 0.0f;
    for (at = 0; at + BLOCK <= n; at += BLOCK) {
        if (mfx_clear_pending) {                /* the main loop's part */
            memset(mfx_arena, 0, sizeof mfx_arena);
            mfx_cleared();
        }
        for (i = 0; i < BLOCK; i++)
            in[2 * i] = in[2 * i + 1] = (int32_t)(x[at + i] * 8388607.0f);
        mfx_process(in, o, BLOCK, 0);
        for (i = 0; i < 2 * BLOCK; i++) {
            float v = (float)o[i] / 8388608.0f;
            if (!isfinite(v))
                v = 9.0f;
            if (fabsf(v) > *peak)
                *peak = fabsf(v);
            if (out)
                out[2 * at + i] = v;
            if (at > n - SR)                    /* the last second */
                tail += (double)v * v;
        }
    }
    *rms_tail = (float)sqrt(tail / (2.0 * SR));
}

static void wav(const char *path, const float *s, uint32_t frames)
{
    FILE *f = fopen(path, "wb");
    uint32_t i, bytes = frames * 4u, v;
    uint16_t w;
    fwrite("RIFF", 1, 4, f); v = 36 + bytes; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f); w = 1; fwrite(&w, 2, 1, f); w = 2; fwrite(&w, 2, 1, f);
    v = 44100; fwrite(&v, 4, 1, f); v = 44100 * 4; fwrite(&v, 4, 1, f); w = 4; fwrite(&w, 2, 1, f);
    w = 16; fwrite(&w, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
    for (i = 0; i < 2 * frames; i++) {
        int16_t q = (int16_t)lrintf(fmaxf(-1.0f, fminf(1.0f, s[i])) * 32767.0f);
        fwrite(&q, 2, 1, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const uint32_t n = SR * 7;
    float *x = pluck(n), *out = calloc(2 * n, sizeof *out), peak, tail;
    uint32_t fx, t, bad = 0;
    char path[512];
    static const uint16_t corners[3][5] = {{0, 0, 0, 0, 1023}, {1023, 1023, 1023, 1023, 1023}, {700, 300, 900, 100, 700}};
    for (fx = 0; fx < MFX_COUNT; fx++) {
        for (t = 0; t < 3; t++) {               /* the switch, and the effect at its extremes */
            run(fx, corners[t], x, n, NULL, &peak, &tail);
            printf("%-16s knobs %-6s peak %5.2f  last second rms %.4f%s\n", mfx_effects[fx]->name,
                   t == 0 ? "0" : t == 1 ? "1" : "mixed", peak, tail, peak > 1.5f ? "  BAD" : "");
            bad += peak > 1.5f;
        }
        run(fx, demo[fx], x, n, out, &peak, &tail);
        printf("%-16s demo         peak %5.2f\n", mfx_effects[fx]->name, peak);
        if (argc > 1) {
            snprintf(path, sizeof path, "%s/%u-%s.wav", argv[1], fx + 1, mfx_effects[fx]->name);
            for (char *c = path + strlen(argv[1]) + 1; *c; c++)
                if (*c == ' ' || *c == '/')
                    *c = '-';
            wav(path, out, n);
        }
    }
    if (argc > 1) {                             /* the dry input too, for comparison */
        for (t = 0; t < n; t++)
            out[2 * t] = out[2 * t + 1] = x[t];
        snprintf(path, sizeof path, "%s/0-dry.wav", argv[1]);
        wav(path, out, n);
    }
    return bad != 0;
}
