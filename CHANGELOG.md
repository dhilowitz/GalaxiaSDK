# Changelog

## Unreleased

- **Build on macOS**: `build.sh` runs itself in a `linux/amd64` Docker container where JieLi's toolchain cannot
  run (Docker Desktop, OrbStack or Colima); `get_toolchain.sh` works on macOS. Tested on an Apple silicon Mac
  with Colima and Rosetta: every example and the update loader identical to the Linux builds.

- **Updates over USB MIDI** (experimental; works on a pedal: reverb and Colorways installed this way, about half a minute each): SDK firmware answers the update protocol
  of M-VAVE's own updater (felucca-bsp's `chip/ota.c`); `tools/gx_pkg.py` makes a package, `tools/gx_update.py`
  installs it with no bootloader or sudo. A Galaxia update loader (`loader/gx_loader.c`, from felucca-bsp's) is
  built with every example. The firmware's identity is `SDK-Galaxia_900` (`GX_UPDATE_ID`); console `d` makes the
  next update a dry run. Checked on the desktop: felucca-bsp's device code, fed a package, leaves the flash
  byte for byte as `gx_flash.py install` does (`tests/update_test.py` covers the host side).
- **Install from macOS**: `gx_flash.py` drives the bootloader over libusb on a Mac (run with sudo; macOS's
  disk driver holds the device otherwise). Backup and install tested on an Apple silicon Mac. Files
  written under sudo belong to the user who ran it.

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
