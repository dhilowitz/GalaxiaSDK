/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 David Hilowitz
 * The boot guard, console and USB poll follow felucca-bsp's examples/hwtest/hwtest.c
 * (MPL-2.0, Leo Kuroshita, Hügelton Instruments). */
/* Galaxia runtime: everything a Galaxia firmware needs that is not the effect itself.
 * An application is one C file that includes this one and defines the hooks below
 * (examples/passthrough/main.c is the smallest).
 *
 * Settings, before the #include (all optional):
 *   GX_USB_PRODUCT "..."    the USB product name (default "Galaxia")
 *   GX_USB_PID 0x....       the USB product id under VID 0x1209 (default 0x0007, a pid.codes TEST id:
 *                           fine on the bench, replace before distributing a firmware)
 *
 * Hooks, defined by the application:
 *   void app_init(void)                    once, before audio starts (main context)
 *   void app_audio(const int32_t *in, int32_t *out, uint32_t frames)
 *                                          audio interrupt, every 128 frames (2.9 ms at 44,118 Hz):
 *                                          interleaved stereo, signed 24-bit in an int32. `in` is the
 *                                          converter's input plus whatever a computer plays over USB;
 *                                          `out` goes to the converter and back to the computer over USB.
 *   void app_poll(uint32_t now_ms)         main loop, every pass (controls, LED, MIDI out)
 *   int app_command(const char *line, uint32_t n)
 *                                          a console line the runtime does not know; 1 = handled
 *   void app_status(void)                  appended to the console's status line (gx_putd etc.)
 *
 * What the runtime provides:
 *   gx_in.knob[GX_KNOBS] (0..1023), gx_in.switches, gx_in.jacks   (boards/galaxia/gx_board.h)
 *   gx_led(red, green)
 *   gx_mono_input()       no plug in the right input: use the left input for both channels
 *   gx_midi_cc(cc, v)     MIDI control change out over USB (channel 1)
 *   gx_puts(s), gx_putd(label, v), gx_puthex(label, v)            console output
 *   gx_ms()               milliseconds since start
 *
 * Built in: USB MIDI (the stock soft key F0 22 24 35 7D F7 enters the bootloader), USB audio both
 * ways, a serial console (h help, p status, c last crash, u bootloader, r reboot; a status line every
 * 0.5 s), the watchdog, the fault report, and the boot guard: two starts in a row that never reach 2 s
 * of running drop into the ROM bootloader, so a broken build can always be replaced over USB. */
#include <stdint.h>
#ifndef GX_USB_PRODUCT
#define GX_USB_PRODUCT "Galaxia"
#endif
#ifndef GX_USB_PID
#define GX_USB_PID 0x0007
#endif
#define FELUCCA_CDC 1
#define FELUCCA_OTA 0
#define FELUCCA_UAC 1
#define FELUCCA_UAC_OUT 1
#define FELUCCA_UAC_48K 0
#define FELUCCA_USB_PID GX_USB_PID
#define FELUCCA_USB_MANUFACTURER "Galaxia SDK (unofficial)"
#define FELUCCA_USB_PRODUCT GX_USB_PRODUCT
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#define HALF_FRAMES 128u                         /* frames per DMA half (usb.c sizes its audio rings from it) */
#define HALF_WORDS (2u * HALF_FRAMES)
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_timer.h"
#include "gx_board.h"
#include "fm1_flash.h"
#include "libc.c"
#include "usb.c"

static void app_init(void);
static void app_audio(const int32_t *in, int32_t *out, uint32_t frames);
static void app_poll(uint32_t now_ms);
static int app_command(const char *line, uint32_t n);
static void app_status(void);

extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _rt_start[], _rt_end[], _rt_load[];

static volatile uint32_t ms;
static uint32_t boots_ok;
static struct { uint32_t magic, pending, failed; } guard __attribute__((section(".noinit")));
static uint32_t gx_ms(void) { return ms; }

/* Boot progress, readable from the ROM bootloader (jl-uboot-tool mem_read) after a failed start:
 * the unused gap between RAMTEXT and RAM, next to crt0's STATUS words at 0x01C07F00. */
#define GX_DBG ((volatile uint32_t *)0x01C07F40u)
enum { DBG_MAGIC, DBG_BOOTS, DBG_STAGE, DBG_MS, DBG_P3RST, DBG_RSTSRC, DBG_AUDIO, DBG_TICKS, DBG_N };
#define DBG_MAGIC_VALUE 0x47584442u                     /* "GXDB" */
static void stage(uint32_t s) { GX_DBG[DBG_STAGE] = s; }

