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
#define GX_SDK_VERSION "0.1.0"                  /* CHANGELOG.md */
#ifndef GX_USB_PRODUCT
#define GX_USB_PRODUCT "Galaxia"
#endif
#ifndef GX_USB_PID
#define GX_USB_PID 0x0007
#endif
/* Updates over USB MIDI (M-UPGRADE, the protocol of M-VAVE's own updater; tools/gx_update.py): on unless
 * GX_MIDI_UPDATE is 0. GX_UPDATE_ID is the identity the firmware answers with, and its packages' (gx_pkg.py
 * reads it from the app): "SDK-Galaxia_<number>". The stock firmware is "Galaxia_002", a different model, so
 * the updater never mistakes one for the other (and the package format cannot spell "Galaxia" alone). */
#ifndef GX_MIDI_UPDATE
#define GX_MIDI_UPDATE 1
#endif
#ifndef GX_UPDATE_ID
#define GX_UPDATE_ID "SDK-Galaxia_900"
#endif
#define FELUCCA_CDC 1
#define FELUCCA_OTA GX_MIDI_UPDATE
#define FELUCCA_ID GX_UPDATE_ID
/* the update record in RAM only. A flash copy, which felucca-bsp writes so that an update cut off by a power
 * loss resumes, did nothing on a Galaxia: its boot loader ignored one at 0xE4F00 and at 0x7AF00 after a power
 * cycle (2026-10-31; its scan seems to run only when something else failed). So a power cycle before the
 * loader starts writing returns to this firmware; one during the write leaves a half-written app. */
#ifndef FELUCCA_OTA_RAMONLY
#define FELUCCA_OTA_RAMONLY 1
#endif
#define FELUCCA_UAC 1
#define FELUCCA_UAC_OUT 1
#define FELUCCA_UAC_48K 0
/* the audio streaming interfaces before the MIDI one in the audio function: macOS up to 15 shows no audio
 * device otherwise (its USB audio driver stops parsing after the MIDI interface; felucca-bsp's usb.c) */
#define FELUCCA_UAC_AS_FIRST 1
/* and the audio streaming interfaces right after audio control, MIDI last: macOS 13-15 attach their USB audio
 * driver only then (felucca-bsp's usb.c, Felucca #67); 1 keeps MIDI in the collection, which Linux needs */
#define FELUCCA_UAC_MIDI_LAST 1
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
/* Saved settings (gx_settings_save): two 4 KiB sectors in the firmware data area above the update staging,
 * blank on a Galaxia (outside what M-VAVE's updater and the update loader write) */
#define GX_SET_LO 0x000F0000u
#define GX_SET_HI 0x000F2000u
/* the runtime writes the update staging and its saved settings only */
#define FL_RANGE_OK(off, n) (FL_IN(off, n, FL_OTA_LO, FL_OTA_HI) || FL_IN(off, n, GX_SET_LO, GX_SET_HI))
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
static volatile uint32_t gx_updating;           /* an update session runs: silence, the LED shows it */
static uint32_t gx_update_dry;                  /* console 'd': the next update stops before the restart */
/* The boot guard's state survives resets but not power: after power-up it is random. volatile, so the
 * compiler keeps the full magic word (it once shrank it to a byte flag that random RAM passed). */
static volatile struct { uint32_t magic, pending, failed; } guard __attribute__((section(".noinit")));
static uint32_t gx_ms(void) { return ms; }

/* Boot progress, readable after a failed start through the USB loader gx_flash.py uploads: in the
 * no-init area, which neither that loader (it overwrites RAM up to about 0x01C08000) nor a reset clears. */
#define GX_DBG ((volatile uint32_t *)0x01C7D000u)
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
#if FELUCCA_UAC_OUT
    uac_play(work, HALF_FRAMES, 4096u);                 /* the computer's audio joins the input */
#endif
    if (gx_updating)
        for (i = 0; i < HALF_WORDS; i++)
            tx_buf[half * HALF_WORDS + i] = 0;
    else
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

