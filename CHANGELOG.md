# Changelog

## Unreleased

- **Install over M-VAVE's firmware, and go back to it**: packages now carry a head built from JieLi's AC79 SDK
  `uboot.boot` (`gx_pkg.py sdk-files` takes it and `cfg_tool.bin` from any Felucca release), which M-VAVE's firmware
  checks before an update: so `gx_update.py` and the installer page can put an SDK firmware on a pedal still running
  M-VAVE's firmware, with no bootloader, backup or sudo. M-VAVE's official packages (`Galaxia.fwsc`, `ELEMENTAL PRO.fwsc`,
  known by their SHA-256) go back on the same way. Tested on an Elemental Pro, M-VAVE's delay on the same board, which
  takes the same firmware. `tools/gx_install_page.py` makes the installer page with firmwares built in.
- **Finding the pedal**: `gx_update.py` and the installer page look only for ports named "Galaxia" by default. If a
  firmware's USB name leaves that out, use `--port NAME` or the page's port field; `GX_PORTS=NAME` keeps `gx_update.py` finding
  it when you install from one firmware to another.
- **Install from a web page (experimental)**: `docs/installer/` does what `gx_update.py` does, from Chrome or Edge over
  Web MIDI. `tests/web_update_test.mjs` runs it against a simulated pedal and checks its bytes against the Python tool.
- **The second core (experimental)**: `#define GX_CPU1 1` before including the runtime starts the chip's second
  core at power-on (felucca-bsp's `fm1_cpu1.h`, from Melodee); `fm1_cpu1_run(fn, arg)` hands it a job and
  `fm1_cpu1_wait()` waits for it. A misread, a fault or a late job retires it and the firmware goes on with one
  core (`FM1_CPU1_FALLBACK 1`). `examples/cpu1bench` measures it: on a Galaxia, a filter load split across the two
  cores ran 1.95 times as fast (64 biquads a channel: 41.5 % of the audio block on one core, 21.3 % on two), and
  in verify mode about 41,000 jobs under full load with USB audio running matched CPU0's results bit for bit.
- **Polarity**: the Galaxia's input and output stages each invert, so the effect saw an inverted input, the USB
  recording was inverted, and audio played over USB came out of the jacks inverted (jack to jack it was upright).
  The runtime now undoes both: jack to jack is unchanged, and USB audio agrees with the jacks both ways.
- **No glitch on the first start after an update**: the firmware erases the staged update loader before the audio
  starts, not after. The erase (five flash sectors) stalled everything and cost an audio block on the first start
  after every update over MIDI.
- **USB recording holds at high load fixed**: the USB audio input keeps a whole audio half more in hand
  (felucca-bsp's `FELUCCA_UAC_DEEP`). An effect using more than about 60 % of the audio block ran it dry and the
  recording repeated frames, heard as crackle in the recording only (an effect at 71 %: some 800 holds in
  25 s). It costs 2 KB of RAM and 2.9 ms more latency to the computer.
- **Example effects run from flash**: the multifx effects and the reverb example no longer put their code in RAM
  (`.ram_text`), which measured slower for every one of them, the code's fetches competing with the effect's own
  reads of its buffers. With a 440 Hz tone in, the whole pedal's load went from 27 % to 16 % on the plate, 12 % to
  7 % on the chorus, 9 % to 6 % on the delay; the reverb example from 13 % to 12 %.
- **Saved settings** (`gx_settings_load`, `gx_settings_save`): up to 26 bytes kept across power-off and updates
  over MIDI, written a second after the last change in steps short enough not to cost any audio.

- **USB audio input no longer choppy**: the audio interrupt now runs below TIMER5, whose USB service nests in the
  render (as Felucca does). Before, a heavy effect blocked it long enough to miss USB frames: 13 % of packets
  at the plate reverb's 27 % load, heard as choppy audio and a growing delay. Now none missed; Mac to pedal to
  Mac measured at 8.9 ms with no gaps. The status line shows the USB audio input's counters while a computer
  records.
- **Knob hysteresis** (`gx_knob_steady` in `gx_board.h`): a knob left alone gives a steady value. A knob's
  reading wanders by a few counts, and a delay time that follows it warbled. The reverb and multifx examples
  use it.

- **USB audio on macOS 13-15**: felucca-bsp updated to cd76d43 for `FELUCCA_UAC_MIDI_LAST` (the audio streaming
  interfaces right after audio control, MIDI last; Felucca #67); the runtime turns it on, with the streams first
  in the collection. The pedal now appears in macOS 15.8 as a 2-in, 2-out audio device (it did not before).
  Two apps can still keep it away: Chrome holding the device open, and Focusrite Control's background service.

- **Example `multifx`**: nine effects on the selector (digital delay, tape echo, ping-pong delay, chorus,
  flanger, phaser, tremolo/auto-pan, plate reverb after Dattorro, lo-fi), one file each, sharing one memory
  arena; tap tempo, bypass, MIDI CCs, console overrides. `tests/mfx_render.c` renders them on a computer and
  checks every effect at its knob extremes. On a pedal: 5 to 27 % of the audio block, none late.

- **Build on macOS**: `build.sh` runs itself in a `linux/amd64` Docker container where JieLi's toolchain cannot
  run (Docker Desktop, OrbStack or Colima); `get_toolchain.sh` works on macOS. Tested on an Apple silicon Mac
  with Colima and Rosetta: every example and the update loader identical to the Linux builds.

- **Updates over USB MIDI** (experimental; works on a pedal: the reverb and a second firmware installed this way, about half a minute each): SDK firmware answers the update protocol
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