/* ---- audio ---- */
#define BLOCK_TICKS (HALF_FRAMES * 24000000u / GX_AUDIO_RATE)   /* TIMER4 ticks per half */
static int32_t tx_buf[2 * HALF_WORDS] __attribute__((aligned(32)));
static int32_t rx_buf[2 * HALF_WORDS] __attribute__((aligned(32)));
static int32_t work[HALF_WORDS];
static const int32_t *gx_rx_raw;                       /* this block's received words, unconverted (diagnostics) */
static struct { volatile uint32_t blocks, late, sum, count, peak; } load;

static int gx_mono_input(void) { return !(gx_in.jacks & GX_JACK_RIGHT); }

void fm1_alnk0_irq(void)
{
    uint8_t p = gx_audio_pending();
    uint32_t half, i, t0, used;
    const int32_t *in;
    gx_audio_ack(p);
    GX_DBG[DBG_AUDIO]++;
    if (!(p & GX_AUDIO_RX))
        return;
    t0 = fm1_ticks();
    half = gx_audio_free_half();
    in = &rx_buf[half * HALF_WORDS];
    gx_rx_raw = in;
    for (i = 0; i < HALF_WORDS; i++)                    /* signed 24-bit in a 32-bit word */
        work[i] = (int32_t)((uint32_t)in[i] << 8) >> 8;
    uac_render_start();
    uac_play(work, HALF_FRAMES, 4096u);                 /* the computer's audio joins the input */
    app_audio(work, &tx_buf[half * HALF_WORDS], HALF_FRAMES);
    uac_tap(&tx_buf[half * HALF_WORDS], HALF_FRAMES);   /* and the output goes back to it */
    if (gx_audio_free_half() != half)
        load.late++;                                    /* the DMA moved on while we rendered */
    used = fm1_ticks() - t0;
    load.sum += used;
    load.count++;
    if (used > load.peak)
        load.peak = used;
    load.blocks++;
}
extern void isr_alnk0(void);

void fm1_timer5_irq(void)                               /* 10 kHz: switches, knobs, ms, USB at 2 kHz */
{
    static uint32_t sub, last, acc;
    uint32_t t = fm1_ticks();
    fm1_timer5_ack();
    GX_DBG[DBG_TICKS]++;
    gx_scan_tick();
    acc += t - last;
    last = t;
    while (acc >= 1000u * FM1_TICKS_PER_US) {
        acc -= 1000u * FM1_TICKS_PER_US;
        ms++;
        fm1_ms = ms;
    }
    if (++sub == 5u) {
        sub = 0;
        usb_poll();
    }
}
extern void isr_timer5(void);

/* a fault: the LED flashes red, then reset; the record in fm1_crash survives ('c') */
static void fm1_fault(const fm1_crash_t *c)
{
    uint32_t i;
    (void)c;
    gx_audio_stop();
    for (i = 0; i < 6u; i++) {
        gx_led(i & 1u, 0);
        fm1_delay_ms(150);
    }
    fm1_reboot();
}

