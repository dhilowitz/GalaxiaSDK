/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Hilowitz */
/* The second core (CPU1) on the Galaxia: a bench test. The input passes straight through; meanwhile a chain of
 * biquad filters runs on each channel (their output is discarded) as a stand-in for an effect's work.
 * Console (then Enter):
 *   L<n>   n biquads a channel, 0-64 (default 16)
 *   S      split on / off: CPU1 runs the right channel's chain while CPU0 runs the left's
 *   V      verify on / off: with split on, CPU0 also runs the right channel from a copy of the same state and
 *          compares the two results bit for bit (no speed-up: it is a test of CPU1's reads)
 * The status line: n, split, verify, mismatches (verify), cpu1_fallbacks (jobs CPU0 did itself), the
 * runtime's load_avg_pm and cpu1_* counters. */
#define GX_USB_PRODUCT "Galaxia CPU1 Bench"
#define GX_CPU1 1
#include "gx_runtime.c"

#define NMAX 64u
typedef struct { float z1, z2; } bq_state;
typedef struct { bq_state s[NMAX]; } chain;
static chain ch[2], check_copy;                 /* left, right; the right's copy for verify */
static float work_r[HALF_FRAMES], check_r[HALF_FRAMES];
static volatile float sink;                     /* keeps the chains' results from being optimised away */
static volatile uint32_t nbq = 16, split, verify, mismatches, fallbacks;

/* a gentle low-pass (about 10 kHz, Q 0.7 at 44.1 kHz), transposed direct form II */
#define B0 0.2929f
#define B1 0.5858f
#define B2 0.2929f
#define A1 (-0.0000f)
#define A2 0.1716f

typedef struct { chain *c; const int32_t *in; float *out; uint32_t frames, n, chan; } job_t;
static job_t job_r;

static void run_chain(const job_t *j)
{
    uint32_t i, k;
    for (i = 0; i < j->frames; i++) {
        float x = (float)j->in[2u * i + j->chan] * (1.0f / 8388608.0f);
        for (k = 0; k < j->n; k++) {
            bq_state *s = &j->c->s[k];
            float y = B0 * x + s->z1;
            s->z1 = B1 * x - A1 * y + s->z2;
            s->z2 = B2 * x - A2 * y;
            x = y;
        }
        j->out[i] = x;
    }
}
static void cpu1_job(uint32_t arg) { run_chain((const job_t *)(uintptr_t)arg); }   /* on CPU1, from the flash */

static void app_init(void) { gx_led(0, 1); }

static void app_audio(const int32_t *in, int32_t *out, uint32_t frames)
{
    static float work_l[HALF_FRAMES];
    uint32_t i, n = nbq, handed = 0;
    job_t jl = {&ch[0], in, work_l, frames, n, 0};
    job_r = (job_t){&ch[1], in, work_r, frames, n, 1};
    if (split) {
        if (verify)
            check_copy = ch[1];                 /* the right chain's state as CPU1 starts from it */
        fm1_cpu1_sync();
        handed = (uint32_t)fm1_cpu1_run(cpu1_job, (uint32_t)(uintptr_t)&job_r);
    }
    run_chain(&jl);
    if (handed) {
        if (fm1_cpu1_wait() != 0) {             /* retired: the job was dropped, do it here */
            run_chain(&job_r);
            fallbacks++;
        }
        fm1_cpu1_sync();
        if (verify) {
            job_t jc = {&check_copy, in, check_r, frames, n, 1};
            run_chain(&jc);
            for (i = 0; i < frames; i++)
                if (*(const uint32_t *)&check_r[i] != *(const uint32_t *)&work_r[i])
                    mismatches++;
            for (i = 0; i < n; i++)
                if (check_copy.s[i].z1 != ch[1].s[i].z1 || check_copy.s[i].z2 != ch[1].s[i].z2)
                    mismatches++;
        }
    } else {
        if (split)
            fallbacks++;
        run_chain(&job_r);
    }
    sink = work_l[frames - 1u] + work_r[frames - 1u];
    for (i = 0; i < 2u * frames; i++)
        out[i] = in[i];
}

static void app_poll(uint32_t now_ms) { (void)now_ms; }

static int app_command(const char *line, uint32_t n)
{
    if (n >= 1 && line[0] == 'S') { split ^= 1u; return 1; }
    if (n >= 1 && line[0] == 'V') { verify ^= 1u; return 1; }
    if (n >= 2 && line[0] == 'L') {
        uint32_t v = 0, i;
        for (i = 1; i < n && line[i] >= '0' && line[i] <= '9'; i++)
            v = v * 10u + (uint32_t)(line[i] - '0');
        nbq = v > NMAX ? NMAX : v;
        return 1;
    }
    return 0;
}

static void app_status(void)
{
    gx_putd("n=", nbq);
    gx_putd("split=", split);
    gx_putd("verify=", verify);
    gx_putd("mismatches=", mismatches);
    gx_putd("cpu1_fallbacks=", fallbacks);
}
