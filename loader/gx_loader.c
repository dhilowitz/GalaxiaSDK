/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Copyright (C) 2026 David Hilowitz */
/* The Galaxia's update loader: felucca-bsp's loader/loader.c with the Galaxia's LED in place of the FM-1's
 * screen, and the Galaxia's names. The boot loader starts it from RAM (0x01C0A800) through the update record
 * an SDK firmware leaves when an update begins (runtime/gx_runtime.c, bsp/chip/ota.c); interrupts stay off and
 * USB is polled. It is a USB-MIDI device "Galaxia Update" that answers the update protocol as
 * "ota-SDK-Galaxia_900" (hosts look for "ota-"), reads the whole package, then writes the app area only
 * (bsp/loader/ldr_core.c: [0x4000, 0x93000); on a Galaxia everything above 0x7C000 there is unused and blank).
 * The soft key (F0 22 24 35 7D F7) still reaches the chip's USB bootloader from here.
 * LED: red while it runs, green flickering as it writes. Built by build.sh into build/loader/ota.bin. */
#include <stdint.h>
#ifndef GX_USB_PID
#define GX_USB_PID 0x0007
#endif
#define FELUCCA_LOADER 1
#define FELUCCA_CDC 0
#define FELUCCA_OTA 1
#define FELUCCA_OTA_DRYRUN 0
#define FELUCCA_USB_PID GX_USB_PID
#define FELUCCA_USB_MANUFACTURER "Galaxia SDK (unofficial)"
#define FELUCCA_USB_PRODUCT "Galaxia Update"
#define FELUCCA_ID "ota-SDK-Galaxia_900"
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_gpio.h"
/* the app area and update records (ldr_core.c); never the boot head */
#define FL_RANGE_OK(off, n) (FL_IN(off, n, 0x4000u, 0x97000u) || FL_IN(off, n, 0xE0000u, 0xFC000u))
#include "fm1_flash.h"
#include "libc.c"
#include "usb.c"

#define LED_RED (1u << 2)                           /* PA2, PA3: active low (boards/galaxia/gx_board.h) */
#define LED_GREEN (1u << 3)

static void led(uint32_t on)
{
    FM1_PR(FM1_PA, FM1_OUT) = (FM1_PR(FM1_PA, FM1_OUT) | LED_RED | LED_GREEN) & ~on;
}

static uint32_t ldr_ms(void)   /* monotonic ms; survives the TIMER4 wrap (178.9 s) */
{
    static uint32_t last, acc, ms;
    uint32_t now = fm1_ticks();
    acc += now - last;
    last = now;
    ms += acc / (1000u * FM1_TICKS_PER_US);
    acc %= 1000u * FM1_TICKS_PER_US;
    return ms;
}

static void ldr_poll(void)                          /* USB at ~2 kHz, watchdog */
{
    static uint32_t last;
    uint32_t now = fm1_ticks();
    fm1_wdt_feed();
    if (now - last >= 500u * FM1_TICKS_PER_US) {
        last = now;
        usb_poll();
    }
}

/* ---- ota.c hooks (the SysEx frame hooks are in usb.c) ---- */
static uint32_t ota_now_ms(void) { return ldr_ms(); }
static void ota_idle(void) { ldr_poll(); }
/* step-1 hooks of ota.c: not used by the loader */
static int ota_erase(uint32_t off) { (void)off; return -1; }
static int ota_prog(uint32_t off, const void *p, uint32_t n) { (void)off; (void)p; (void)n; return -1; }
static int ota_fread(uint32_t off, void *p, uint32_t n) { return fl_read_ram(off, p, n); }
static void ota_show(uint32_t step, int32_t code) { (void)step; (void)code; }
static void ota_commit(const uint8_t *parm) { (void)parm; }
#include "ota.c"

/* ---- ldr_core.c hooks: interrupts are off for the whole loader ---- */
static int ldr_fread(uint32_t off, void *p, uint32_t n) { return fl_read_ram(off, p, n); }
static int ldr_erase(uint32_t off)
{
    uint32_t took;
    return fl_erase4k_ram(off, &took);
}
static int ldr_prog(uint32_t off, const void *p, uint32_t n)
{
    uint32_t took;
    return fl_prog_ram(off, p, n, &took);
}
static void ldr_record_clear(void)
{
    fm1_updata_parm_clear();                        /* CRC 0: the SPL ignores it on a warm reset */
}
static void ldr_progress(uint32_t done, uint32_t total)
{
    (void)total;
    led(LED_RED | ((done >> 12) & 1u ? LED_GREEN : 0u));
    ldr_poll();
}
#include "ldr_core.c"

extern uint32_t _bss_start[], _bss_end[];

void ldr_main(void)
{
    fm1_wdt_arm(0x0D);                              /* ~8 s, fed by ldr_poll */
    fm1_time_init();
    FM1_PR(FM1_PA, FM1_DIE) |= LED_RED | LED_GREEN;
    FM1_PR(FM1_PA, FM1_DIR) &= ~(LED_RED | LED_GREEN);
    led(LED_RED);
    usb_start();
    for (;;) {
        ldr_poll();
        usb_retry(ldr_ms());
        if (usb.uboot_req) {                        /* soft key: the chip's USB bootloader */
            usb_detach();
            fm1_delay_ms(30);
            fm1_enter_uboot();
        }
        ota_service();                              /* handshake */
        if (usb.ota_req) {
            usb.ota_req = 0;
            if (ldr_session() == 0) {
                led(LED_GREEN);
                fm1_delay_ms(200);                  /* the "success" reply leaves */
                usb_detach();
                fm1_delay_ms(30);
                fm1_core_reset();                   /* the SPL boots the new app */
            }
            led(LED_RED);
        }
    }
}

void ldr_cstart(void)
{
    uint32_t *p;
    ldr_record_clear();                             /* first thing: a crash below cannot loop through a
                                                     * warm reset; resume after power loss uses the flash record */
    for (p = _bss_start; p < _bss_end; p++)
        *p = 0;
    ldr_main();
}