/* ---- MIDI and console ---- */
static void gx_midi_cc(uint32_t cc, uint32_t v)
{
    (void)midi_out_try(0x0Bu | 0xB0u << 8 | (cc & 0x7Fu) << 16 | (v & 0x7Fu) << 24);
}
static void gx_putc(char c)
{
    if (!cdc.dtr || co_w - co_r >= CO_N)
        return;
    cdc_out[co_w % CO_N] = (uint8_t)c;
    RING_PUBLISH();
    co_w++;
}
static void gx_puts(const char *s) { while (*s) gx_putc(*s++); }
static void gx_putd(const char *k, uint32_t v)
{
    char b[10];
    uint32_t n = 0;
    gx_puts(k);
    do { b[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n)
        gx_putc(b[--n]);
    gx_putc(' ');
}
static void gx_puthex(const char *k, uint32_t v)
{
    uint32_t i;
    gx_puts(k);
    for (i = 8; i--;)
        gx_putc("0123456789ABCDEF"[(v >> (4u * i)) & 15u]);
    gx_putc(' ');
}

static void status(void)
{
    static uint32_t prev_blocks;
    uint32_t f = irq_save(), sum = load.sum, count = load.count, peak = load.peak, blocks = load.blocks, i;
    load.sum = load.count = load.peak = 0;
    irq_restore(f);
    gx_putd("ms=", ms);
    gx_putd("load_avg_pm=", count ? (uint32_t)((uint64_t)sum * 1000u / count / BLOCK_TICKS) : 0u);
    gx_putd("load_peak_pm=", (uint32_t)((uint64_t)peak * 1000u / BLOCK_TICKS));
    gx_putd("blocks/s=", (blocks - prev_blocks) * 1000u / 500u);
    prev_blocks = blocks;
    gx_putd("late=", load.late);
    gx_puts("knobs=");
    for (i = 0; i < GX_KNOBS; i++)
        gx_putd("", gx_in.knob[i]);
    gx_putd("sw=", gx_in.switches);
    gx_putd("jacks=", gx_in.jacks);
    gx_putd("adc_timeouts=", gx_in.adc_timeouts);
    gx_putd("usb_cfg=", usb.config);
    app_status();
    gx_puts("\r\n");
}

static void runtime_command(const char *s, uint32_t n)
{
    switch (s[0]) {
    case 'h':
        gx_puts("h help, p status, c last crash, u bootloader, r reboot\r\n");
        (void)app_command(s, n);                /* the application may add its own help */
        return;
    case 'p': status(); return;
    case 'c':
        if (fm1_crash.magic != FM1_CRASH_MAGIC) {
            gx_puts("no crash on record\r\n");
            return;
        }
        gx_putd("count=", fm1_crash.count); gx_putd("vec=", fm1_crash.vec); gx_putd("up_ms=", fm1_crash.uptime_ms);
        gx_puthex("pc=", fm1_crash.pc); gx_puthex("rets=", fm1_crash.rets); gx_puthex("sp=", fm1_crash.sp);
        gx_puts("\r\n");
        return;
    case 'u':
        gx_puts("bootloader\r\n");
        fm1_delay_ms(30);
        usb_detach();
        fm1_enter_uboot();
        return;
    case 'r':
        gx_puts("reboot\r\n");
        fm1_delay_ms(30);
        usb_detach();
        fm1_reboot();
        return;
    }
    if (app_command(s, n))
        gx_puts("ok\r\n");
}

/* Lines end with Enter; h p c u r on their own act at once. */
static void console(void)
{
    static char line[24];
    static uint32_t n;
    while (ci_r != ci_w) {
        char c = (char)cdc_in[ci_r % CI_N];
        ci_r++;
        if (c == '\r' || c == '\n') {
            if (n)
                runtime_command(line, n);
            n = 0;
        } else if (n == 0 && (c == 'h' || c == 'p' || c == 'c' || c == 'u' || c == 'r')) {
            line[0] = c;
            runtime_command(line, 1);
        } else if (n < sizeof line) {
            line[n++] = c;
        }
    }
}

static void app_main(void)
{
    uint32_t last_status = 0;
    stage(20);
    gx_board_init();
    stage(21);
    app_init();
    usb_start();
    stage(22);
    fm1_timer5_start(isr_timer5, 4);
    stage(23);
    gx_audio_init(tx_buf, rx_buf, HALF_WORDS, isr_alnk0, 5);
    stage(24);
    fm1_guard_lock_top();
    stage(25);
    fm1_irq_enable_all();
    stage(26);
    for (;;) {
        uint32_t now = ms;
        GX_DBG[DBG_MS] = now;
        fm1_wdt_feed();
        usb_retry(now);
        if (usb.uboot_req)                      /* the SysEx soft key */
            runtime_command("u", 1);
        if (!boots_ok && now > 2000u) {         /* it runs: the next boot is not a loop */
            boots_ok = 1;
            guard.pending = 0;
            guard.failed = 0;
            stage(30);
        }
        app_poll(now);
        console();
        if (now - last_status >= 500u) {
            last_status = now;
            status();
        }
    }
}

void fm1_cstart(void)
{
    uint32_t *s, *d, i;
    if (GX_DBG[DBG_MAGIC] != DBG_MAGIC_VALUE) {
        for (i = 0; i < DBG_N; i++)
            GX_DBG[i] = 0;
        GX_DBG[DBG_MAGIC] = DBG_MAGIC_VALUE;
    }
    GX_DBG[DBG_BOOTS]++;
    GX_DBG[DBG_AUDIO] = GX_DBG[DBG_TICKS] = 0;
    stage(1);
    fm1_time_init();
    stage(2);
    fm1_reset_reason();
    GX_DBG[DBG_P3RST] = fm1_boot.p3_rst;
    GX_DBG[DBG_RSTSRC] = fm1_boot.rst_src;
    stage(3);
    fm1_wdt_arm(0x0D);
    stage(4);
    if (guard.magic != 0x47584231u) {           /* "GXB1" */
        guard.magic = 0x47584231u;
        guard.failed = 0;
        guard.pending = 0;
    }
    if (guard.pending)
        guard.failed++;
    guard.pending = 1;
    if (guard.failed >= 2u) {
        guard.failed = 0;
        guard.pending = 0;
        fm1_enter_uboot();
    }
    stage(5);
    fm1_irq_init();
    stage(6);
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;
    stage(7);
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    stage(8);
    app_main();
    for (;;)
        ;
}