/* saved settings (gx_settings_save, below) */
#define GX_SET_SLOT 32u
#define GX_SET_SLOTS 120u                       /* 0x000..0xEFF of each sector */
#define GX_SET_MAX 26u                          /* bytes a firmware may save */
static struct {
    uint8_t data[GX_SET_MAX], pending_data[GX_SET_MAX];
    uint8_t slot[GX_SET_SLOT];                   /* the slot being written */
    uint32_t len, pending_len, seq, have, sector, next, other_blank, pending, since, saves, last_us, failed;
    uint32_t writing, steps, blocks, max_us;    /* a write in progress: its steps, the last audio block seen */
} gx_set;

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
    if (uac.alt) {                              /* the USB audio input while a computer records */
        gx_putd("uac_pkts=", uac.pkts);
        gx_putd("uac_missed=", uac.missed);     /* the host asked, no packet was ready */
        gx_putd("uac_under=", uac.underruns);   /* the ring ran dry (frames repeated) */
        gx_putd("uac_over=", uac.overruns);     /* the ring overflowed (frames dropped) */
        gx_putd("uac_stalls=", uac.stalls);
    }
    gx_putd("set_saves=", gx_set.saves);        /* saved settings: writes since power-up, the last one's time */
    gx_putd("set_us=", gx_set.last_us);          /* the longest stall of the last write */
    gx_putd("set_steps=", gx_set.steps);
    gx_putd("set_failed=", gx_set.failed);
    app_status();
    gx_puts("\r\n");
}

#if GX_MIDI_UPDATE
/* ---- updates over USB MIDI: the hooks bsp/chip/ota.c takes (usb.c has the SysEx frame ones). The firmware
 * checks the package, stages its update loader in flash 0xE0000..0xE4FFF, and on the host's go restarts into
 * it through an update record; the loader then writes the app area. ---- */
static uint32_t ota_now_ms(void) { return ms; }
static void ota_idle(void)
{
    fm1_wdt_feed();
    usb_retry(ms);
}
static int ota_erase(uint32_t off)
{
    uint32_t took;
    if (!FL_RANGE_OK(off, 0x1000u) || (off & 0xFFFu))
        return -8;
    return fl_erase4k(off, &took);
}
static int ota_prog(uint32_t off, const void *p, uint32_t n)
{
    if (!FL_RANGE_OK(off, n))
        return -8;
    return fl_write(off, (const uint8_t *)p, n);
}
static int ota_fread(uint32_t off, void *p, uint32_t n)
{
    uint32_t f = irq_save();
    int rc = FL_FAR(fl_read_ram)(off, (uint8_t *)p, n);
    irq_restore(f);
    return rc;
}
static void ota_show(uint32_t step, int32_t code)   /* amber while it runs; red blinks: it stopped, nothing changed */
{
    uint32_t i;
    (void)code;
    if (step < 9u) {
        gx_led(1, 1);
        return;
    }
    for (i = 0; i < 6u; i++) {
        gx_led(!(i & 1u), 0);
        fm1_delay_ms(150);
        fm1_wdt_feed();
    }
}
static void ota_commit(const uint8_t *parm)
{
    if (gx_update_dry) {                        /* checked and staged; update_session() erases the staging */
        gx_puts("update dry run: the package checked and the loader staged; not restarting\r\n");
        return;
    }
    guard.pending = 0;                          /* a restart on purpose, not a failed start */
    usb_detach();
    fm1_delay_ms(30);
    fm1_enter_update(parm);                     /* the record into RAM, core reset: the boot loader runs ours */
}
#include "ota.c"

static void update_session(void)                /* returns only if nothing was committed */
{
    uint32_t i;
    gx_updating = 1;
    for (i = 0; i < 2u * HALF_WORDS; i++)
        tx_buf[i] = 0;
    ota_session();
    if (gx_update_dry) {
        ota_unstage();
        gx_update_dry = 0;
    }
    gx_updating = 0;
}
#endif

