# Galaxia hardware (board PD49 V03)

Everything here was checked on a real pedal (2026-10-30). Pin numbers use JieLi's scheme
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

## Controls

| Control | Pin | ADC channel | Notes |
|---|---|---|---|
| left-bottom knob | PB06 | 4 | |
| left-top knob | PB07 | 5 | |
| middle-right knob | PC00 | 6 | |
| right-top knob | PC01 | 7 | |
| selector | PC09 or PC10 | 8 | continuous reading; quantise to its detents in firmware |
| middle-bottom knob | PC10 or PC09 | 9 | |
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
| Format | signed 24-bit samples in 32-bit words, interleaved L/R |
| Measured | USB tone -> outputs -> cables -> inputs -> USB: channels correct, ~75 dB separation, 2nd harmonic ~-65 dB; input noise ~-98 dBFS |

## Not mapped yet

Bluetooth (the chip has a radio; whether the board has an antenna is unknown), the other jacks'
switches, power and battery sensing (there is no battery), which ADC channel is PC09 vs PC10.
