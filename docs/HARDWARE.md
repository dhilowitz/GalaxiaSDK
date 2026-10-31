# Galaxia hardware (board PD49 V03)

> **Use at your own risk.** Unofficial notes from one pedal, provided as is, with no warranty: other
> board revisions may differ, and driving a pin the wrong way can damage hardware. Not affiliated with
> or endorsed by M-VAVE or JieLi.

Everything here was checked on a real pedal (2026-10-30 and 31). Pin numbers use JieLi's scheme
(pin = port x 16 + bit; GPIO block at 0x50000 + 0x40 x port: OUT +0, IN +4, DIR +8, DIE +12, PU +16, PD +20).

## Chip and memory

| | |
|---|---|
| SoC | JieLi WL82 / AC791N family, LQFP48 (marked C188612-11B8); chip key 0x980F |
| CPU | pi32v2 with a single-precision FPU (`-mcpu=r3 -mfprev1`); 24 MHz crystal |
| Flash | 1 MiB SPI NOR (JEDEC 0x856014), in the package; executes in place at 0x02000000 |
| RAM | 0x01C00000-0x01C7FFFF; with the runtime: 24 KiB RAM code window, 432 KiB for data and buffers, two 24 KiB stacks |
| Boot | ROM -> the boot loader in flash (0x0000-0x3FFF, with its settings) -> app at 0x02000120 |
| Bootloader | ROM USB loader, "WL82 UBOOT1.00" (4c4a:8057): entered by the soft key SysEx F0 22 24 35 7D F7, or by the request "usb_update_mode" at 0x01C7FD80 and a PMU reset |
| Flash layout | 0x0000-0x3FFF boot head (never written); 0x4000-0x7BFFF app area (the app, then its VM and BTIF regions); 0x7C000-0xFEFFF user region, blank on a stock pedal; 0xFF000 key area. Updates over MIDI rewrite 0x4000-0x92FFF (blank above 0x7C000) and stage their loader at 0xE0000-0xE4FFF: a firmware keeps its own data out of both |
| Update record | 112 bytes at 0x01C7FD88 (CRC16 over 78 bytes from +2, "AT" at +6, the loader's flash address at +72): the boot loader then loads the update loader to 0x01C0A800. Its code also scans the flash for a flash copy (4 KiB boundary minus 256 bytes, the "AT" marker), but a copy at 0xE4F00 or 0x7AF00 was ignored after a power cycle (2026-10-31): the scan seems to run only on some other failure |

## Controls

The top panel has two rows of three: top-left knob, selector, top-right knob; bottom-left,
bottom-middle and bottom-right knobs. Every knob reads 0 fully anticlockwise and 1023 fully clockwise.

| Control | Pin | ADC channel | Notes |
|---|---|---|---|
| bottom-left knob | PB06 | 4 | |
| top-left knob | PB07 | 5 | |
| bottom-right knob | PC00 | 6 | |
| top-right knob | PC01 | 7 | |
| selector | PC09 or PC10 | 8 | nine detents, unevenly spaced in the reading: use `gx_selector_position()` |
| bottom-middle knob | PC10 or PC09 | 9 | |
| left footswitch | PA01 | | low while pressed; pull-up |
| right footswitch | PA00 | | low while pressed; pull-up |
| left input jack | PC05 | | low with a plug in (PC02 also changes) |
| right input jack | PC06 | | low with a plug in |
| LED | PA02 red, PA03 green | | active low; both = yellow |

The output jacks have no detect switch. The channel-to-knob order is measured; which of PC09/PC10 is
channel 8 is not.

## Audio

| | |
|---|---|
| Link | ALNK0 (I2S, 0x12E00, IRQ 11) on the port A group: IOMAP_CON5 (0x51030) bits 6-7 = 1 |
| Pins | PA04 MCLK, PA05 SCLK, PA06 LRCLK, PA07 data out (lane 0, to the DAC), PA08 data in (lane 1, from the ADC) |
| Converter | external stereo codec; held in reset on PA09 until the clocks run; no other set-up needed |
| Clock | 11.2896 MHz MCLK (256 fs): 44,118 Hz nominal 44.1 kHz |
| Format | signed 24-bit samples in the low 24 bits of 32-bit words, interleaved L/R (received words are not sign-extended) |
| Double buffer | ALNK_CON0 bits 12-15 show each lane's current DMA half; the free half is the other one. Lanes 0 and 1 use bits 12 and 13 (felucca-bsp's FM-1 code reads bit 15, its lane 3: on the Galaxia that bit never changes) |
| Measured | Through an audio interface (Scarlett 2i2) at about -32 dBFS: both outputs and both inputs clean (distortion and spurious tones below the noise, 60 dB down), L to L and R to R, no crosstalk above the noise (> 60 dB); `examples/passthrough` within 1 dB end to end. Input noise ~-98 dBFS. Turn an interface's direct monitoring off for such tests: it feeds the outputs back in |

## Debugging notes

- The ROM bootloader's memory read returns scrambled data, and the USB loader that `gx_flash.py` uploads
  (to 0x01C02000) overwrites RAM up to about 0x01C08000, and also around 0x01C7C180. Read RAM through
  the loader. The runtime leaves its boot progress at 0x01C7D000 (magic "GXDB", start count, stage,
  reset reasons), which survives both.
- RAM is random after power-up, and the P33 reset source says so (bit 0 power-on, bit 1 VDDIO came up):
  anything kept in `.noinit` across resets must be checked against both, and declared volatile so the
  compiler cannot shrink a magic word.
- The first restart after installing from the stock firmware can return to the bootloader: its soft key
  leaves "usb_update_mode" at 0x01C7FD80. `gx_flash.py` clears it before restarting.
- On a Linux desktop, PulseAudio or PipeWire may hold the pedal's USB audio; record through it
  (`parecord`, `pw-record`) or use `hw:` devices on a machine without a sound server.

## Not mapped yet

Bluetooth (the chip has a radio; whether the board has an antenna is unknown), the other jacks'
switches, power and battery sensing (there is no battery), which ADC channel is PC09 vs PC10.