static void runtime_command(const char *s, uint32_t n)
{
    switch (s[0]) {
    case 'h':
        gx_puts("Galaxia SDK " GX_SDK_VERSION ": h help, p status, c last crash, u bootloader, r reboot"
#if GX_MIDI_UPDATE
                ", d (then Enter) next MIDI update a dry run"
#endif
                "\r\n");
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
#if GX_MIDI_UPDATE
    case 'd':
        if (n == 1u) {
            gx_update_dry = 1;
            gx_puts("the next update over MIDI is a dry run (checks, stages, erases; no restart)\r\n");
            return;
        }
        break;
#endif
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

/* ---- saved settings: a few bytes a firmware keeps across power-off ----
 * gx_settings_load(dst, n) at app_init gives back the last n bytes saved (1), or nothing (0: first boot, or the
 * size changed). gx_settings_save(src, n) queues them; the runtime writes them a second after the last change,
 * right after an audio block, so the write (about a millisecond with interrupts off) costs no audio.
 * Each save appends one 32-byte slot to a sector; erasing (tens of ms, everything stalls) happens only at
 * power-up, before the audio starts, which keeps the other sector blank for when this one fills (120 slots).
 * Slots avoid each sector's last 256 bytes, where the update loader looks for an update record. */

/* One step of a settings write, interrupts off, from RAM: start the page program (start) or resume it, let it
 * run for up to budget_us, then suspend it (75h; resume 7Ah, the Puya P25Q80 and most NOR parts) and give the
 * flash back to XIP. 0: written; 1: suspended, call again after the next audio block; < 0: error. A part that
 * does not suspend just finishes the program here, as one longer stall. */
static RAMFN int gx_set_step_ram(uint32_t off, const uint8_t *src, uint32_t n, uint32_t start, uint32_t budget_us)
{
    fl_saved_t s;
    uint32_t t0, i, took;
    int rc = 0;
    if (!FL_RANGE_OK(off, n) || n == 0 || n > 256u || ((off & 0xFFu) + n) > 256u)
        return -1;
    if (fl_enter(&s))
        return -2;
    if (start) {
        fl_wren();
        if (!(fl_rdsr(0x05) & 2u)) {
            fl_exit(&s);
            return -3;
        }
        fl_cs(0); fl_tx(0x02); fl_addr(off);
        for (i = 0; i < n; i++) fl_tx(src[i]);
        fl_cs(1);
    } else {
        fl_cs(0); fl_tx(0x7A); fl_cs(1);          /* resume */
    }
    t0 = T4_CNT;
    while (fl_rdsr(0x05) & 1u)
        if ((T4_CNT - t0) / 24u > budget_us)
            break;
    if (fl_rdsr(0x05) & 1u) {                     /* still programming: suspend */
        fl_cs(0); fl_tx(0x75); fl_cs(1);
        t0 = T4_CNT;
        while ((fl_rdsr(0x05) & 1u) && (T4_CNT - t0) / 24u < 200u)
            ;
        if (fl_rdsr(0x05) & 1u)                   /* it does not suspend: finish here */
            rc = fl_wait_ready(10000u, &took);
        else if (fl_rdsr(0x35) & 0x80u) {         /* SUS: suspended, readable again */
            fl_exit(&s);
            return 1;
        }                                         /* else it finished meanwhile */
    }
    fl_inval(off, n);
    fl_exit(&s);
    return rc;
}

static uint16_t gx_set_crc(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFu, i;
    while (n--) {
        c ^= (uint32_t)*p++ << 8;
        for (i = 0; i < 8u; i++)
            c = c & 0x8000u ? (c << 1) ^ 0x1021u : c << 1;
    }
    return (uint16_t)c;
}

static uint32_t gx_set_addr(uint32_t sector, uint32_t slot) { return GX_SET_LO + sector * 0x1000u + slot * GX_SET_SLOT; }

static int gx_set_read(uint32_t off, uint8_t *p, uint32_t n)
{
    uint32_t f = irq_save();
    int rc = FL_FAR(fl_read_ram)(off, p, n);
    irq_restore(f);
    return rc;
}

/* at power-up, interrupts still off: find the newest slot, and make sure the other sector is blank */
static void gx_settings_boot(void)
{
    static uint8_t b[GX_SET_SLOT];
    uint32_t sec, slot, i, used[2] = {0, 0};
    for (sec = 0; sec < 2u; sec++)
        for (slot = 0; slot < GX_SET_SLOTS; slot++) {
            uint32_t blank = 1;
            gx_set_read(gx_set_addr(sec, slot), b, GX_SET_SLOT);
            for (i = 0; i < GX_SET_SLOT; i++)
                if (b[i] != 0xFFu)
                    blank = 0;
            if (blank)
                continue;
            used[sec] = slot + 1u;               /* the next free slot follows the last one written */
            if (b[0] == 0x5Au && b[1] >= 1u && b[1] <= GX_SET_MAX &&
                gx_set_crc(b, GX_SET_SLOT - 2u) == (uint16_t)(b[30] | b[31] << 8)) {
                uint32_t seq = (uint32_t)(b[2] | b[3] << 8);
                if (!gx_set.have || (int16_t)(uint16_t)(seq - gx_set.seq) > 0) {
                    gx_set.have = 1;
                    gx_set.seq = seq;
                    gx_set.sector = sec;
                    gx_set.len = b[1];
                    memcpy(gx_set.data, &b[4], b[1]);
                }
            }
        }
    gx_set.next = used[gx_set.sector];
    if (used[gx_set.sector ^ 1u]) {             /* the other sector: blank it now, while erasing is free */
        uint32_t took;
        fm1_wdt_feed();
        if (fl_erase4k(gx_set_addr(gx_set.sector ^ 1u, 0), &took))
            gx_set.failed++;
        fm1_wdt_feed();
    }
    gx_set.other_blank = 1;
}

static __attribute__((unused)) int gx_settings_load(void *dst, uint32_t n)
{
    if (!gx_set.have || gx_set.len != n)
        return 0;
    memcpy(dst, gx_set.data, n);
    return 1;
}

static __attribute__((unused)) void gx_settings_save(const void *src, uint32_t n)
{
    if (n < 1u || n > GX_SET_MAX)
        return;
    if (gx_set.pending ? gx_set.pending_len == n && !memcmp(gx_set.pending_data, src, n)
                       : gx_set.have && gx_set.len == n && !memcmp(gx_set.data, src, n))
        return;                                 /* nothing new */
    memcpy(gx_set.pending_data, src, n);
    gx_set.pending_len = n;
    gx_set.pending = 1;
    gx_set.since = ms;
}

static void gx_settings_service(uint32_t now)
{
    uint32_t i, f, t0, dt;
    uint16_t crc;
    int rc;
    if (gx_set.writing) {                       /* a step after each audio block, while the most time is free */
        if (load.blocks == gx_set.blocks)
            return;
        gx_set.blocks = load.blocks;
        t0 = fm1_ticks();
        f = irq_save();
        rc = FL_FAR(gx_set_step_ram)(gx_set_addr(gx_set.sector, gx_set.next), gx_set.slot, GX_SET_SLOT, 0, 250u);
        irq_restore(f);
        dt = (fm1_ticks() - t0) / 24u;
        if (dt > gx_set.max_us)
            gx_set.max_us = dt;
        gx_set.steps++;
        if (rc == 1 && gx_set.steps < 200u)
            return;
        gx_set.writing = 0;
        gx_set.next++;
        gx_set.last_us = gx_set.max_us;
        if (rc) {
            gx_set.failed++;
            return;
        }
        gx_set.saves++;
        return;
    }
    if (!gx_set.pending || now - gx_set.since < 1000u || gx_updating)
        return;
    if (gx_set.next >= GX_SET_SLOTS) {          /* this sector is full: on to the blank one */
        if (!gx_set.other_blank) {
            gx_set.pending = 0;                 /* both full (240 saves since power-up): until the next */
            gx_set.failed++;
            return;
        }
        gx_set.sector ^= 1u;
        gx_set.next = 0;
        gx_set.other_blank = 0;
    }
    memset(gx_set.slot, 0xFF, sizeof gx_set.slot);
    gx_set.seq = (gx_set.seq + 1u) & 0xFFFFu;
    gx_set.slot[0] = 0x5Au;
    gx_set.slot[1] = (uint8_t)gx_set.pending_len;
    gx_set.slot[2] = (uint8_t)gx_set.seq;
    gx_set.slot[3] = (uint8_t)(gx_set.seq >> 8);
    for (i = 0; i < gx_set.pending_len; i++)
        gx_set.slot[4 + i] = gx_set.pending_data[i];
    crc = gx_set_crc(gx_set.slot, GX_SET_SLOT - 2u);
    gx_set.slot[30] = (uint8_t)crc;
    gx_set.slot[31] = (uint8_t)(crc >> 8);
    gx_set.pending = 0;
    gx_set.have = 1;                            /* what is saved, as soon as it is on its way (later calls with */
    gx_set.len = gx_set.slot[1];                /* the same bytes add nothing) */
    memcpy(gx_set.data, &gx_set.slot[4], gx_set.len);
    i = load.blocks;                            /* start just after an audio block */
    t0 = fm1_ticks();
    while (load.blocks == i && fm1_ticks() - t0 < 24000u * 10u)
        ;
    gx_set.blocks = load.blocks;
    gx_set.steps = 1;
    gx_set.max_us = 0;
    t0 = fm1_ticks();
    f = irq_save();
    rc = FL_FAR(gx_set_step_ram)(gx_set_addr(gx_set.sector, gx_set.next), gx_set.slot, GX_SET_SLOT, 1, 250u);
    irq_restore(f);
    gx_set.max_us = (fm1_ticks() - t0) / 24u;
    gx_set.writing = 1;
    if (rc != 1) {                              /* done (or failed) in one step */
        gx_set.blocks = load.blocks - 1u;       /* finish on the next call */
        gx_set.writing = 0;
        gx_set.next++;
        gx_set.last_us = gx_set.max_us;
        if (rc) {
            gx_set.failed++;
            return;
        }
        gx_set.saves++;
    }
}

static void app_main(void)
{
    uint32_t last_status = 0;
    stage(20);
    gx_board_init();
    gx_settings_boot();
    stage(21);
    app_init();
    usb_start();
    stage(22);
    fm1_timer5_start(isr_timer5, 4);
    stage(23);
    gx_audio_init(tx_buf, rx_buf, HALF_WORDS, isr_alnk0, 3);   /* below TIMER5 (4): its USB service nests in
                                                                * the render, or a long effect starves the USB
                                                                * audio input of packets (as Felucca does) */
    stage(24);
    fm1_guard_lock_top();
    stage(25);
    fm1_irq_enable_all();
    stage(26);
#if GX_MIDI_UPDATE
    ota_boot_cleanup();                         /* we run: an update finished or never started */
#endif
    for (;;) {
        uint32_t now = ms;
        GX_DBG[DBG_MS] = now;
        fm1_wdt_feed();
        usb_retry(now);
        if (usb.uboot_req)                      /* the SysEx soft key */
            runtime_command("u", 1);
#if GX_MIDI_UPDATE
        ota_service();                          /* the updater's handshake */
        if (usb.ota_req) {                      /* and its upgrade command */
            usb.ota_req = 0;
            update_session();
        }
#endif
        if (!boots_ok && now > 2000u) {         /* it runs: the next boot is not a loop */
            boots_ok = 1;
            guard.pending = 0;
            guard.failed = 0;
            stage(30);
        }
        app_poll(now);
        gx_settings_service(now);
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
    /* a power-on reset (P33 reset source bit 0, or bit 1: VDDIO came up) starts the count afresh */
    if (guard.magic != 0x47584231u || (fm1_boot.p3_rst & 3u) || guard.failed > 2u) {   /* "GXB1" */
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
