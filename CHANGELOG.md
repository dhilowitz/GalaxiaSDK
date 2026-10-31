# Changelog

## 0.1.0 (2026-10-31)

The first release.

- **Board support** for the M-VAVE Galaxia (board PD49 V03) on felucca-bsp: the audio link to its
  converter (stereo in and out, 44.1 kHz), the five knobs and the nine-position selector, both
  footswitches, the input jack detects and the two-colour LED. Every pin checked on a real pedal.
- **Runtime**: USB MIDI, USB audio both ways and a serial console; the watchdog, a fault report, and a
  boot guard that drops a firmware that cannot start into the bootloader. Five hooks for an effect.
- **Examples**: `passthrough`, `reverb` (an 8-line feedback delay network) and `diag` (audio link checks).
- **Tools**: back up, install, restore and restart firmware over USB through the chip's ROM bootloader
  (Linux); fetch JieLi's toolchain. No M-VAVE or JieLi files ship: images reuse the pedal's own boot
  loader and settings from the user's backup.
- **Measured** on a pedal through an audio interface: both channels clean in both directions.

Fixed before release: the audio double buffer used the FM-1's half flag (a 172 Hz chop on input and
output); the boot guard misread random memory after power-up, so a firmware did not start from
power-off; the selector's unevenly spaced detents.
